// 将主站、设备类型和自动设备清单展开为稳定的逐设备逐区块读取计划。
// 边界：只负责地址与顺序计算，不执行 Modbus I/O，也不解析字段。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "common/types.h"
#include "model/device_config.h"
#include "model/device_template.h"
#include "model/master_node_config.h"

namespace edge_controller {

struct BlockReadPlan {
    DeviceId device_id;
    std::size_t device_index{0};
    std::uint32_t device_base_address{0};
    std::string block_key;
    std::string block_display_name;
    std::uint32_t function_code{3};
    std::uint16_t request_address{0};
    std::uint16_t register_count{0};
    int sort_order{0};
    // RTU 请求在配置世代内完全不变，预先编码后跨轮询周期复用；TCP 事务号动态生成。
    std::vector<std::uint8_t> rtu_request_frame;
};

// 展开主站与设备类型配置，生成稳定的分区块读取计划。
StatusCode build_block_read_plan(
    const MasterNodeConfig& master_config,
    const DeviceTemplateDefinition& device_template,
    const std::vector<const DeviceConfig*>& devices,
    std::vector<BlockReadPlan>* plans,
    std::string* error_message = nullptr);

}  // namespace edge_controller
