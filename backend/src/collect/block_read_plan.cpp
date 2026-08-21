// 多读取区块地址计划：统一执行宽整数边界校验和稳定排序。
#include "collect/block_read_plan.h"

#include <algorithm>
#include <limits>

#include "protocol/modbus_rtu_protocol.h"

namespace edge_controller {

namespace {

// 生成读取区块显示标签。
std::string block_label(const DeviceTemplateReadBlockDefinition& block)
{
    return block.display_name.empty()
               ? block.block_key
               : block.display_name + "（" + block.block_key + "）";
}

// 生成主站显示标签。
std::string master_label(const MasterNodeConfig& master)
{
    return master.master_name.empty()
               ? master.master_id
               : master.master_name + "（" + master.master_id + "）";
}

}  // namespace

// 按设备和读取区块生成有序的 Modbus 读取计划。
StatusCode build_block_read_plan(
    const MasterNodeConfig& master_config,
    const DeviceTemplateDefinition& device_template,
    const std::vector<const DeviceConfig*>& devices,
    std::vector<BlockReadPlan>* plans,
    std::string* error_message)
{
    // 校验输出参数、设备类型读取模型和主站设备数量。
    if (plans == nullptr) {
        if (error_message != nullptr) *error_message = "缺少读取区块计划输出参数";
        return StatusCode::kInvalidArgument;
    }
    plans->clear();

    std::string model_error;
    if (!is_ok(validate_device_template_read_model(device_template, &model_error))) {
        if (error_message != nullptr) {
            *error_message = "主站 " + master_label(master_config) + " 的设备类型读取模型无效：" + model_error;
        }
        return StatusCode::kInvalidArgument;
    }
    if (master_config.device_count == 0 ||
        master_config.device_count > kMaxDevicesPerMaster) {
        if (error_message != nullptr) {
            *error_message =
                "主站设备数量必须在 1-" +
                std::to_string(kMaxDevicesPerMaster) + " 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (devices.size() != static_cast<std::size_t>(master_config.device_count)) {
        if (error_message != nullptr) {
            *error_message =
                "主站 " + master_label(master_config) + " 的自动设备数量与 device_count 不一致：配置=" +
                std::to_string(master_config.device_count) + "，运行设备=" + std::to_string(devices.size());
        }
        return StatusCode::kInvalidState;
    }

    // 按配置顺序和区块标识稳定排序读取区块。
    std::vector<const DeviceTemplateReadBlockDefinition*> sorted_blocks;
    sorted_blocks.reserve(device_template.read_blocks.size());
    for (const auto& block : device_template.read_blocks) sorted_blocks.push_back(&block);
    std::sort(
        sorted_blocks.begin(),
        sorted_blocks.end(),
        [](const auto* left, const auto* right) {
            if (left->sort_order != right->sort_order) return left->sort_order < right->sort_order;
            return left->block_key < right->block_key;
        });

    // 预估计划数量并防止容量溢出。
    const auto plan_count =
        static_cast<std::uint64_t>(master_config.device_count) *
        static_cast<std::uint64_t>(sorted_blocks.size());
    if (plan_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        if (error_message != nullptr) *error_message = "读取区块计划数量超出运行环境容量";
        return StatusCode::kInvalidArgument;
    }
    plans->reserve(static_cast<std::size_t>(plan_count));

    // 为每台设备计算基地址，并展开所有读取区块。
    for (std::size_t device_index = 0; device_index < devices.size(); ++device_index) {
        const auto* device = devices[device_index];
        if (device == nullptr) {
            if (error_message != nullptr) {
                *error_message = "主站 " + master_label(master_config) + " 的设备序号 " +
                                 std::to_string(device_index) + " 缺少运行设备定义";
            }
            plans->clear();
            return StatusCode::kInvalidState;
        }
        const auto device_base_address =
            static_cast<std::uint64_t>(master_config.block_start_register) +
            static_cast<std::uint64_t>(device_index) *
                static_cast<std::uint64_t>(device_template.device_address_stride);

        for (const auto* block : sorted_blocks) {
            const auto request_address =
                device_base_address + static_cast<std::uint64_t>(block->start_offset);
            const auto request_end =
                request_address + static_cast<std::uint64_t>(block->register_count) - 1ULL;
            if (request_address > 65535ULL || request_end > 65535ULL) {
                if (error_message != nullptr) {
                    *error_message =
                        "主站 " + master_label(master_config) + " 的设备序号 " +
                        std::to_string(device_index) + "、读取区块“" + block_label(*block) +
                        "”地址范围 " + std::to_string(request_address) + "～" +
                        std::to_string(request_end) + " 超出 Modbus 地址空间";
                }
                plans->clear();
                return StatusCode::kInvalidArgument;
            }

            const auto request_address_u16 =
                static_cast<std::uint16_t>(request_address);
            const auto register_count_u16 =
                static_cast<std::uint16_t>(block->register_count);
            plans->push_back(BlockReadPlan{
                device->device_id,
                device_index,
                static_cast<std::uint32_t>(device_base_address),
                block->block_key,
                block->display_name,
                block->function_code,
                request_address_u16,
                register_count_u16,
                block->sort_order,
                master_config.protocol == MasterProtocol::kModbusRtu
                    ? ModbusRtuProtocol::build_read_registers_request(
                          static_cast<std::uint8_t>(master_config.target_address),
                          static_cast<std::uint8_t>(block->function_code),
                          request_address_u16,
                          register_count_u16)
                    : std::vector<std::uint8_t>{},
            });
        }
    }

    if (error_message != nullptr) error_message->clear();
    return StatusCode::kOk;
}

}  // namespace edge_controller
