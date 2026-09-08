// 后端总生命周期：按依赖顺序初始化存储、配置、通道、轮询、MQTT 和时间监测，并逆序停止。
#include "application/service/backend_service.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>
#include "shared/common/filesystem_compat.h"
#include "shared/common/logger.h"
#include "shared/common/readable_error.h"
#include "shared/common/time_utils.h"
#include "data/datastore/database_paths.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

namespace {

constexpr auto kPendingEventRetryInterval = std::chrono::seconds(5);


// 返回配置变更被阻止时的稳定错误文本。
const char* config_mutation_blocked_message()
{
    return "当前系统正在采集，不能直接重新加载配置；请通过配置保存流程自动应用到运行态";
}

}  // namespace

BackendService::~BackendService()
{
    modbus_tcp_server_.stop();
    stop_data_maintenance();
    stop_time_jump_monitor();
    if (polling_runtime_.get() != nullptr) {
        polling_runtime_.get()->stop();
    }
    time_runtime_.shutdown();
    mqtt_publisher_service_.stop();
    channel_manager_.close_all();
}

// 初始化后端配置、SQLite 存储、模板存储和运行态拓扑。
StatusCode BackendService::initialize(const std::string& data_directory)
{
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (config_apply_in_progress_.load()) {
        const std::string message = "配置正在应用中，请稍后重试";
        set_last_error("initialize", "", message, time_utils::system_now_ms());
        return StatusCode::kInvalidState;
    }
    if (is_polling_running_locked()) {
        const auto now_ms = time_utils::system_now_ms();
        const std::string message = config_mutation_blocked_message();
        set_last_error("initialize", "", message, now_ms);
        Logger::warn(message);
        return StatusCode::kInvalidState;
    }
    const DatabasePaths database_paths(data_directory);
    std::string old_database_error;
    const auto old_database_status = database_paths.reject_old_single_database(&old_database_error);
    if (!is_ok(old_database_status)) {
        Logger::error(old_database_error);
        return old_database_status;
    }

    // 三个数据库分别由 ConfigStore、HistoryStore、EventStore 管理 schema 和 user_version。
    std::string config_store_error;
    const auto config_store_status = config_store_.initialize(database_paths.config_database(), &config_store_error);
    if (!is_ok(config_store_status)) {
        Logger::error("配置 SQLite 存储初始化失败：" + config_store_error);
        return config_store_status;
    }
    std::string web_auth_error;
    const auto web_auth_status = config_store_.load_or_initialize_web_users(&web_auth_error);
    if (!is_ok(web_auth_status)) {
        Logger::error("Web 账户初始化失败：" + web_auth_error);
        return web_auth_status;
    }
    std::string template_error;
    const auto template_status = device_template_store_.initialize(
        database_paths.config_database(),
        &template_error);
    if (!is_ok(template_status)) {
        Logger::error("设备模板 SQLite 存储初始化失败：" + template_error);
        return template_status;
    }
    std::string alarm_store_error;
    const auto alarm_store_status = alarm_store_.initialize(database_paths.config_database(), &alarm_store_error);
    if (!is_ok(alarm_store_status)) {
        Logger::error("告警 SQLite 存储初始化失败：" + alarm_store_error);
        return alarm_store_status;
    }
    const auto history_status = history_store_.initialize(database_paths.history_database());
    if (!is_ok(history_status)) {
        Logger::error("历史 SQLite 存储初始化失败，路径=" + database_paths.history_database());
        return history_status;
    }
    std::string event_store_error;
    const auto event_store_status = event_store_.initialize(database_paths.events_database(), &event_store_error);
    if (!is_ok(event_store_status)) {
        Logger::error("事件 SQLite 存储初始化失败：" + event_store_error);
        return event_store_status;
    }
    DataMaintenanceSummary maintenance_summary;
    std::string maintenance_error;
    const auto maintenance_status = cleanup_expired_data(&maintenance_summary, &maintenance_error);
    if (!is_ok(maintenance_status)) {
        Logger::warn("启动数据维护失败，采集服务仍将继续启动：" + maintenance_error);
    }
    Logger::info(
        "SQLite 多库初始化完成：config=" + database_paths.config_database() +
        "，history=" + database_paths.history_database() +
        "，events=" + database_paths.events_database());

    std::vector<std::string> load_errors;
    const auto load_status = load_config_internal(&load_errors);
    if (!is_ok(load_status)) {
        const auto detail = load_errors.empty() ? "请检查 edge-config.db 和配置初始化逻辑" : load_errors.front();
        Logger::error("加载配置失败：" + detail);
        return load_status;
    }
    // 北向 Modbus 运行时故障与后端主流程隔离：配置/监听失败只记录诊断，不阻止南向轮询。
    bool modbus_runtime_prepared = false;
    ModbusServerSettings modbus_settings;
    std::string modbus_error;
    const auto modbus_settings_status = config_store_.load_or_initialize_modbus_server_settings(
        &modbus_settings, &modbus_error);
    std::vector<ModbusRegisterMapping> modbus_mappings;
    const auto modbus_mappings_status = config_store_.load_modbus_register_mappings(
        &modbus_mappings, &modbus_error);
    if (is_ok(modbus_settings_status) && is_ok(modbus_mappings_status)) {
        auto bank = std::make_shared<ModbusRegisterBank>();
        const auto configure_status = modbus_export_service_->configure(modbus_mappings, bank, &modbus_error);
        if (is_ok(configure_status)) {
            modbus_export_service_->backfill_device_statuses(data_store_.get_all_device_statuses());
            modbus_register_bank_ = std::move(bank);
            modbus_server_settings_ = modbus_settings;
            modbus_register_mappings_ = std::move(modbus_mappings);
            modbus_runtime_prepared = true;
        } else {
            Logger::warn("Modbus Register Bank 初始化失败，南向采集继续：" + modbus_error);
        }
    } else {
        Logger::warn("加载 Modbus Server 配置或映射失败，南向采集继续：" + modbus_error);
    }
    std::string alarm_error;
    // 告警判定器需要当前拓扑上下文来校验规则目标，并在拓扑失效时自动清理规则。
    const auto alarm_status = alarm_evaluator_.initialize(
        &alarm_store_, build_alarm_point_contexts_locked(),
        [this](ServiceEvent event) {
            (void)event;
            data_maintenance_wakeup_.notify_all();
        }, &alarm_error, [this] { return event_persistence_suppressed_.load(); });
    if (!is_ok(alarm_status)) {
        Logger::error("告警判定服务初始化失败：" + alarm_error);
        // load_config_internal 已经发布运行态并可能启动 MQTT/打开 TCP 通道；初始化失败必须立即回收。
        mqtt_publisher_service_.stop();
        channel_manager_.close_all();
        initialized_ = false;
        return alarm_status;
    }
    mqtt_publisher_service_.set_snapshot_provider([this]() {
        return data_store_.get_mqtt_realtime_snapshot();
    });
    std::string mqtt_error;
    const auto mqtt_status = mqtt_publisher_service_.start(&mqtt_error);
    if (!is_ok(mqtt_status) && !mqtt_error.empty()) {
        Logger::warn("MQTT 北向服务启动失败：" + mqtt_error);
    }
    std::string monitor_error;
    const auto monitor_status = start_time_jump_monitor(&monitor_error);
    if (!is_ok(monitor_status)) {
        Logger::error(monitor_error);
        mqtt_publisher_service_.stop();
        channel_manager_.close_all();
        initialized_ = false;
        return monitor_status;
    }
    std::string maintenance_thread_error;
    const auto maintenance_thread_status = start_data_maintenance(&maintenance_thread_error);
    if (!is_ok(maintenance_thread_status)) {
        Logger::error(maintenance_thread_error);
        stop_time_jump_monitor();
        mqtt_publisher_service_.stop();
        channel_manager_.close_all();
        initialized_ = false;
        return maintenance_thread_status;
    }
    // 所有会令 initialize() 返回失败的必要初始化均已完成后，才真正开放北向监听。
    if (modbus_runtime_prepared) {
        const auto server_status = modbus_tcp_server_.start(
            modbus_server_settings_, modbus_register_bank_, &modbus_error);
        if (!is_ok(server_status)) {
            Logger::warn("Modbus TCP Server 启动失败，南向采集继续：" + modbus_error);
        }
    }
    return StatusCode::kOk;
}

