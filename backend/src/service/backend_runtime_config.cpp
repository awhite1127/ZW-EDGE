// 运行配置重建事务：先准备新拓扑和通道，成功后再替换旧运行态，并按需恢复轮询。
#include "service/backend_service.h"
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/filesystem_compat.h"
#include "common/logger.h"
#include "common/readable_error.h"
#include "common/time_utils.h"
#include "service/backend_service_internal.h"

#ifndef EDGE_CONTROLLER_PACKAGE_VERSION
#error "EDGE_CONTROLLER_PACKAGE_VERSION must be provided by the controller build"
#endif

namespace edge_controller {

namespace {

// 优先读取集中式部署变量，空值时使用正式安装目录。
std::string environment_or_default(const char* name, const char* fallback)
{
    const auto* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : fallback;
}

// 判断诊断列表中是否存在通道不可用警告。
bool has_unavailable_channels_warning(const SystemStatus& system_status)
{
    return system_status.last_status_message.find("不可用通道") != std::string::npos;
}

// SQLite 只覆盖持久化业务设置；进程版本和目录遵循构建、systemd 部署契约。
SystemConfig make_default_runtime_config()
{
    SystemConfig config;
    config.project_name = "edge-controller";
    config.project_version = EDGE_CONTROLLER_PACKAGE_VERSION;
    config.site_id = "default-site";
    config.data_directory = environment_or_default("EDGE_CONTROLLER_DATA_DIR", "/opt/edge-controller/data");
    config.log_directory = environment_or_default("EDGE_CONTROLLER_LOG_DIR", "/opt/edge-controller/log");
    config.log_level = LogLevel::kInfo;
    config.default_poll_interval_ms = 1000;
    config.max_channel_reconnect_interval_ms = 5000;
    return config;
}

// 汇总通道打开失败的诊断信息。
std::string summarize_channel_open_failures(const ChannelOpenSummary& summary)
{
    if (summary.failed_count == 0) {
        return {};
    }
    return "后端服务已启动，但存在不可用通道: " + std::to_string(summary.failed_count);
}

// 收集当前启用且参与采集的通道标识。
std::vector<ChannelId> enabled_collection_channel_ids(
    const SystemConfig& config,
    const TopologyManager& topology_manager)
{
    std::vector<ChannelId> channel_ids;
    std::unordered_set<ChannelId> seen;

    for (const auto& master : config.master_nodes) {
        if (!master.enabled ||
            (master.protocol != MasterProtocol::kModbusRtu &&
             master.protocol != MasterProtocol::kModbusTcp)) {
            continue;
        }

        const auto* channel = backend_internal::find_channel_config_in_list(config.channels, master.channel_id);
        if (channel == nullptr || !channel->enabled) {
            continue;
        }

        bool has_enabled_device = false;
        for (const auto* device : topology_manager.get_devices_by_master(master.master_id)) {
            if (device != nullptr && device->enabled) {
                has_enabled_device = true;
                break;
            }
        }
        if (!has_enabled_device || seen.find(master.channel_id) != seen.end()) {
            continue;
        }

        channel_ids.push_back(master.channel_id);
        seen.insert(master.channel_id);
    }

    return channel_ids;
}

// 按通道类型筛选通道标识列表。
std::vector<ChannelId> filter_channel_ids_by_type(
    const SystemConfig& config,
    const std::vector<ChannelId>& channel_ids,
    ChannelType channel_type)
{
    std::vector<ChannelId> filtered;
    for (const auto& channel_id : channel_ids) {
        const auto* channel = backend_internal::find_channel_config_in_list(config.channels, channel_id);
        if (channel != nullptr && channel->channel_type == channel_type) {
            filtered.push_back(channel_id);
        }
    }
    return filtered;
}

}  // namespace

// 设置配置应用过程中的系统运行状态。
void BackendService::set_config_apply_status_locked(
    const std::string& polling_state,
    const std::string& status_message)
{
    auto runtime_status = data_store_.get_system_status();
    runtime_status.polling_running = false;
    runtime_status.polling_state = polling_state;
    runtime_status.last_status_message = status_message;
    runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
    data_store_.update_system_status(runtime_status);
}

// 完成配置应用，并按需恢复采集轮询。
BackendService::ConfigApplyOutcome BackendService::finish_config_apply_locked(
    bool was_polling_running,
    const std::string& warning_message)
{
    ConfigApplyOutcome outcome;
    outcome.warning_message = warning_message;
    const auto current_runtime_status = data_store_.get_system_status();
    const std::string unavailable_warning =
        has_unavailable_channels_warning(current_runtime_status) ? current_runtime_status.last_status_message : "";

    // 配置保存后是否恢复轮询取决于新的运行态，而不是保存前的按钮状态。
    if (!has_enabled_collection_target_locked()) {
        outcome.message = "配置已保存，当前无有效采集目标，轮询保持停止";
        if (!unavailable_warning.empty()) {
            outcome.warning_message = unavailable_warning;
            append_event("warning", "config_apply", "", "配置已保存，但存在通道异常", unavailable_warning, time_utils::system_now_ms());
        }
        set_config_apply_status_locked("stopped", outcome.message);
        append_event("info", "config_apply", "", outcome.message, "", time_utils::system_now_ms());
        return outcome;
    }

    std::string start_error;
    set_config_apply_status_locked(
        "polling_rebuilding",
        was_polling_running ? "轮询重建中" : "轮询启动中");
    const auto start_status = start_polling_locked(&start_error);
    if (!is_ok(start_status)) {
        const auto detail = start_error.empty() ? "未知错误" : start_error;
        outcome.message = "配置已保存，但轮询启动失败: " + detail;
        set_config_apply_status_locked("fault", outcome.message);
        set_last_error("polling", "", outcome.message, time_utils::system_now_ms());
        return outcome;
    }

    outcome.polling_restarted = true;
    outcome.message = was_polling_running ? "配置已保存，轮询已重启" : "配置已保存，轮询已启动";
    if (!unavailable_warning.empty()) {
        outcome.warning_message = unavailable_warning;
        outcome.message += "；" + unavailable_warning;
        append_event("warning", "config_apply", "", outcome.message, unavailable_warning, time_utils::system_now_ms());
    } else {
        append_event("info", "config_apply", "", outcome.message, "", time_utils::system_now_ms());
    }

    auto runtime_status = data_store_.get_system_status();
    runtime_status.last_status_message = outcome.message;
    runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
    data_store_.update_system_status(runtime_status);
    return outcome;
}

// 处理配置应用失败状态，并按需恢复原轮询状态。
StatusCode BackendService::fail_config_apply_locked(
    StatusCode status,
    const std::string& message,
    bool was_polling_running,
    std::string* error_message,
    bool recovery_succeeded)
{
    std::string final_message = message.empty() ? "配置保存失败" : message;
    if (!recovery_succeeded) {
        backend_internal::append_message(&final_message, "旧配置或旧运行态未完整恢复");
        set_config_apply_status_locked("fault", final_message);
        set_last_error("polling", "", final_message, time_utils::system_now_ms());
    } else if (was_polling_running) {
        std::string restore_error;
        const auto restore_status = start_polling_locked(&restore_error);
        if (!is_ok(restore_status)) {
            backend_internal::append_message(
                &final_message,
                "恢复原轮询状态失败: " + (restore_error.empty() ? "未知错误" : restore_error));
            set_config_apply_status_locked("fault", final_message);
            set_last_error("polling", "", final_message, time_utils::system_now_ms());
        } else {
            backend_internal::append_message(&final_message, "旧运行态已恢复，原轮询已重新启动");
            auto runtime_status = data_store_.get_system_status();
            runtime_status.polling_running = true;
            runtime_status.polling_state = "running";
            runtime_status.last_status_message = final_message;
            runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
            data_store_.update_system_status(runtime_status);
        }
    } else {
        backend_internal::append_message(&final_message, "轮询保持停止");
        set_config_apply_status_locked(initialized_ ? "stopped" : "not_started", final_message);
    }
    set_last_error("config_apply", "", final_message, time_utils::system_now_ms());
    if (error_message != nullptr) {
        *error_message = final_message;
    }
    return status;
}

// 根据当前配置准备拓扑、通道和默认状态。
StatusCode BackendService::prepare_runtime_config(
    PreparedRuntimeConfig* prepared,
    std::vector<std::string>* errors)
{
    if (prepared == nullptr) {
        if (errors != nullptr) {
            errors->push_back("运行配置准备参数为空");
        }
        return StatusCode::kInvalidArgument;
    }

    PreparedRuntimeConfig next;
    auto next_config = make_default_runtime_config();
    // 先在临时对象中完成 SQLite 加载、设备推导、拓扑构建和通道初始化；全部成功后再切换到运行态。
    std::vector<ChannelConfig> sqlite_channels;
    std::string channels_error;
    const auto channels_status = config_store_.load_channels(&sqlite_channels, &channels_error);
    if (!is_ok(channels_status)) {
        if (errors != nullptr) {
            errors->push_back(channels_error.empty() ? "加载 SQLite channels 失败" : channels_error);
        }
        return channels_status;
    }
    next_config.channels = std::move(sqlite_channels);

    std::vector<MasterNodeConfig> sqlite_masters;
    std::string masters_error;
    const auto masters_status = config_store_.load_masters(&sqlite_masters, &masters_error);
    if (!is_ok(masters_status)) {
        if (errors != nullptr) {
            errors->push_back(masters_error.empty() ? "加载 SQLite masters 失败" : masters_error);
        }
        return masters_status;
    }
    next_config.master_nodes = std::move(sqlite_masters);

    SystemSettings sqlite_settings;
    std::string settings_error;
    const auto settings_status = config_store_.load_or_initialize_system_settings(
        &sqlite_settings,
        &settings_error);
    if (!is_ok(settings_status)) {
        if (errors != nullptr) {
            errors->push_back(settings_error.empty() ? "加载 SQLite system_settings 失败" : settings_error);
        }
        return settings_status;
    }
    next_config.settings = std::move(sqlite_settings);

    TimeSettings sqlite_time_settings;
    std::string time_settings_error;
    const auto time_settings_status = config_store_.load_or_initialize_time_settings(
        &sqlite_time_settings,
        &time_settings_error);
    if (!is_ok(time_settings_status)) {
        if (errors != nullptr) {
            errors->push_back(time_settings_error.empty() ? "加载 SQLite time_settings 失败" : time_settings_error);
        }
        return time_settings_status;
    }
    next_config.time_settings = std::move(sqlite_time_settings);

    NetworkSettings sqlite_network_settings;
    bool network_settings_explicitly_configured = false;
    std::string network_settings_error;
    const auto network_settings_status = config_store_.load_or_initialize_network_settings_state(
        &sqlite_network_settings,
        &network_settings_explicitly_configured,
        &network_settings_error);
    if (!is_ok(network_settings_status)) {
        if (errors != nullptr) {
            errors->push_back(
                network_settings_error.empty() ? "加载 SQLite network_settings 失败" : network_settings_error);
        }
        return network_settings_status;
    }
    next_config.network_settings = std::move(sqlite_network_settings);
    next_config.network_settings_explicitly_configured = network_settings_explicitly_configured;

    MqttSettings sqlite_mqtt_settings;
    std::string mqtt_settings_error;
    const auto mqtt_settings_status = config_store_.load_or_initialize_mqtt_settings(
        &sqlite_mqtt_settings,
        &mqtt_settings_error);
    if (!is_ok(mqtt_settings_status)) {
        if (errors != nullptr) {
            errors->push_back(
                mqtt_settings_error.empty() ? "加载 SQLite mqtt_settings 失败" : mqtt_settings_error);
        }
        return mqtt_settings_status;
    }
    next_config.mqtt_settings = std::move(sqlite_mqtt_settings);

    std::vector<DeviceConfig> synced_devices;
    // 设备不再由独立配置维护，而是由主控采集块和设备模板自动推导。
    const auto sync_status = sync_devices_for_configs_locked(next_config.master_nodes, &synced_devices, errors);
    if (!is_ok(sync_status)) {
        return sync_status;
    }
    std::vector<DeviceAlias> aliases;
    std::string alias_error;
    const auto alias_status = config_store_.load_device_aliases(&aliases, &alias_error);
    if (!is_ok(alias_status)) {
        if (errors != nullptr) {
            errors->push_back(alias_error.empty() ? "加载设备显示名称失败" : alias_error);
        }
        return alias_status;
    }
    std::unordered_map<DeviceId, std::string> alias_by_device;
    for (const auto& alias : aliases) {
        if (!alias.device_id.empty() && !alias.display_name.empty()) {
            alias_by_device[alias.device_id] = alias.display_name;
        }
    }
    for (auto& device : synced_devices) {
        const auto alias = alias_by_device.find(device.device_id);
        if (alias != alias_by_device.end()) device.device_name = alias->second;
    }
    next_config.devices = std::move(synced_devices);
    const auto synced_validation_status = validate_synced_auto_devices_locked(next_config, errors);
    if (!is_ok(synced_validation_status)) {
        return synced_validation_status;
    }

    std::string topology_error;
    const auto topology_status = next.topology_manager.build(next_config, &topology_error);
    if (!is_ok(topology_status)) {
        if (errors != nullptr && !topology_error.empty()) {
            errors->push_back(topology_error);
        }
        return topology_status;
    }

    std::string channel_error;
    const auto channel_init_status = next.channel_manager.initialize(next_config.channels, &channel_error);
    if (!is_ok(channel_init_status)) {
        if (errors != nullptr && !channel_error.empty()) {
            errors->push_back(channel_error);
        }
        return channel_init_status;
    }

    const auto active_collection_channel_ids =
        enabled_collection_channel_ids(next_config, next.topology_manager);
    const auto tcp_collection_channel_ids = filter_channel_ids_by_type(
        next_config,
        active_collection_channel_ids,
        ChannelType::kModbusTcp);
    const auto channel_open_status = next.channel_manager.open_enabled_channels(
        tcp_collection_channel_ids,
        &next.channel_open_summary);
    if (!is_ok(channel_open_status)) {
        if (errors != nullptr) {
            errors->push_back("通道管理器初始化失败");
        }
        return channel_open_status;
    }

    next.system_config = std::move(next_config);
    *prepared = std::move(next);
    return StatusCode::kOk;
}

// 在持锁状态下替换为新准备好的运行态配置。
void BackendService::apply_prepared_runtime_config_locked(
    PreparedRuntimeConfig&& prepared)
{
    // 到这里说明新配置已经完整准备成功，可以用一次性切换替换旧运行态。
    channel_manager_.close_all();
    std::unordered_set<ChannelId> retained_channel_ids;
    retained_channel_ids.reserve(prepared.system_config.channels.size());
    for (const auto& channel : prepared.system_config.channels) {
        retained_channel_ids.insert(channel.channel_id);
    }
    for (const auto& channel : system_config_.channels) {
        if (retained_channel_ids.find(channel.channel_id) == retained_channel_ids.end()) {
            communication_trace_store_.clear_channel(channel.channel_id);
        }
    }
    system_config_ = std::move(prepared.system_config);
    if (!applied_time_settings_initialized_) {
        // initialize 只加载持久化配置；启动恢复之前，已应用配置按当前干净板实际状态建立。
        applied_time_settings_ = time_runtime_.snapshot_applied_settings(system_config_.time_settings);
        applied_time_settings_initialized_ = true;
    }
    topology_manager_ = std::move(prepared.topology_manager);
    topology_manager_.rebind_system_config(system_config_);
    channel_manager_ = std::move(prepared.channel_manager);
    data_store_.initialize(system_config_);
    if (modbus_export_service_ != nullptr && modbus_export_service_->register_bank() != nullptr) {
        modbus_export_service_->backfill_device_statuses(data_store_.get_all_device_statuses());
    }
    synchronize_alarm_topology_locked("拓扑失效");
    std::string mqtt_error;
    const auto mqtt_configure_status = mqtt_publisher_service_.configure(
        system_config_.mqtt_settings,
        system_config_.settings,
        &mqtt_error);
    if (!is_ok(mqtt_configure_status) && !mqtt_error.empty()) {
        Logger::warn("MQTT 北向服务配置失败：" + mqtt_error);
    } else {
        const auto mqtt_start_status = mqtt_publisher_service_.start(&mqtt_error);
        if (!is_ok(mqtt_start_status) && !mqtt_error.empty()) {
            Logger::warn("MQTT 北向服务启动失败：" + mqtt_error);
        }
    }

    initialized_ = true;
    build_default_runtime_status();
    refresh_channel_statuses();

    const auto startup_warning = summarize_channel_open_failures(prepared.channel_open_summary);
    auto runtime_status = data_store_.get_system_status();
    if (!startup_warning.empty()) {
        runtime_status.last_status_message = startup_warning;
        Logger::warn(startup_warning);
        if (!prepared.channel_open_summary.failures.empty()) {
            const auto& failure = prepared.channel_open_summary.failures.front();
            set_last_error("channel_startup", failure.channel_id, failure.error_message, failure.attempt_time_ms);
        }
    } else {
        runtime_status.last_status_message = "后端服务就绪";
        runtime_status.diagnosis = make_normal_diagnosis(
            DiagnosisLevel::kSystem,
            "system",
            "系统",
            runtime_status.started_at_ms);
        {
            std::lock_guard<std::mutex> error_lock(error_mutex_);
            if (last_error_summary_.source == "channel_startup") {
                last_error_summary_ = {};
            }
        }
        Logger::info("后端服务已启动，所有启用通道均已就绪");
    }

    data_store_.update_system_status(runtime_status);
}

// 配置保存后重新加载运行态，并合并加载告警。
StatusCode BackendService::reload_config_for_apply(
    std::unique_lock<std::shared_mutex>& lock,
    std::vector<std::string>* errors)
{
    set_config_apply_status_locked("config_applying", "配置应用中");
    lock.unlock();
    PreparedRuntimeConfig prepared;
    const auto prepare_status = prepare_runtime_config(&prepared, errors);
    lock.lock();
    if (!is_ok(prepare_status)) {
        return prepare_status;
    }
    apply_prepared_runtime_config_locked(std::move(prepared));
    return StatusCode::kOk;
}

// 构造后端启动后的默认系统状态。
void BackendService::build_default_runtime_status()
{
    SystemStatus runtime_status{};
    runtime_status.config_loaded = initialized_;
    runtime_status.service_ready = initialized_;
    runtime_status.running = initialized_;
    runtime_status.polling_running = false;
    runtime_status.polling_state = initialized_ ? "stopped" : "not_started";
    runtime_status.started_at_ms = initialized_ ? time_utils::system_now_ms() : 0;
    runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
    runtime_status.last_status_message = initialized_ ? "后端服务就绪" : "后端服务尚未初始化";
    runtime_status.diagnosis = initialized_
                                   ? make_normal_diagnosis(DiagnosisLevel::kSystem, "system", "系统", runtime_status.started_at_ms)
                                   : make_diagnosis(
                                         DiagnosisLevel::kSystem,
                                         "system",
                                         "系统",
                                         DiagnosisRunStatus::kWarning,
                                         DiagnosisErrorCode::kPollingNotRunning,
                                         0,
                                         0,
                                         0);
    data_store_.update_system_status(runtime_status);
}

// 从通道管理器同步最新通道状态。
void BackendService::refresh_channel_statuses()
{
    for (const auto& status : channel_manager_.snapshot_statuses()) {
        data_store_.update_channel_status(status);
    }
}

// 查找通道配置。
const ChannelConfig* BackendService::find_channel_config(const ChannelId& channel_id) const
{
    for (const auto& channel : system_config_.channels) {
        if (channel.channel_id == channel_id) {
            return &channel;
        }
    }
    return nullptr;
}

// 查找主站配置。
const MasterNodeConfig* BackendService::find_master_config(const MasterNodeId& master_id) const
{
    for (const auto& master : system_config_.master_nodes) {
        if (master.master_id == master_id) {
            return &master;
        }
    }
    return nullptr;
}

// 查找设备配置。
const DeviceConfig* BackendService::find_device_config(const DeviceId& device_id) const
{
    for (const auto& device : system_config_.devices) {
        if (device.device_id == device_id) {
            return &device;
        }
    }
    return nullptr;
}

}  // namespace edge_controller
