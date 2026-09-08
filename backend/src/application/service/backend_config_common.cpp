// 配置写入公共流程：协调持久化、运行态替换、轮询恢复和部分失败消息。
#include "application/service/backend_service.h"

#include <cctype>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "data/model/device_derivation.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

namespace backend_internal {

// 返回去除首尾空白的字符串副本。
std::string trim_copy(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(begin, end - begin);
}

// 生成配置写入失败的可读错误。
std::string config_write_error_message(const std::string& fallback, const std::string& detail)
{
    return detail.empty() ? fallback : "配置保存失败: " + detail;
}

// 查找通道配置内列表。
const ChannelConfig* find_channel_config_in_list(
    const std::vector<ChannelConfig>& channels,
    const ChannelId& channel_id,
    std::size_t* index)
{
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].channel_id == channel_id) {
            if (index != nullptr) {
                *index = i;
            }
            return &channels[i];
        }
    }
    return nullptr;
}

// 查找主站配置内列表。
const MasterNodeConfig* find_master_config_in_list(
    const std::vector<MasterNodeConfig>& masters,
    const MasterNodeId& master_id,
    std::size_t* index)
{
    for (std::size_t i = 0; i < masters.size(); ++i) {
        if (masters[i].master_id == master_id) {
            if (index != nullptr) {
                *index = i;
            }
            return &masters[i];
        }
    }
    return nullptr;
}

}  // namespace backend_internal

using namespace backend_internal;

// 在持锁状态下确认配置变更可以开始。
StatusCode BackendService::ensure_config_mutation_ready_locked(
    const char* missing_result_message,
    const void* result,
    std::string* error_message) const
{
    if (!initialized_) {
        if (error_message != nullptr) {
            *error_message = "后端服务尚未初始化";
        }
        return StatusCode::kInvalidState;
    }
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = missing_result_message != nullptr ? missing_result_message : "缺少配置变更结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (config_apply_in_progress_.load()) {
        if (error_message != nullptr) {
            *error_message = "配置正在应用中，请稍后重试";
        }
        return StatusCode::kInvalidState;
    }
    return StatusCode::kOk;
}

// 在持锁状态下确保通道标识可用状态。
StatusCode BackendService::ensure_channel_id_available_locked(
    const ChannelId& channel_id,
    std::string* error_message) const
{
    if (find_channel_config_in_list(system_config_.channels, channel_id) != nullptr) {
        if (error_message != nullptr) {
            *error_message = "通道 ID 已存在: " + channel_id;
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 在持锁状态下读取并校验通道配置。
StatusCode BackendService::require_channel_config_locked(
    const ChannelId& channel_id,
    const ChannelConfig** channel,
    std::size_t* index,
    std::string* error_message) const
{
    const auto* found = find_channel_config_in_list(system_config_.channels, channel_id, index);
    if (found == nullptr) {
        if (error_message != nullptr) {
            *error_message = "未找到通道: " + channel_id;
        }
        return StatusCode::kNotFound;
    }
    if (channel != nullptr) {
        *channel = found;
    }
    return StatusCode::kOk;
}

// 在持锁状态下读取并校验主站配置。
StatusCode BackendService::require_master_config_locked(
    const MasterNodeId& master_id,
    const MasterNodeConfig** master,
    std::size_t* index,
    std::string* error_message) const
{
    const auto* found = find_master_config_in_list(system_config_.master_nodes, master_id, index);
    if (found == nullptr) {
        if (error_message != nullptr) {
            *error_message = "未找到主控: " + master_id;
        }
        return StatusCode::kNotFound;
    }
    if (master != nullptr) {
        *master = found;
    }
    return StatusCode::kOk;
}

// 在持锁状态下同步指定主站配置对应的自动设备。
StatusCode BackendService::sync_devices_for_master_configs_locked(
    const std::vector<MasterNodeConfig>& masters,
    std::vector<DeviceConfig>& devices,
    std::string* error_message) const
{
    std::vector<std::string> sync_errors;
    const auto status = derive_devices_from_masters(masters, devices, &sync_errors);
    if (!is_ok(status) && error_message != nullptr) {
        *error_message = sync_errors.empty() ? "自动推导设备失败" : sync_errors.front();
    }
    return status;
}

}  // namespace edge_controller
