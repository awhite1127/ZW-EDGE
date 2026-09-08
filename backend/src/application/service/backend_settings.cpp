// 处理系统设置、账号及恢复出厂相关服务操作。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "application/service/backend_service.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "shared/common/logger.h"
#include "shared/common/time_utils.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

using namespace backend_internal;

// 更新系统显示设置并保存到配置存储。
StatusCode BackendService::update_system_settings(
    const SystemSettingsUpdateRequest& request,
    SystemSettingsUpdateResult* result,
    std::string* error_message)
{
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少系统设置保存结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);

    SystemSettings next_settings;
    const auto validation_status = validate_system_settings_request(request, &next_settings, error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    std::string write_error;
    lock.unlock();
    const auto write_status = config_store_.save_system_settings(next_settings, &write_error);
    lock.lock();
    if (!is_ok(write_status)) {
        if (error_message != nullptr) {
            *error_message = write_error.empty() ? "保存系统设置到 SQLite 失败" : "保存系统设置到 SQLite 失败: " + write_error;
        }
        return write_status;
    }

    system_config_.settings = next_settings;
    result->settings = next_settings;
    result->message = "系统设置已保存";
    return StatusCode::kOk;
}

// 获取Web认证状态。
StatusCode BackendService::get_web_auth_status(
    WebAuthStatus* status,
    std::string* error_message)
{
    if (status == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 登录状态输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    return config_store_.get_web_auth_status(status, error_message);
}

// 获取首次启动管理员入口可用状态。
StatusCode BackendService::get_first_boot_admin_entry_available(
    bool* available,
    std::string* error_message) const
{
    return config_store_.load_first_boot_admin_entry_available(available, error_message);
}

// 消费首次启动管理员入口并返回操作结果。
StatusCode BackendService::consume_first_boot_admin_entry(
    bool* consumed,
    std::string* error_message)
{
    return config_store_.consume_first_boot_admin_entry(consumed, error_message);
}

// 验证Web登录。
StatusCode BackendService::verify_web_login(
    const WebLoginRequest& request,
    WebLoginResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 登录校验结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    WebLoginRequest normalized;
    normalized.username = trim_copy(request.username);
    normalized.password = request.password;
    return config_store_.verify_web_login(normalized, result, error_message);
}

// 修改Web密码。
StatusCode BackendService::change_web_password(
    const WebPasswordChangeRequest& request,
    WebPasswordChangeResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少账户密码修改结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    WebPasswordChangeRequest normalized;
    normalized.username = trim_copy(request.username);
    normalized.current_password = request.current_password;
    normalized.new_password = request.new_password;
    return config_store_.change_web_password(normalized, result, error_message);
}

// 设置Web只读用户密码。
StatusCode BackendService::set_web_viewer_password(
    const WebViewerPasswordSetRequest& request,
    WebPasswordChangeResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少只读用户密码设置结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    WebViewerPasswordSetRequest normalized;
    normalized.admin_password = request.admin_password;
    normalized.new_user_password = request.new_user_password;
    return config_store_.set_web_viewer_password(normalized, result, error_message);
}

// 列出Web用户。
StatusCode BackendService::list_web_users(
    std::vector<WebUserView>* users,
    std::string* error_message)
{
    return config_store_.list_web_users(users, error_message);
}

// 创建Web用户。
StatusCode BackendService::create_web_user(
    const WebUserCreateRequest& request,
    WebUserMutationResult* result,
    std::string* error_message)
{
    WebUserCreateRequest normalized;
    normalized.username = trim_copy(request.username);
    normalized.display_name = trim_copy(request.display_name);
    normalized.role = trim_copy(request.role);
    normalized.password = request.password;
    normalized.enabled = request.enabled;
    const auto status = config_store_.create_web_user(normalized, result, error_message);
    if (is_ok(status) && result != nullptr && result->success) {
        append_event("info", "user_management", normalized.username, "创建用户", "角色=" + normalized.role, time_utils::system_now_ms());
    }
    return status;
}

// 更新Web用户。
StatusCode BackendService::update_web_user(
    const WebUserUpdateRequest& request,
    WebUserMutationResult* result,
    std::string* error_message)
{
    WebUserUpdateRequest normalized;
    normalized.username = trim_copy(request.username);
    normalized.display_name = trim_copy(request.display_name);
    normalized.role = trim_copy(request.role);
    normalized.enabled = request.enabled;
    const auto status = config_store_.update_web_user(normalized, result, error_message);
    if (is_ok(status) && result != nullptr && result->success) {
        append_event(
            "info",
            "user_management",
            normalized.username,
            normalized.enabled ? "启用或修改用户" : "禁用用户",
            "角色=" + normalized.role,
            time_utils::system_now_ms());
    }
    return status;
}

// 重置指定 Web 用户密码并返回操作结果。
StatusCode BackendService::reset_web_user_password(
    const WebUserPasswordResetRequest& request,
    WebUserMutationResult* result,
    std::string* error_message)
{
    WebUserPasswordResetRequest normalized;
    normalized.username = trim_copy(request.username);
    normalized.new_password = request.new_password;
    const auto status = config_store_.reset_web_user_password(normalized, result, error_message);
    if (is_ok(status) && result != nullptr && result->success) {
        append_event("info", "user_management", normalized.username, "重置用户密码", "", time_utils::system_now_ms());
    }
    return status;
}

// 恢复出厂数据并重新初始化默认配置。
StatusCode BackendService::request_factory_reset(FactoryResetResult* result, std::string* error_message)
{
    std::unique_lock<std::mutex> modbus_management_lock(modbus_management_mutex_);
    std::lock_guard<std::mutex> time_operation_lock(time_service_mutex_);
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "恢复出厂数据结果参数为空";
        }
        return StatusCode::kInvalidArgument;
    }

    *result = {};
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) {
        const std::string message = "后端服务尚未初始化，无法恢复出厂数据";
        if (error_message != nullptr) {
            *error_message = message;
        }
        return StatusCode::kInvalidState;
    }
    if (config_apply_in_progress_.load()) {
        const std::string message = "配置正在应用中，请稍后再恢复出厂数据";
        if (error_message != nullptr) {
            *error_message = message;
        }
        return StatusCode::kInvalidState;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    std::unique_lock<std::mutex> alarm_delivery_guard(alarm_delivery_mutex_);
    const auto previous_modbus_settings = modbus_server_settings_;
    const auto previous_modbus_bank = modbus_register_bank_;
    const auto previous_modbus_mappings = modbus_register_mappings_;
    const bool was_polling_running = is_polling_running_locked();
    std::shared_ptr<PollingService> polling_to_stop;
    stop_polling_for_config_apply(lock, &polling_to_stop);

    SystemConfig reset_config = system_config_;
    const auto reset_settings = SystemSettings{};
    reset_config.settings = reset_settings;
    reset_config.channels.clear();
    reset_config.master_nodes.clear();
    reset_config.devices.clear();
    TimeSettings reset_time_settings;
    reset_time_settings.updated_at = time_utils::system_now_ms();

    std::string write_error;
    lock.unlock();
    // 先停止旧监听，确保恢复流程中不会继续暴露旧端口或旧地址映射。
    modbus_tcp_server_.stop();
    auto write_status = config_store_.save_system_settings(reset_settings, &write_error);
    if (is_ok(write_status)) {
        write_status = config_store_.save_mqtt_settings(MqttSettings{}, &write_error);
    }
    if (is_ok(write_status)) {
        // 仅恢复默认时间配置；本次恢复不改变当前系统时间、时区或 NTP 进程。
        write_status = config_store_.save_time_settings(reset_time_settings, &write_error);
    }
    if (is_ok(write_status)) {
        write_status = config_store_.save_masters(reset_config.master_nodes, &write_error);
    }
    if (is_ok(write_status)) {
        write_status = config_store_.save_channels(reset_config.channels, &write_error);
    }
    const ModbusServerSettings reset_modbus_settings;
    if (is_ok(write_status)) {
        write_status = config_store_.save_modbus_server_settings(reset_modbus_settings, &write_error);
    }
    if (is_ok(write_status)) {
        write_status = config_store_.replace_modbus_register_mappings({}, &write_error);
    }
    if (is_ok(write_status)) {
        write_status = config_store_.reset_web_user_passwords(&write_error);
    }
    std::shared_ptr<ModbusRegisterBank> reset_modbus_bank;
    if (is_ok(write_status)) {
        reset_modbus_bank = std::make_shared<ModbusRegisterBank>();
        write_status = modbus_export_service_->configure({}, reset_modbus_bank, &write_error);
    }
    if (is_ok(write_status)) {
        // enabled=false 的 start 不创建线程，同时会清空累计统计和最近错误。
        write_status = modbus_tcp_server_.start(reset_modbus_settings, reset_modbus_bank, &write_error);
    }
    if (!is_ok(write_status)) {
        StatusCode restore_failure = StatusCode::kOk;
        std::string restore_error;
        auto record_restore = [&](StatusCode status, const std::string& step) {
            if (is_ok(status)) return;
            if (is_ok(restore_failure)) restore_failure = status;
            backend_internal::append_message(
                &write_error,
                step + (restore_error.empty() ? std::string{} : "：" + restore_error));
            restore_error.clear();
        };
        const auto settings_restore_status =
            config_store_.save_modbus_server_settings(previous_modbus_settings, &restore_error);
        record_restore(settings_restore_status, "恢复原 Modbus Server 设置失败");
        const auto mappings_restore_status =
            config_store_.replace_modbus_register_mappings(previous_modbus_mappings, &restore_error);
        record_restore(mappings_restore_status, "恢复原 Modbus 映射失败");
        if (is_ok(settings_restore_status) && is_ok(mappings_restore_status)) {
            record_restore(
                modbus_tcp_server_.restart(previous_modbus_settings, previous_modbus_bank, &restore_error),
                "恢复原 Modbus TCP Server 运行状态失败");
        } else {
            // 持久化配置未完整恢复时继续保持监听关闭，禁止旧 Bank 与残缺数据库组合对外服务。
            modbus_tcp_server_.stop();
            backend_internal::append_message(&write_error, "旧 Modbus 监听保持关闭");
        }
        if (!is_ok(restore_failure)) {
            backend_internal::append_message(&write_error, "持久化配置与运行状态可能不一致");
            Logger::error("恢复出厂失败后的 Modbus 状态恢复未完整成功：" + write_error);
            write_status = restore_failure;
        }
    }
    lock.lock();
    if (!is_ok(write_status)) {
        std::string message = "恢复出厂数据失败: 写入默认配置失败";
        backend_internal::append_message(&message, write_error);
        return fail_config_apply_locked(
            write_status,
            message,
            was_polling_running,
            error_message,
            false);
    }

    std::vector<std::string> reload_errors;
    const auto reload_status = reload_config_for_apply(lock, &reload_errors);
    if (!is_ok(reload_status)) {
        const auto detail = !reload_errors.empty() ? reload_errors.front() : "恢复默认配置后重新加载失败";
        const std::string message = "恢复出厂数据失败: " + detail;
        return fail_config_apply_locked(
            reload_status,
            message,
            was_polling_running,
            error_message,
            false);
    }
    modbus_server_settings_ = reset_modbus_settings;
    modbus_register_mappings_.clear();
    modbus_register_bank_ = std::move(reset_modbus_bank);

    lock.unlock();
    std::vector<std::string> cleanup_errors;
    std::string cleanup_error;
    const auto history_status = history_store_.clear_all(&cleanup_error);
    if (!is_ok(history_status)) cleanup_errors.push_back("历史采集数据: " + cleanup_error);
    cleanup_error.clear();
    const auto event_status = event_store_.clear_all(&cleanup_error);
    if (!is_ok(event_status)) cleanup_errors.push_back("历史事件: " + cleanup_error);
    cleanup_error.clear();
    const auto rule_status = alarm_store_.clear_rules(&cleanup_error);
    if (!is_ok(rule_status)) cleanup_errors.push_back("告警配置: " + cleanup_error);
    cleanup_error.clear();
    const auto runtime_status = alarm_store_.clear_runtime_states(&cleanup_error);
    if (!is_ok(runtime_status)) cleanup_errors.push_back("告警状态: " + cleanup_error);
    cleanup_error.clear();
    if (!is_ok(alarm_store_.clear_event_outbox(&cleanup_error))) cleanup_errors.push_back("告警待发送事件: " + cleanup_error);
    std::string first_boot_entry_error;
    const auto first_boot_entry_status = config_store_.reset_first_boot_admin_entry(&first_boot_entry_error);
    if (!is_ok(first_boot_entry_status)) {
        lock.lock();
        const std::string message = "恢复出厂数据失败: 无法重置首次部署入口" +
            (first_boot_entry_error.empty() ? std::string{} : "；" + first_boot_entry_error);
        return fail_config_apply_locked(
            first_boot_entry_status,
            message,
            false,
            error_message,
            false);
    }
    lock.lock();
    if (cleanup_errors.empty()) {
        std::string alarm_reload_error;
        const auto alarm_reload_status = alarm_evaluator_.initialize(
            &alarm_store_, build_alarm_point_contexts_locked(),
            [this](ServiceEvent event) {
                (void)event;
                data_maintenance_wakeup_.notify_all();
            }, &alarm_reload_error, [this] { return event_persistence_suppressed_.load(); });
        if (!is_ok(alarm_reload_status)) cleanup_errors.push_back("告警缓存: " + alarm_reload_error);
    }
    std::string reset_message = "恢复出厂数据成功";
    std::string reset_status_message = "恢复出厂数据完成，等待重新配置";
    std::string reset_event_detail = "基础配置、时间配置、MQTT 与 Modbus 北向配置、Modbus 寄存器映射、Web 账户密码、历史事件、历史采集数据、告警配置、告警状态和最近错误已恢复出厂状态；当前系统时间未调整，默认时间设置将在后续启动时应用，网络配置和设备模板未修改";
    if (!cleanup_errors.empty()) {
        std::string cleanup_warning = "恢复出厂数据完成，但部分 SQLite 数据清理失败";
        for (const auto& item : cleanup_errors) backend_internal::append_message(&cleanup_warning, item);
        Logger::error(cleanup_warning);
        reset_message = cleanup_warning;
        reset_status_message = cleanup_warning;
        reset_event_detail = cleanup_warning + "；已成功清理的项目保持恢复出厂状态，网络配置和设备模板未修改";
    }

    {
        std::lock_guard<std::mutex> error_lock(error_mutex_);
        last_error_summary_ = {};
    }
    if (polling_to_stop != nullptr) {
        polling_to_stop->clear_last_error_summary();
    }

    set_config_apply_status_locked("stopped", reset_status_message);
    append_event(
        cleanup_errors.empty() ? "info" : "warning",
        "system_maintenance",
        "",
        "用户执行恢复出厂数据",
        reset_event_detail,
        time_utils::system_now_ms());

    result->reset_completed = true;
    result->message = reset_message;
    return StatusCode::kOk;
}

}  // namespace edge_controller
