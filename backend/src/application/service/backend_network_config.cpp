// 网络配置持久化入口：校验 static/DHCP 参数并委托平台运行时应用，不触碰采集拓扑。
#include "application/service/backend_service.h"

#include <mutex>
#include <string>

#include "application/service/backend_service_internal.h"
#include "application/service/network_config_policy.h"

namespace edge_controller {

using namespace backend_internal;

// 更新网络配置并保存到配置存储。
StatusCode BackendService::update_network_settings(
    const NetworkSettingsUpdateRequest& request,
    NetworkSettingsUpdateResult* result,
    std::string* error_message)
{
    std::lock_guard<std::mutex> network_operation_lock(network_operation_mutex_);
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少网络配置保存结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);

    NetworkSettings next_settings;
    const auto validation_status = backend_internal::canonicalize_network_settings_request(
        request,
        &system_config_.network_settings,
        &next_settings,
        error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    std::string write_error;
    lock.unlock();
    // 仅保存不代表用户已经验证运行态；确认状态必须由“保存并应用”成功或配置导入建立。
    const auto write_status = config_store_.save_network_settings_state(next_settings, false, &write_error);
    lock.lock();
    if (!is_ok(write_status)) {
        if (error_message != nullptr) {
            *error_message = write_error.empty() ? "保存网络配置失败" : "保存网络配置失败: " + write_error;
        }
        return write_status;
    }

    system_config_.network_settings = next_settings;
    system_config_.network_settings_explicitly_configured = false;
    result->settings = next_settings;
    result->message = "网络配置已保存";
    append_event(
        "info",
        "network_config",
        next_settings.interface_name,
        "网络配置已保存",
        "模式=" + next_settings.mode,
        0);
    return StatusCode::kOk;
}

// 保存并应用网络设置。
StatusCode BackendService::save_and_apply_network_settings(
    const NetworkSettingsUpdateRequest& request,
    NetworkApplyResult* result,
    std::string* error_message)
{
    // 从持久化写入到运行态应用、失败回滚使用同一网络操作锁，防止另一个 IPC
    // 在中途停止 DHCP、刷新地址或基于半完成状态执行回滚。
    std::lock_guard<std::mutex> network_operation_lock(network_operation_mutex_);
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少网络配置保存应用结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);

    NetworkSettings next_settings;
    const auto validation_status =
        backend_internal::canonicalize_network_settings_request(
            request,
            &system_config_.network_settings,
            &next_settings,
            error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    const auto previous_settings = system_config_.network_settings;
    const bool previous_explicitly_configured =
        system_config_.network_settings_explicitly_configured;

    std::string write_error;
    lock.unlock();
    const auto write_status = config_store_.save_network_settings_state(next_settings, true, &write_error);
    lock.lock();
    if (!is_ok(write_status)) {
        if (error_message != nullptr) {
            *error_message = write_error.empty() ? "保存网络配置失败" : "保存网络配置失败：" + write_error;
        }
        return write_status;
    }
    system_config_.network_settings = next_settings;
    system_config_.network_settings_explicitly_configured = true;
    lock.unlock();

    std::string apply_error;
    const auto apply_status = apply_network_settings_internal(false, result, &apply_error);
    if (is_ok(apply_status)) {
        return StatusCode::kOk;
    }

    std::string rollback_write_error;
    const auto rollback_write_status = config_store_.save_network_settings_state(
        previous_settings, previous_explicitly_configured, &rollback_write_error);
    lock.lock();
    if (is_ok(rollback_write_status)) {
        system_config_.network_settings = previous_settings;
        system_config_.network_settings_explicitly_configured = previous_explicitly_configured;
    }
    lock.unlock();

    NetworkApplyResult rollback_result;
    std::string rollback_apply_error;
    // 首次未确认状态的“原配置”只是数据库默认值，绝不能在失败回滚时把它应用到现场网口。
    // apply_network_settings_internal 已经按应用前运行态快照完成回滚，此处仅对已确认配置沿用旧恢复流程。
    const bool preserve_unconfirmed_runtime =
        network_config_policy::require_explicit_config() && !previous_explicitly_configured;
    const auto rollback_apply_status = !is_ok(rollback_write_status)
                                           ? StatusCode::kInvalidState
                                           : (preserve_unconfirmed_runtime
                                                  ? StatusCode::kOk
                                                  : apply_network_settings_internal(
                                                        false, &rollback_result, &rollback_apply_error));

    std::string message = apply_error.empty() ? "应用网络配置失败" : apply_error;
    if (!is_ok(rollback_write_status)) {
        message += "；恢复原数据库配置失败：" +
                   (rollback_write_error.empty() ? std::string("未知错误") : rollback_write_error);
    }
    if (!is_ok(rollback_apply_status)) {
        message += "；恢复原网络运行状态失败：" +
                   (rollback_apply_error.empty() ? std::string("未知错误") : rollback_apply_error);
    } else if (preserve_unconfirmed_runtime) {
        message += "；未应用未确认的默认配置，运行态由失败应用流程恢复";
    } else {
        message += "；已恢复原网络配置";
    }
    result->error_message = message;
    result->message = message;
    if (error_message != nullptr) {
        *error_message = message;
    }
    return apply_status;
}

}  // namespace edge_controller
