// 主站配置写入与应用入口：验证通道/模板引用后持久化，并通过统一配置重载路径生效。
#include "service/backend_service.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "service/backend_service_internal.h"

namespace edge_controller {

using namespace backend_internal;

// 新增主站配置、同步自动设备并重新加载运行态。
StatusCode BackendService::create_master_config(
    const MasterNodeConfigUpdateRequest& request,
    MasterNodeConfigUpdateResult* result,
    std::string* error_message)
{
    std::unique_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少主控创建结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    const auto validation_status = validate_master_update_request(system_config_, request, true, error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    auto next_masters = system_config_.master_nodes;
    const auto next_master = build_updated_master_config(nullptr, request);
    next_masters.push_back(next_master);
    std::vector<DeviceConfig> derived_devices;
    const auto sync_status = sync_devices_for_master_configs_locked(next_masters, &derived_devices, error_message);
    if (!is_ok(sync_status)) {
        return sync_status;
    }

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }

    const auto save_status = write_sqlite_masters_and_reload_locked(
        lock,
        next_masters,
        was_polling_running,
        error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto* applied_master = find_master_config(request.master_id);
    if (applied_master == nullptr) {
        return fail_config_apply_locked(
            StatusCode::kInternalError,
            "主控配置已保存，但无法重新读取",
            was_polling_running,
            error_message);
    }

    const auto apply_outcome = finish_config_apply_locked(was_polling_running);

    result->master_id = applied_master->master_id;
    result->master_config = *applied_master;
    result->message = apply_outcome.message;
    result->warning_message = apply_outcome.warning_message;
    result->polling_restarted = apply_outcome.polling_restarted;
    return StatusCode::kOk;
}

// 更新主站配置、同步自动设备并重新加载运行态。
StatusCode BackendService::update_master_config(
    const MasterNodeConfigUpdateRequest& request,
    MasterNodeConfigUpdateResult* result,
    std::string* error_message)
{
    std::unique_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少主控更新结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    std::size_t target_index = 0;
    const MasterNodeConfig* current_master = nullptr;
    const auto find_status =
        require_master_config_locked(request.master_id, &current_master, &target_index, error_message);
    if (!is_ok(find_status)) {
        return find_status;
    }

    const auto validation_status = validate_master_update_request(system_config_, request, false, error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    auto next_masters = system_config_.master_nodes;
    const auto next_master = build_updated_master_config(current_master, request);
    next_masters[target_index] = next_master;
    std::vector<DeviceConfig> derived_devices;
    const auto sync_status = sync_devices_for_master_configs_locked(next_masters, &derived_devices, error_message);
    if (!is_ok(sync_status)) {
        return sync_status;
    }

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }

    const auto save_status = write_sqlite_masters_and_reload_locked(
        lock,
        next_masters,
        was_polling_running,
        error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto* applied_master = find_master_config(request.master_id);
    if (applied_master == nullptr) {
        return fail_config_apply_locked(
            StatusCode::kInternalError,
            "主控配置已保存，但无法重新读取",
            was_polling_running,
            error_message);
    }

    const auto apply_outcome = finish_config_apply_locked(was_polling_running);

    result->master_id = applied_master->master_id;
    result->master_config = *applied_master;
    result->message = apply_outcome.message;
    result->warning_message = apply_outcome.warning_message;
    result->polling_restarted = apply_outcome.polling_restarted;
    return StatusCode::kOk;
}

// 删除主站配置、同步自动设备并重新加载运行态。
StatusCode BackendService::delete_master_config(
    const MasterNodeId& master_id,
    MasterNodeConfigDeleteResult* result,
    std::string* error_message)
{
    std::unique_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少主控删除结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    std::size_t target_index = 0;
    const auto find_status = require_master_config_locked(master_id, nullptr, &target_index, error_message);
    if (!is_ok(find_status)) {
        return find_status;
    }
    auto next_masters = system_config_.master_nodes;
    next_masters.erase(next_masters.begin() + static_cast<std::ptrdiff_t>(target_index));
    std::vector<DeviceConfig> derived_devices;
    const auto sync_status = sync_devices_for_master_configs_locked(next_masters, &derived_devices, error_message);
    if (!is_ok(sync_status)) {
        return sync_status;
    }

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }

    const auto save_status = write_sqlite_masters_and_reload_locked(
        lock,
        next_masters,
        was_polling_running,
        error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto apply_outcome = finish_config_apply_locked(was_polling_running);

    result->master_id = master_id;
    result->message = apply_outcome.message;
    result->polling_restarted = apply_outcome.polling_restarted;
    return StatusCode::kOk;
}


// 保存主站配置后刷新运行态，必要时恢复采集。
StatusCode BackendService::write_sqlite_masters_and_reload_locked(
    std::unique_lock<std::shared_mutex>& lock,
    const std::vector<MasterNodeConfig>& masters,
    bool restore_polling,
    std::string* error_message)
{
    const auto previous_masters = system_config_.master_nodes;
    std::string write_error;
    lock.unlock();
    const auto write_status = config_store_.save_masters(masters, &write_error);
    lock.lock();
    if (!is_ok(write_status)) {
        return fail_config_apply_locked(
            write_status,
            config_write_error_message("配置保存失败: 写入 SQLite masters 失败", write_error),
            restore_polling,
            error_message);
    }

    std::vector<std::string> reload_errors;
    const auto reload_status = reload_config_for_apply(lock, &reload_errors);
    if (is_ok(reload_status)) {
        return StatusCode::kOk;
    }

    const auto reload_detail = reload_errors.empty()
        ? std::string("重新加载主控配置失败")
        : reload_errors.front();
    std::string rollback_error;
    lock.unlock();
    const auto rollback_status = config_store_.save_masters(previous_masters, &rollback_error);
    lock.lock();

    std::string failure_message = "主控配置应用失败：" + reload_detail;
    if (is_ok(rollback_status)) {
        append_message(&failure_message, "SQLite masters 已回滚，旧运行态保持不变");
    } else {
        append_message(
            &failure_message,
            config_write_error_message("回滚 SQLite masters 失败", rollback_error));
    }
    return fail_config_apply_locked(
        reload_status,
        failure_message,
        restore_polling,
        error_message,
        is_ok(rollback_status));
}

}  // namespace edge_controller
