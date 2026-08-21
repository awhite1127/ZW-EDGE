// 定义采集器使用的 Modbus 客户端抽象。
// 边界：严格校验帧边界与响应一致性，不猜测修复异常报文。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "common/types.h"
#include "model/diagnosis_status.h"
#include "model/master_node_config.h"

namespace edge_controller {

struct ModbusReadResult {
    bool success{false};
    std::vector<std::uint16_t> registers;
    std::vector<std::uint8_t> request_frame;
    std::vector<std::uint8_t> response_frame;
    StatusCode transport_status{StatusCode::kOk};
    DiagnosisErrorCode diagnosis_error_code{DiagnosisErrorCode::kNone};
    TimestampMs timestamp_ms{0};
    std::string error_message;
};

struct ModbusWriteMultipleRegistersResult {
    bool success{false};
    std::uint16_t start_register{0};
    std::uint16_t register_count{0};
    std::vector<std::uint8_t> request_frame;
    std::vector<std::uint8_t> response_frame;
    StatusCode transport_status{StatusCode::kOk};
    DiagnosisErrorCode diagnosis_error_code{DiagnosisErrorCode::kNone};
    TimestampMs timestamp_ms{0};
    std::string error_message;
};

// 采集客户端边界。当前 TCP 侧通过 ModbusTcpClient 接入，
// RTU 主路径仍保留在 MasterCollector 内；通用读取能力支持 FC03/FC04。
class IModbusClient {
public:
    // 销毁 IModbusClient 实例并释放相关资源。
    virtual ~IModbusClient() = default;

    // 按读取区块给出的功能码、起始地址和数量读取连续寄存器块。
    virtual StatusCode read_registers(
        const MasterNodeConfig& master_config,
        std::uint8_t function_code,
        std::uint16_t start_register,
        std::uint16_t register_count,
        ModbusReadResult* result) = 0;
};

}  // namespace edge_controller
