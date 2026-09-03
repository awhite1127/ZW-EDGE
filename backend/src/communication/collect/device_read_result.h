// 单设备多读取区块采集结果，供采集器、寄存器映射器和状态汇总共享。
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "shared/common/types.h"
#include "data/model/diagnosis_status.h"

namespace edge_controller {

// 一个读取区块的一次独立请求结果。寄存器数组始终从该区块偏移 0 开始，
// 不使用设备地址跨度填充稀疏占位。
struct DeviceReadBlockResult {
    std::string block_key;
    std::string display_name;
    std::uint32_t function_code{3};
    std::uint32_t request_address{0};
    std::uint32_t register_count{0};
    bool success{false};
    std::vector<std::uint16_t> registers;
    DiagnosisErrorCode diagnosis_error_code{DiagnosisErrorCode::kNone};
    std::string error_message;
};

using DeviceReadBlockResultMap =
    std::unordered_map<std::string, DeviceReadBlockResult>;

// 一台逻辑设备的全部读取区块结果；device_id 与 block_key 共同构成映射键。
struct DeviceReadResult {
    DeviceId device_id;
    std::uint32_t device_index{0};
    std::uint32_t device_base_address{0};
    DeviceReadBlockResultMap read_blocks;
};

}  // namespace edge_controller