// 重新加载配置并刷新当前运行态。
StatusCode BackendService::reload_config(std::vector<std::string>* errors)
{
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (config_apply_in_progress_.load()) {
        const std::string message = "配置正在应用中，请稍后重试";
        if (errors != nullptr) {
            errors->push_back(message);
        }
        set_last_error("reload_config", "", message, time_utils::system_now_ms());
        return StatusCode::kInvalidState;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    if (is_polling_running_locked()) {
        // 配置重载会重建通道和拓扑，运行中直接 reload 容易打断采集线程持有的对象。
        const std::string message = config_mutation_blocked_message();
        if (errors != nullptr) {
            errors->push_back(message);
        }
        set_last_error("reload_config", "", message, time_utils::system_now_ms());
        return StatusCode::kInvalidState;
    }
    const auto reload_status = reload_config_for_apply(lock, errors);
    if (is_ok(reload_status)) {
        return reload_status;
    }

    const auto detail =
        errors != nullptr && !errors->empty() ? errors->front() : "重新加载运行配置失败";
    std::string final_error;
    const auto final_status = fail_config_apply_locked(
        reload_status,
        "重新加载运行配置失败: " + detail,
        false,
        &final_error);
    if (errors != nullptr) {
        errors->push_back(final_error);
    }
    return final_status;
}

// 关闭后端服务并同步最终运行状态。
void BackendService::shutdown()
{
    // 从关闭入口开始阻止设置应用/映射重载，直至 initialized_ 已撤销。
    std::unique_lock<std::mutex> modbus_management_lock(modbus_management_mutex_);
    stop_data_maintenance();
    stop_time_jump_monitor();
    {
        std::lock_guard<std::mutex> adjustment_lock(time_adjustment_mutex_);
        last_time_adjustment_source_.clear();
        last_time_adjustment_after_ms_ = 0;
        last_time_adjustment_delta_ms_ = 0;
    }
    std::shared_ptr<PollingService> polling_to_stop;
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        polling_to_stop = detach_polling_service_locked("stopping", "轮询停止中");
    }
    // 先解除轮询回调，再停止北向线程；两种 join 都不持有 service_mutex_。
    modbus_tcp_server_.stop();
    PollingRuntime::stop(polling_to_stop);
    time_runtime_.shutdown();
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        merge_stopped_polling_service_locked(polling_to_stop.get(), "not_started", "后端服务已停止");
        // 先在服务锁内撤销对外可用状态，随后在锁外等待 MQTT 线程和通道 I/O 收尾。
        // 此时轮询 worker 已全部回收，不再有线程依赖 ChannelManager 中的 fd 生命周期。
        initialized_ = false;
        applied_time_settings_ = {};
        applied_time_settings_initialized_ = false;
        auto runtime_status = data_store_.get_system_status();
        runtime_status.polling_running = false;
        runtime_status.running = false;
        runtime_status.service_ready = false;
        runtime_status.stopped_at_ms = time_utils::system_now_ms();
        runtime_status.last_status_message = "后端服务已停止";
        data_store_.update_system_status(runtime_status);
    }
    // initialized_ 已撤销后，后续管理调用会在 service_mutex_ 内稳定返回 invalid_state。
    modbus_management_lock.unlock();
    mqtt_publisher_service_.stop();
    channel_manager_.close_all();
    history_store_.save_now();
    std::string alarm_error;
    if (!is_ok(alarm_evaluator_.save_now(&alarm_error))) {
        Logger::error("退出前刷新告警运行状态失败：" + alarm_error);
    }
    std::string event_error;
    if (!is_ok(event_store_.save_now(&event_error))) {
        Logger::error("退出前刷新历史事件失败：" + event_error);
    }
}

