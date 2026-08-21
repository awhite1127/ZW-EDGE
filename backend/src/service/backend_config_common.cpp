// 配置写入公共流程：协调持久化、运行态替换、轮询恢复和部分失败消息。
#include "service/backend_service.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "model/device_template.h"
#include "service/backend_service_internal.h"

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

// 查找设备配置内列表。
const DeviceConfig* find_device_config_in_list(
    const std::vector<DeviceConfig>& devices,
    const DeviceId& device_id,
    std::size_t* index)
{
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].device_id == device_id) {
            if (index != nullptr) {
                *index = i;
            }
            return &devices[i];
        }
    }
    return nullptr;
}

// 生成自动推导设备的稳定标识。
std::string generated_device_id(const MasterNodeConfig& master, std::uint32_t index)
{
    return master.master_id + "_dev_" + std::to_string(index);
}

// 生成自动推导设备的显示名称。
std::string generated_device_name(const MasterNodeConfig& master, std::uint32_t index)
{
    if (!trim_copy(master.master_name).empty()) {
        return trim_copy(master.master_name) + "-设备" + std::to_string(index);
    }
    return "设备 " + std::to_string(index);
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

// 在持锁状态下读取并校验设备配置内列表。
StatusCode BackendService::require_device_config_in_list_locked(
    const std::vector<DeviceConfig>& devices,
    const DeviceId& device_id,
    std::size_t* index,
    std::string* error_message) const
{
    if (find_device_config_in_list(devices, device_id, index) == nullptr) {
        if (error_message != nullptr) {
            *error_message = "未找到自动推导设备: " + device_id;
        }
        return StatusCode::kNotFound;
    }
    return StatusCode::kOk;
}

// 在持锁状态下根据主站配置同步自动推导设备。
StatusCode BackendService::sync_devices_for_configs_locked(
    const std::vector<MasterNodeConfig>& masters,
    std::vector<DeviceConfig>* devices,
    std::vector<std::string>* errors) const
{
    if (devices == nullptr) {
        if (errors != nullptr) {
            errors->push_back("缺少自动设备输出列表");
        }
        return StatusCode::kInvalidArgument;
    }

    std::vector<DeviceConfig> generated_devices;
    std::unordered_set<DeviceId> generated_ids;
    std::vector<std::string> local_errors;
    auto add_error = [&](const std::string& message) {
        local_errors.push_back(message);
    };

    for (const auto& master : masters) {
        const auto stored_device_template = find_device_template(master.device_template);
        if (stored_device_template == nullptr) {
            add_error("主控 " + master.master_id + " 绑定的设备模板 " + master.device_template + " 不存在");
            continue;
        }
        std::string read_model_error;
        if (!is_ok(validate_device_template_read_model(*stored_device_template, &read_model_error))) {
            add_error(
                "主控 " + master.master_id + " 绑定的设备类型读取区块配置无效：" +
                read_model_error);
            continue;
        }
        const auto& device_template = stored_device_template;
        if (master.device_count == 0 || master.device_count > kMaxDevicesPerMaster) {
            add_error(
                "主控 " + master.master_id + " 的设备数量必须在 1-" +
                std::to_string(kMaxDevicesPerMaster) + " 范围内");
            continue;
        }

        const auto maximum_device_index = static_cast<std::uint64_t>(master.device_count - 1U);
        bool address_layout_valid = true;
        for (const auto& read_block : device_template->read_blocks) {
            const auto range_start =
                static_cast<std::uint64_t>(master.block_start_register) +
                maximum_device_index *
                    static_cast<std::uint64_t>(device_template->device_address_stride) +
                static_cast<std::uint64_t>(read_block.start_offset);
            const auto range_end =
                range_start + static_cast<std::uint64_t>(read_block.register_count);
            if (range_start >= 65536ULL || range_end > 65536ULL) {
                const auto master_label = trim_copy(master.master_name).empty()
                                              ? master.master_id
                                              : trim_copy(master.master_name) + "（" +
                                                    master.master_id + "）";
                const auto block_label = read_block.display_name.empty()
                                             ? read_block.block_key
                                             : read_block.display_name + "（" +
                                                   read_block.block_key + "）";
                add_error(
                    "主控 " + master_label + " 的设备序号 " +
                    std::to_string(maximum_device_index) + " 读取区块 " + block_label +
                    " 地址范围 [" + std::to_string(range_start) + "," +
                    std::to_string(range_end) + ") 超出 Modbus 寄存器地址上限");
                address_layout_valid = false;
            }
        }
        if (!address_layout_valid) continue;

        generated_devices.reserve(
            generated_devices.size() + static_cast<std::size_t>(master.device_count));
        for (std::uint32_t device_index = 0; device_index < master.device_count; ++device_index) {
            const auto sequence = device_index + 1U;
            const auto offset_value =
                static_cast<std::uint64_t>(device_index) *
                static_cast<std::uint64_t>(device_template->device_address_stride);
            const auto offset = static_cast<RegisterAddress>(offset_value);

            const auto id = generated_device_id(master, sequence);
            if (!generated_ids.insert(id).second) {
                add_error("自动生成设备 ID 重复: " + id);
                continue;
            }
            DeviceConfig device;
            device.device_id = id;
            device.generated_name = generated_device_name(master, sequence);
            device.device_name = device.generated_name;
            if (trim_copy(device.device_name).empty()) {
                device.device_name = generated_device_name(master, sequence);
            }
            device.master_id = master.master_id;
            device.enabled = master.enabled;
            device.register_offset = offset;
            generated_devices.push_back(device);
        }
    }

    if (!local_errors.empty()) {
        if (errors != nullptr) {
            errors->insert(errors->end(), local_errors.begin(), local_errors.end());
        }
        return StatusCode::kInvalidArgument;
    }

    *devices = std::move(generated_devices);
    return StatusCode::kOk;
}

// 在持锁状态下同步指定主站配置对应的自动设备。
StatusCode BackendService::sync_devices_for_master_configs_locked(
    const std::vector<MasterNodeConfig>& masters,
    std::vector<DeviceConfig>* devices,
    std::string* error_message) const
{
    std::vector<std::string> sync_errors;
    const auto status = sync_devices_for_configs_locked(masters, devices, &sync_errors);
    if (!is_ok(status) && error_message != nullptr) {
        *error_message = sync_errors.empty() ? "自动推导设备失败" : sync_errors.front();
    }
    return status;
}

// 在持锁状态下校验同步后的自动设备列表。
StatusCode BackendService::validate_synced_auto_devices_locked(
    const SystemConfig& config,
    std::vector<std::string>* errors) const
{
    std::vector<std::string> local_errors;
    std::unordered_set<DeviceId> seen_device_ids;
    std::unordered_map<MasterNodeId, std::uint32_t> device_counts_by_master;
    auto add_error = [&](const std::string& message) {
        local_errors.push_back(message);
    };

    for (const auto& device : config.devices) {
        if (!seen_device_ids.insert(device.device_id).second) {
            add_error("设备 ID 重复: " + device.device_id);
            continue;
        }
        const auto* master = find_master_config_in_list(config.master_nodes, device.master_id);
        if (master == nullptr) {
            add_error("设备 " + device.device_id + " 引用了不存在的主控");
            continue;
        }
        ++device_counts_by_master[master->master_id];
        const auto stored_definition = find_device_template(master->device_template);
        if (stored_definition == nullptr) {
            add_error("设备 " + device.device_id + " 的所属主控未配置有效设备模板");
            continue;
        }
        std::string read_model_error;
        if (!is_ok(validate_device_template_read_model(*stored_definition, &read_model_error))) {
            add_error(
                "设备 " + device.device_id + " 的所属主控设备类型读取区块配置无效：" +
                read_model_error);
            continue;
        }
        if (master->device_count == 0 || master->device_count > kMaxDevicesPerMaster) {
            add_error(
                "设备 " + device.device_id + " 的所属主控设备数量必须在 1-" +
                std::to_string(kMaxDevicesPerMaster) + " 范围内");
            continue;
        }
        const auto offset = static_cast<std::uint64_t>(device.register_offset);
        const auto stride = static_cast<std::uint64_t>(stored_definition->device_address_stride);
        if (stride == 0 || offset % stride != 0 || offset / stride >= master->device_count) {
            add_error(
                "设备 " + device.device_id + " 的设备基地址偏移不符合所属主控的设备数量与地址跨度");
            continue;
        }
        const auto device_index = static_cast<std::uint32_t>(offset / stride);
        for (const auto& read_block : stored_definition->read_blocks) {
            const auto range_start =
                static_cast<std::uint64_t>(master->block_start_register) + offset +
                static_cast<std::uint64_t>(read_block.start_offset);
            const auto range_end =
                range_start + static_cast<std::uint64_t>(read_block.register_count);
            if (range_start >= 65536ULL || range_end > 65536ULL) {
                const auto master_label = trim_copy(master->master_name).empty()
                                              ? master->master_id
                                              : trim_copy(master->master_name) + "（" +
                                                    master->master_id + "）";
                const auto block_label = read_block.display_name.empty()
                                             ? read_block.block_key
                                             : read_block.display_name + "（" +
                                                   read_block.block_key + "）";
                add_error(
                    "主控 " + master_label + " 的设备序号 " +
                    std::to_string(device_index) + " 读取区块 " + block_label +
                    " 地址范围 [" + std::to_string(range_start) + "," +
                    std::to_string(range_end) + ") 超出 Modbus 寄存器地址上限");
            }
        }
    }

    for (const auto& master : config.master_nodes) {
        const auto generated_count = device_counts_by_master[master.master_id];
        if (generated_count != master.device_count) {
            add_error(
                "主控 " + master.master_id + " 的自动生成设备数量 " +
                std::to_string(generated_count) + " 与配置 device_count=" +
                std::to_string(master.device_count) + " 不一致");
        }
    }

    for (const auto& left : config.devices) {
        for (const auto& right : config.devices) {
            if (left.device_id >= right.device_id || left.master_id != right.master_id) {
                continue;
            }
            if (left.register_offset == right.register_offset) {
                add_error(
                    "主控 " + left.master_id + " 下设备基地址偏移重复，冲突设备为 " +
                    left.device_id + " 和 " + right.device_id);
            }
        }
    }

    if (!local_errors.empty()) {
        if (errors != nullptr) {
            errors->insert(errors->end(), local_errors.begin(), local_errors.end());
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}


// 在持锁状态下重新加载运行状态之后配置写入。
StatusCode BackendService::reload_runtime_after_config_write_locked(
    std::unique_lock<std::shared_mutex>& lock,
    const std::string& reload_error_fallback,
    bool restore_polling,
    std::string* error_message)
{
    std::vector<std::string> reload_errors;
    const auto reload_status = reload_config_for_apply(lock, &reload_errors);
    if (!is_ok(reload_status)) {
        const auto detail = !reload_errors.empty() ? reload_errors.front() : reload_error_fallback;
        return fail_config_apply_locked(
            reload_status,
            "配置已保存，但采集链路重建失败: " + detail,
            restore_polling,
            error_message);
    }
    return StatusCode::kOk;
}


}  // namespace edge_controller
