// 编码 RTU 请求、校验 CRC 并解析功能码响应。
// 边界：严格校验帧边界与响应一致性，不猜测修复异常报文。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status_code.h"

namespace edge_controller {

// 提供无状态的 Modbus RTU 帧编解码。

class ModbusRtuProtocol {
public:
    static constexpr std::uint8_t kReadHoldingRegistersFunction = 0x03;
    static constexpr std::uint8_t kReadInputRegistersFunction = 0x04;
    static constexpr std::uint8_t kWriteMultipleHoldingRegistersFunction = 0x10;
    static constexpr std::uint16_t kMaxReadRegisterCount = 125;
    static constexpr std::uint16_t kMaxReadHoldingRegisterCount = kMaxReadRegisterCount;
    static constexpr std::uint16_t kMaxWriteMultipleRegisterCount = 123;

    // 计算 Modbus RTU 帧 CRC16 校验值。
    static std::uint16_t crc16(const std::vector<std::uint8_t>& data);
    // 按指定 FC03/FC04 构造 Modbus RTU 连续寄存器读取请求帧。
    static std::vector<std::uint8_t> build_read_registers_request(
        std::uint8_t slave_address,
        std::uint8_t function_code,
        std::uint16_t start_register,
        std::uint16_t register_count);
    // 构造 Modbus RTU 读保持寄存器请求帧。
    static std::vector<std::uint8_t> build_read_holding_registers_request(
        std::uint8_t slave_address,
        std::uint16_t start_register,
        std::uint16_t register_count);

    // 构造 Modbus RTU 写多个保持寄存器请求帧。
    static std::vector<std::uint8_t> build_write_multiple_holding_registers_request(
        std::uint8_t slave_address,
        std::uint16_t start_register,
        const std::vector<std::uint16_t>& values);

    // 按指定 FC03/FC04 解析 Modbus RTU 连续寄存器读取响应帧。
    static StatusCode parse_read_registers_response(
        std::uint8_t expected_slave_address,
        std::uint8_t expected_function_code,
        std::uint16_t expected_register_count,
        const std::vector<std::uint8_t>& response,
        std::vector<std::uint16_t>* registers,
        std::string* error_message);
    // 解析 Modbus RTU 读保持寄存器响应帧。
    static StatusCode parse_read_holding_registers_response(
        std::uint8_t expected_slave_address,
        std::uint16_t expected_register_count,
        const std::vector<std::uint8_t>& response,
        std::vector<std::uint16_t>* registers,
        std::string* error_message);

    // 解析 Modbus RTU 写多个保持寄存器响应帧。
    static StatusCode parse_write_multiple_holding_registers_response(
        std::uint8_t expected_slave_address,
        std::uint16_t expected_start_register,
        std::uint16_t expected_register_count,
        const std::vector<std::uint8_t>& response,
        std::string* error_message);
};

}  // namespace edge_controller