// 启动数据维护。
StatusCode BackendService::start_data_maintenance(std::string* error_message)
{
    std::lock_guard<std::mutex> lock(data_maintenance_thread_mutex_);
    if (data_maintenance_thread_.joinable()) return StatusCode::kOk;
    data_maintenance_stop_requested_ = false;
    try {
        data_maintenance_thread_ = std::thread([this]() { data_maintenance_loop(); });
    } catch (const std::system_error& error) {
        data_maintenance_stop_requested_ = true;
        if (error_message != nullptr) {
            *error_message = "创建数据维护线程失败：" + std::string(error.what());
        }
        return StatusCode::kInternalError;
    }
    return StatusCode::kOk;
}

// 停止数据维护。
void BackendService::stop_data_maintenance()
{
    {
        std::lock_guard<std::mutex> lock(data_maintenance_thread_mutex_);
        data_maintenance_stop_requested_ = true;
    }
    data_maintenance_wakeup_.notify_all();
    if (data_maintenance_thread_.joinable()) data_maintenance_thread_.join();
}

// 运行历史、事件和报警数据的周期维护循环。
void BackendService::data_maintenance_loop()
{
    std::unique_lock<std::mutex> lock(data_maintenance_thread_mutex_);
    auto next_cleanup_time = std::chrono::steady_clock::now() + std::chrono::hours(24);
    while (!data_maintenance_stop_requested_) {
        if (data_maintenance_wakeup_.wait_for(
                lock, kPendingEventRetryInterval, [this]() { return data_maintenance_stop_requested_; })) {
            break;
        }
        lock.unlock();
        retry_pending_events();
        if (std::chrono::steady_clock::now() >= next_cleanup_time) {
            DataMaintenanceSummary summary;
            std::string error_message;
            const auto status = cleanup_expired_data(&summary, &error_message);
            if (!is_ok(status)) {
                Logger::warn("每日数据维护失败，将在下一周期重试：" + error_message);
            }
            next_cleanup_time = std::chrono::steady_clock::now() + std::chrono::hours(24);
        }
        lock.lock();
    }
}

