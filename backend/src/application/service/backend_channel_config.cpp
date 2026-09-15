// 通道配置局部应用：持久化成功后替换受影响资源，无关通道继续采集。
#include "application/service/backend_service.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "application/service/backend_service_internal.h"

namespace edge_controller {

using namespace backend_internal;

// 新增通道配置并局部应用连接变化。
StatusCode BackendService::create_channel_config(
    const ChannelConfigUpdateRequest& request,
    ChannelConfigUpdateResult* result,
    std::string* error_message)
{
    // 与手动 I/O 和 shutdown 串行；等待通道租约时不占用页面快照锁。
    std::lock_guard<std::mutex> channel_lock(channel_operation_mutex_);
    std::lock_guard<std::mutex> command_lock(manual_modbus_mutex_);
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

    const auto save_status =
        apply_channel_configs_locked(lock, next_channels, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto* applied_channel = &next_channels.back();

    result->channel_id = applied_channel->channel_id;
    result->channel_config = *applied_channel;
    result->message = "通道配置已应用，其他通道继续采集";
    result->warning_message = warning_message;
    result->polling_restarted = false;
    return StatusCode::kOk;
}

// 更新通道配置并局部应用连接变化。
StatusCode BackendService::update_channel_config(
    const ChannelConfigUpdateRequest& request,
    ChannelConfigUpdateResult* result,
    std::string* error_message)
{
    // 与手动 I/O 和 shutdown 串行；等待通道租约时不占用页面快照锁。
    std::lock_guard<std::mutex> channel_lock(channel_operation_mutex_);
    std::lock_guard<std::mutex> command_lock(manual_modbus_mutex_);
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

    const auto save_status =
        apply_channel_configs_locked(lock, next_channels, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    const auto* applied_channel = &next_channels[target_index];

    result->channel_id = applied_channel->channel_id;
    result->channel_config = *applied_channel;
    result->message = "通道配置已应用，其他通道继续采集";
    result->warning_message = warning_message;
    result->polling_restarted = false;
    return StatusCode::kOk;
}

// 删除通道配置并局部应用连接变化。
StatusCode BackendService::delete_channel_config(
    const ChannelId& channel_id,
    ChannelConfigDeleteResult* result,
    std::string* error_message)
{
    // 与手动 I/O 和 shutdown 串行；等待通道租约时不占用页面快照锁。
    std::lock_guard<std::mutex> channel_lock(channel_operation_mutex_);
    std::lock_guard<std::mutex> command_lock(manual_modbus_mutex_);
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

    const auto save_status =
        apply_channel_configs_locked(lock, next_channels, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }

    result->channel_id = channel_id;
    result->message = "通道配置已应用，其他通道继续采集";
    result->polling_restarted = false;
    return StatusCode::kOk;
}

// 保存已验证配置并替换受影响通道，保持无关资源与设备状态。
StatusCode BackendService::apply_channel_configs_locked(
    std::unique_lock<std::shared_mutex>& lock,
    const std::vector<ChannelConfig>& channels,
    std::string* error_message)
{
    auto next_config = system_config_;
    next_config.channels = channels;
    TopologyManager next_topology;
    const auto topology_status = next_topology.build(next_config, error_message);
    if (!is_ok(topology_status)) return topology_status;
    const auto previous_channels = system_config_.channels;
    // config_apply_in_progress_ 阻止其他配置应用，manual_modbus_mutex_ 阻止新手动 I/O。
    // 仅等待物理资源租约时释放服务锁；持久化和资源/拓扑交换仍作为一次提交。
    lock.unlock();
    try {
        const auto status = channel_manager_.apply_channels(channels,
            [&] {
                lock.lock();
                return config_store_.save_channels(channels, error_message);
            }, error_message);
        if (!lock.owns_lock()) lock.lock();
        if (!is_ok(status)) return status;
    } catch (const std::exception& error) {
        if (!lock.owns_lock()) lock.lock();
        if (error_message) *error_message = std::string("准备通道配置失败：") + error.what();
        return StatusCode::kInternalError;
    }
    for (const auto& previous : previous_channels)
        if (!find_channel_config_in_list(channels, previous.channel_id)) communication_trace_store_.clear_channel(previous.channel_id);
    system_config_.channels.swap(next_config.channels);
    topology_manager_ = std::move(next_topology);
    topology_manager_.rebind_system_config(system_config_);
    data_store_.reconcile_channels(channels);
    refresh_channel_statuses();
    // 被停用或重绑定的通道旧值立即失效，后续新采样恢复质量。
    if (polling_runtime_.get()) polling_runtime_.get()->invalidate_channels(previous_channels, channels);
    return StatusCode::kOk;
}

}  // namespace edge_controller
