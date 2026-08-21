// 通道配置写入与应用入口；配置变更需要重建相关运行态，失败时返回可诊断的部分应用结果。
#include "service/backend_service.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "service/backend_service_internal.h"

namespace edge_controller {

using namespace backend_internal;

// 新增通道配置并重新加载运行态。
StatusCode BackendService::create_channel_config(
    const ChannelConfigUpdateRequest& request,
    ChannelConfigUpdateResult* result,
    std::string* error_message)
{
    std::unique_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少通道创建结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    const auto channel_id = trim_copy(request.channel_id);
    const auto id_status = ensure_channel_id_available_locked(channel_id, error_message);
    if (!is_ok(id_status)) {
        return id_status;
    }

    std::vector<SerialPortInfo> serial_ports;
    std::string serial_error;
    if (!is_ok(serial_port_enumerator_.enumerate(&serial_ports, &serial_error))) {
        serial_ports.clear();
    }

    std::string warning_message;
    const auto validation_status = validate_channel_update_request(
        system_config_.channels,
        request,
        serial_ports,
        &warning_message,
        error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    auto next_channels = system_config_.channels;
    next_channels.push_back(build_new_channel_config(request));

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }

    const auto save_status =
        write_channels_and_reload_locked(lock, next_channels, was_polling_running, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto* applied_channel = find_channel_config(channel_id);
    if (applied_channel == nullptr) {
        return fail_config_apply_locked(
            StatusCode::kInternalError,
            "通道配置已保存，但无法重新读取",
            was_polling_running,
            error_message);
    }

    const auto apply_outcome = finish_config_apply_locked(was_polling_running, warning_message);

    result->channel_id = applied_channel->channel_id;
    result->channel_config = *applied_channel;
    result->message = apply_outcome.message;
    result->warning_message = apply_outcome.warning_message;
    result->polling_restarted = apply_outcome.polling_restarted;
    return StatusCode::kOk;
}

// 更新通道配置并重新加载运行态。
StatusCode BackendService::update_channel_config(
    const ChannelConfigUpdateRequest& request,
    ChannelConfigUpdateResult* result,
    std::string* error_message)
{
    std::unique_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少通道更新结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    std::size_t target_index = 0;
    const ChannelConfig* current_channel = nullptr;
    const auto find_status =
        require_channel_config_locked(request.channel_id, &current_channel, &target_index, error_message);
    if (!is_ok(find_status)) {
        return find_status;
    }

    std::vector<SerialPortInfo> serial_ports;
    std::string serial_error;
    if (!is_ok(serial_port_enumerator_.enumerate(&serial_ports, &serial_error))) {
        serial_ports.clear();
    }

    std::string warning_message;
    const auto validation_status = validate_channel_update_request(
        system_config_.channels,
        request,
        serial_ports,
        &warning_message,
        error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    auto next_channels = system_config_.channels;
    next_channels[target_index] = build_updated_channel_config(*current_channel, request);

    const auto reference_status = validate_channel_type_for_referencing_masters(
        system_config_.master_nodes,
        next_channels[target_index].channel_id,
        next_channels[target_index].channel_type,
        error_message);
    if (!is_ok(reference_status)) {
        return reference_status;
    }

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }

    const auto save_status =
        write_channels_and_reload_locked(lock, next_channels, was_polling_running, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto* applied_channel = find_channel_config(request.channel_id);
    if (applied_channel == nullptr) {
        return fail_config_apply_locked(
            StatusCode::kInternalError,
            "通道配置已保存，但无法重新读取",
            was_polling_running,
            error_message);
    }

    const auto apply_outcome = finish_config_apply_locked(was_polling_running, warning_message);

    result->channel_id = applied_channel->channel_id;
    result->channel_config = *applied_channel;
    result->message = apply_outcome.message;
    result->warning_message = apply_outcome.warning_message;
    result->polling_restarted = apply_outcome.polling_restarted;
    return StatusCode::kOk;
}

// 删除通道配置并重新加载运行态。
StatusCode BackendService::delete_channel_config(
    const ChannelId& channel_id,
    ChannelConfigDeleteResult* result,
    std::string* error_message)
{
    std::unique_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少通道删除结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);
    std::size_t target_index = 0;
    const auto find_status = require_channel_config_locked(channel_id, nullptr, &target_index, error_message);
    if (!is_ok(find_status)) {
        return find_status;
    }

    for (const auto& master : system_config_.master_nodes) {
        if ((master.protocol == MasterProtocol::kModbusRtu ||
             master.protocol == MasterProtocol::kModbusTcp) &&
            master.channel_id == channel_id) {
            if (error_message != nullptr) {
                *error_message = "通道仍被主控引用: " + master.master_id;
            }
            return StatusCode::kInvalidArgument;
        }
    }

    auto next_channels = system_config_.channels;
    next_channels.erase(next_channels.begin() + static_cast<std::ptrdiff_t>(target_index));

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }

    const auto save_status =
        write_channels_and_reload_locked(lock, next_channels, was_polling_running, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto apply_outcome = finish_config_apply_locked(was_polling_running);

    result->channel_id = channel_id;
    result->message = apply_outcome.message;
    result->polling_restarted = apply_outcome.polling_restarted;
    return StatusCode::kOk;
}


// 保存通道配置后刷新运行态，必要时恢复采集。
StatusCode BackendService::write_channels_and_reload_locked(
    std::unique_lock<std::shared_mutex>& lock,
    const std::vector<ChannelConfig>& channels,
    bool restore_polling,
    std::string* error_message)
{
    const auto previous_channels = system_config_.channels;
    std::string write_error;
    lock.unlock();
    const auto write_status = config_store_.save_channels(channels, &write_error);
    lock.lock();
    if (!is_ok(write_status)) {
        return fail_config_apply_locked(
            write_status,
            config_write_error_message("配置保存失败: 写入 SQLite channels 失败", write_error),
            restore_polling,
            error_message);
    }

    std::vector<std::string> reload_errors;
    const auto reload_status = reload_config_for_apply(lock, &reload_errors);
    if (is_ok(reload_status)) {
        return StatusCode::kOk;
    }

    const auto reload_detail = reload_errors.empty()
        ? std::string("重新加载通道配置失败")
        : reload_errors.front();
    std::string rollback_error;
    lock.unlock();
    const auto rollback_status = config_store_.save_channels(previous_channels, &rollback_error);
    lock.lock();

    std::string failure_message = "通道配置应用失败：" + reload_detail;
    if (is_ok(rollback_status)) {
        append_message(&failure_message, "SQLite channels 已回滚，旧运行态保持不变");
    } else {
        append_message(
            &failure_message,
            config_write_error_message("回滚 SQLite channels 失败", rollback_error));
    }
    return fail_config_apply_locked(
        reload_status,
        failure_message,
        restore_polling,
        error_message,
        is_ok(rollback_status));
}

}  // namespace edge_controller