// 从存储加载配置并构建后端运行态。
StatusCode BackendService::load_config_internal(std::vector<std::string>* errors)
{
    PreparedRuntimeConfig prepared;
    const auto prepare_status = runtime_config_compiler_.prepare(&prepared, errors);
    if (!is_ok(prepare_status)) {
        return prepare_status;
    }

    apply_prepared_runtime_config_locked(std::move(prepared));
    return StatusCode::kOk;
}

// 记录后端最近一次错误摘要。
void BackendService::set_last_error(
    const std::string& source,
    const std::string& target_id,
    const std::string& message,
    TimestampMs timestamp_ms)
{
    if (message.empty()) {
        return;
    }

    const auto readable = build_readable_runtime_error(message);
    const auto error_code = DiagnosisErrorCode::kUnknownError;
    const auto level = source.find("channel") != std::string::npos
                           ? DiagnosisLevel::kChannel
                           : (source == "polling" ? DiagnosisLevel::kMaster : DiagnosisLevel::kSystem);
    const auto diagnosis = make_diagnosis(
        level,
        target_id,
        target_id,
        error_code == DiagnosisErrorCode::kModbusTimeout ? DiagnosisRunStatus::kOffline : DiagnosisRunStatus::kError,
        error_code,
        0,
        timestamp_ms,
        0);
    DiagnosisStatus effective_diagnosis = diagnosis;
    effective_diagnosis.message = message;
    if (!target_id.empty()) {
        const auto channel_status = data_store_.get_channel_status(target_id);
        if (channel_status.has_value() && channel_status->diagnosis.error_code != "NONE") {
            effective_diagnosis = channel_status->diagnosis;
        } else {
            const auto master_status = data_store_.get_master_status(target_id);
            if (master_status.has_value() && master_status->diagnosis.error_code != "NONE") {
                effective_diagnosis = master_status->diagnosis;
            } else {
                const auto device_status = data_store_.get_device_status(target_id);
                if (device_status.has_value() && device_status->diagnosis.error_code != "NONE") {
                    effective_diagnosis = device_status->diagnosis;
                }
            }
        }
    }

    const auto event_source = effective_diagnosis.level.empty() ? source : effective_diagnosis.level;
    const auto event_target = effective_diagnosis.target_id.empty() ? target_id : effective_diagnosis.target_id;
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_summary_.has_error = true;
        last_error_summary_.source = source;
        last_error_summary_.target_id = event_target;
        last_error_summary_.diagnosis = effective_diagnosis;
        last_error_summary_.message = effective_diagnosis.message;
        last_error_summary_.timestamp_ms =
            effective_diagnosis.last_error_time_ms != 0
                ? effective_diagnosis.last_error_time_ms
                : timestamp_ms;
    }
    // SQLite 持久化和 MQTT 发布都在 error_mutex_ 外执行，避免错误摘要读取被磁盘或网络链路阻塞。
    append_event(
        "error",
        event_source,
        event_target,
        effective_diagnosis.message,
        effective_diagnosis.suggestion.empty() ? readable.detail : effective_diagnosis.suggestion,
        timestamp_ms,
        effective_diagnosis);
}

