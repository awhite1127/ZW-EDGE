// 编码 MBAP 报文并严格校验 Modbus TCP 响应。
// 边界：严格校验帧边界与响应一致性，不猜测修复异常报文。

#pragma once
#include "data/model/diagnosis_status.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/common/status_code.h"

namespace edge_controller {

// 提供无状态的 Modbus TCP ADU 编解码。
class ModbusTcpProtocol {
public:
    static constexpr std::uint8_t kReadHoldingRegistersFunction = 0x03;
    static constexpr std::uint8_t kReadInputRegistersFunction = 0x04;
    static constexpr std::uint8_t kWriteMultipleHoldingRegistersFunction = 0x10;
    static constexpr std::uint16_t kProtocolId = 0;
    static constexpr std::uint16_t kMaxReadRegisterCount = 125;
    static constexpr std::uint16_t kMaxReadHoldingRegisterCount = kMaxReadRegisterCount;
    static constexpr std::uint16_t kMaxWriteMultipleRegisterCount = 123;
    static constexpr std::size_t kMbapHeaderSize = 7;

    // 按指定 FC03/FC04 构造 Modbus TCP 连续寄存器读取 ADU。
    static std::vector<std::uint8_t> build_read_registers_request(
        std::uint16_t transaction_id,
        std::uint8_t unit_id,
        std::uint8_t function_code,
        std::uint16_t start_register,
        std::uint16_t register_count);
    // 构造 Modbus TCP 读保持寄存器请求 ADU。
    static std::vector<std::uint8_t> build_read_holding_registers_request(
        std::uint16_t transaction_id,
        std::uint8_t unit_id,
        std::uint16_t start_register,
        std::uint16_t register_count);

    // 构造 Modbus TCP 写多个保持寄存器请求 ADU。
    static std::vector<std::uint8_t> build_write_multiple_holding_registers_request(
        std::uint16_t transaction_id,
        std::uint8_t unit_id,
        std::uint16_t start_register,
        const std::vector<std::uint16_t>& values);

    // 按指定 FC03/FC04 解析 Modbus TCP 连续寄存器读取响应 ADU。
    static StatusCode parse_read_registers_response(
        std::uint16_t expected_transaction_id,
        std::uint8_t expected_unit_id,
        std::uint8_t expected_function_code,
        std::uint16_t expected_register_count,
        const std::vector<std::uint8_t>& response,
        std::vector<std::uint16_t>* registers,
        std::string* error_message,
        bool* connection_reusable = nullptr,
        DiagnosisErrorCode* diagnosis = nullptr);
    // 解析 Modbus TCP 读保持寄存器响应 ADU。
    static StatusCode parse_read_holding_registers_response(
        std::uint16_t expected_transaction_id,
        std::uint8_t expected_unit_id,
        std::uint16_t expected_register_count,
        const std::vector<std::uint8_t>& response,
        std::vector<std::uint16_t>* registers,
        std::string* error_message,
        bool* connection_reusable = nullptr,
        DiagnosisErrorCode* diagnosis = nullptr);

    // 解析 Modbus TCP 写多个保持寄存器响应 ADU。
    static StatusCode parse_write_multiple_holding_registers_response(
        std::uint16_t expected_transaction_id,
        std::uint8_t expected_unit_id,
        std::uint16_t expected_start_register,
        std::uint16_t expected_register_count,
        const std::vector<std::uint8_t>& response,
        std::string* error_message,
        bool* connection_reusable = nullptr,
        DiagnosisErrorCode* diagnosis = nullptr);
};

}  // namespace edge_controller
