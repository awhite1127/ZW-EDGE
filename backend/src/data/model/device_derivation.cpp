// 主站配置到自动设备的唯一构造边界，不依赖 BackendService 生命周期或仓储。
#include "data/model/device_derivation.h"

#include <cctype>
#include <cstdint>
#include <unordered_set>
#include <utility>

#include "data/model/device_template.h"

namespace edge_controller {
namespace {

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

}  // namespace

// 先验证主站布局，再生成设备；成功后才替换输出，不暴露半成品。
StatusCode derive_devices_from_masters(
    const std::vector<MasterNodeConfig>& masters,
    std::vector<DeviceConfig>& devices,
    std::vector<std::string>* errors)
{
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

        // 正跨度与最大设备地址通过校验后，各序号偏移天然唯一，无需两两复检。
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

    devices = std::move(generated_devices);
    return StatusCode::kOk;
}

}  // namespace edge_controller