// 追加一条后端运行事件。
void BackendService::append_event(
    const std::string& level,
    const std::string& source,
    const std::string& target_id,
    const std::string& summary,
    const std::string& detail,
    TimestampMs timestamp_ms,
    const DiagnosisStatus& diagnosis)
{
    if (event_persistence_suppressed_.load()) {
        return;
    }
    if (summary.empty() && detail.empty()) {
        return;
    }

    ServiceEvent event;
    event.timestamp_ms = timestamp_ms != 0 ? timestamp_ms : time_utils::system_now_ms();
    event.level = level.empty() ? "info" : level;
    event.source = source.empty() ? "system" : source;
    event.target_id = (event.source == "data_alarm" || event.source == "alarm_ack")
                          ? target_id
                          : (!diagnosis.target_id.empty() ? diagnosis.target_id : target_id);
    event.diagnosis = diagnosis;
    event.summary = summary.empty() ? detail : summary;
    event.detail = detail;

    ServiceEvent stored_event;
    std::string event_error;
    const auto status = event_store_.append(event, &event_error, &stored_event);
    if (!is_ok(status)) {
        if (!stored_event.event_id.empty()) {
            // 主事件已落盘、仅后置容量清理失败时仍按持久化成功发布，避免错误入队后丢失 MQTT 一次性语义。
            Logger::warn("历史事件已持久化，但后置维护失败：" + event_error);
            publish_persisted_event(stored_event);
            return;
        }
        Logger::error("历史事件持久化失败：" + event_error);
        if (event.source == "data_alarm" || event.source == "alarm_ack") {
            std::vector<ServiceEvent> events{std::move(event)};
            if (!is_ok(alarm_store_.apply_runtime_state_batch({}, {}, &event_error, &events)))
                Logger::error("保存告警待发送记录失败：" + event_error);
        }
        return;
    }
    if (stored_event.event_id.empty()) {
        return;
    }

    publish_persisted_event(stored_event);
}

// 周期搬运持久化告警事件，不重新执行状态机。
void BackendService::retry_pending_events()
{
    std::lock_guard<std::mutex> lock(alarm_delivery_mutex_);
    if (event_persistence_suppressed_.load()) return;
    std::vector<ServiceEvent> durable_events;
    std::string durable_error;
    if (is_ok(alarm_store_.pending_events(&durable_events, &durable_error))) {
        for (const auto& event : durable_events) deliver_alarm_event(event);
    } else {
        Logger::warn("读取告警待发送记录失败：" + durable_error);
    }

}


void BackendService::deliver_alarm_event(const ServiceEvent& event)
{
    ServiceEvent stored;
    std::string error;
    const auto status = event_store_.append(event, &error, &stored);
    if (!is_ok(status) && stored.event_id.empty()) {
        Logger::error("告警事件搬运失败，保留持久化待发送记录：" + error);
        return;
    }
    if (!mqtt_publisher_service_.deliver_durable_event(stored)) return;
    if (!is_ok(alarm_store_.acknowledge_event(event.event_id, &error)))
        Logger::warn("告警事件确认失败，下轮幂等重试：" + error);
}

// EventStore 成功是 MQTT 一次性发布的唯一入口，失败重试期间不会重复发布。
void BackendService::publish_persisted_event(const ServiceEvent& event)
{
    mqtt_publisher_service_.publish_event(event);
    if (event.source == "data_alarm") {
        mqtt_publisher_service_.publish_alarm(event);
    }
}

// 确保指定主站绑定的通道可用于即时读写操作。
StatusCode BackendService::ensure_channel_ready_for_master(
    const MasterNodeConfig& master_config,
    std::string* error_message)
{
    if (master_config.protocol != MasterProtocol::kModbusRtu &&
        master_config.protocol != MasterProtocol::kModbusTcp) {
        if (error_message != nullptr) {
            *error_message = "当前采集链路仅接受 Modbus RTU 或 Modbus TCP 主控";
        }
        return StatusCode::kInvalidState;
    }

    auto* channel = channel_manager_.get_channel(master_config.channel_id);
    if (channel == nullptr) {
        if (error_message != nullptr) {
            *error_message = "未找到通道: " + master_config.channel_id;
        }
        return StatusCode::kNotFound;
    }
    if (!channel->config().enabled) {
        if (error_message != nullptr) {
            *error_message = "通道未启用: " + channel->config().channel_id;
        }
        return StatusCode::kInvalidState;
    }
    if (!channel->is_open()) {
        const auto open_status = channel->open();
        refresh_channel_statuses();
        if (!is_ok(open_status)) {
            if (error_message != nullptr) {
                *error_message = !channel->status().last_error_message.empty()
                                     ? channel->status().last_error_message
                                     : "打开目标通道失败";
            }
            return open_status;
        }
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
