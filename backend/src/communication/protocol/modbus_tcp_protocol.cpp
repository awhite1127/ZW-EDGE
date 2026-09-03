// Modbus TCP 协议实现：维护事务号并校验 MBAP/PDU 边界，不负责 TCP 重连策略。
#include "communication/protocol/modbus_tcp_protocol.h"

#include <cstddef>
#include <string>

namespace edge_controller {

namespace {

// 按大端字节序读取 16 位无符号整数。
std::uint16_t read_u16_be(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[offset]) << 8U) |
        static_cast<std::uint16_t>(data[offset + 1]));
}

// 按大端字节序追加 16 位无符号整数。
void append_u16_be(std::vector<std::uint8_t>* data, std::uint16_t value)
{
    data->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    data->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

// 判断 Modbus 读功能码是否受支持。
bool is_supported_read_function(std::uint8_t function_code)
{
    return function_code == ModbusTcpProtocol::kReadHoldingRegistersFunction ||
           function_code == ModbusTcpProtocol::kReadInputRegistersFunction;
}

// 解析 Modbus TCP 异常响应。
StatusCode parse_tcp_exception_response(
    std::uint16_t mbap_length,
    const std::vector<std::uint8_t>& response,
    std::string* error_message)
{
    if (mbap_length != 3 || response.size() != ModbusTcpProtocol::kMbapHeaderSize + 2U) {
        if (error_message != nullptr) {
            *error_message = "Modbus TCP 异常响应长度不匹配";
        }
        return StatusCode::kProtocolError;
    }
    if (error_message != nullptr) {
        *error_message = "Modbus TCP 异常码: " + std::to_string(response[8]);
    }
    return StatusCode::kProtocolError;
}

}  // namespace

// 按指定 FC03/FC04 构造 Modbus TCP 连续寄存器读取 ADU。
std::vector<std::uint8_t> ModbusTcpProtocol::build_read_registers_request(
    std::uint16_t transaction_id,
    std::uint8_t unit_id,
    std::uint8_t function_code,
    std::uint16_t start_register,
    std::uint16_t register_count)
{
    if (!is_supported_read_function(function_code) ||
        register_count == 0 || register_count > kMaxReadRegisterCount) {
        return {};
    }

    std::vector<std::uint8_t> adu;
    adu.reserve(12);
    append_u16_be(&adu, transaction_id);
    append_u16_be(&adu, kProtocolId);
    append_u16_be(&adu, 6);
    adu.push_back(unit_id);
    adu.push_back(function_code);
    append_u16_be(&adu, start_register);
    append_u16_be(&adu, register_count);
    return adu;
}

// 构造 Modbus TCP 读保持寄存器请求 ADU。
std::vector<std::uint8_t> ModbusTcpProtocol::build_read_holding_registers_request(
    std::uint16_t transaction_id,
    std::uint8_t unit_id,
    std::uint16_t start_register,
    std::uint16_t register_count)
{
    return build_read_registers_request(
        transaction_id,
        unit_id,
        kReadHoldingRegistersFunction,
        start_register,
        register_count);
}

// 构造 Modbus TCP 写多个保持寄存器请求 ADU。
std::vector<std::uint8_t> ModbusTcpProtocol::build_write_multiple_holding_registers_request(
    std::uint16_t transaction_id,
    std::uint8_t unit_id,
    std::uint16_t start_register,
    const std::vector<std::uint16_t>& values)
{
    if (values.empty() || values.size() > kMaxWriteMultipleRegisterCount) {
        return {};
    }

    const auto register_count = static_cast<std::uint16_t>(values.size());
    const auto byte_count = static_cast<std::uint8_t>(register_count * 2U);
    const auto mbap_length = static_cast<std::uint16_t>(7U + byte_count);

    std::vector<std::uint8_t> adu;
    adu.reserve(static_cast<std::size_t>(6U + mbap_length));
    append_u16_be(&adu, transaction_id);
    append_u16_be(&adu, kProtocolId);
    append_u16_be(&adu, mbap_length);
    adu.push_back(unit_id);
    adu.push_back(kWriteMultipleHoldingRegistersFunction);
    append_u16_be(&adu, start_register);
    append_u16_be(&adu, register_count);
    adu.push_back(byte_count);
    for (const auto value : values) {
        append_u16_be(&adu, value);
    }
    return adu;
}

// 按指定 FC03/FC04 解析 Modbus TCP 连续寄存器读取响应 ADU。
StatusCode ModbusTcpProtocol::parse_read_registers_response(
    std::uint16_t expected_transaction_id,
    std::uint8_t expected_unit_id,
    std::uint8_t expected_function_code,
    std::uint16_t expected_register_count,
    const std::vector<std::uint8_t>& response,
    std::vector<std::uint16_t>* registers,
    std::string* error_message,
    bool* connection_reusable)
{
    if (connection_reusable != nullptr) {
        *connection_reusable = false;
    }
    if (registers == nullptr) {
        if (error_message != nullptr) {
            *error_message = "寄存器输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    registers->clear();

    if (!is_supported_read_function(expected_function_code)) {
        if (error_message != nullptr) {
            *error_message = "读取寄存器功能码只支持 FC03 或 FC04";
        }
        return StatusCode::kInvalidArgument;
    }
    if (expected_register_count == 0 || expected_register_count > kMaxReadRegisterCount) {
        if (error_message != nullptr) {
            *error_message = "单次读取寄存器数量超出范围，允许范围为 1-125";
        }
        return StatusCode::kInvalidArgument;
    }

    if (response.size() < kMbapHeaderSize + 2U) {
        if (error_message != nullptr) {
            *error_message = "Modbus TCP 响应长度不足";
        }
        return StatusCode::kProtocolError;
    }

    const auto transaction_id = read_u16_be(response, 0);
    if (transaction_id != expected_transaction_id) {
        if (error_message != nullptr) {
            *error_message = "MBAP Transaction ID 不匹配";
        }
        return StatusCode::kProtocolError;
    }

    const auto protocol_id = read_u16_be(response, 2);
    if (protocol_id != kProtocolId) {
        if (error_message != nullptr) {
            *error_message = "MBAP Protocol ID 异常";
        }
        return StatusCode::kProtocolError;
    }

    const auto mbap_length = read_u16_be(response, 4);
    if (mbap_length < 2 || response.size() != static_cast<std::size_t>(6U + mbap_length)) {
        if (error_message != nullptr) {
            *error_message = "MBAP Length 异常";
        }
        return StatusCode::kProtocolError;
    }

    const auto unit_id = response[6];
    if (unit_id != expected_unit_id) {
        if (error_message != nullptr) {
            *error_message = "Unit ID 不匹配";
        }
        return StatusCode::kProtocolError;
    }

    // 到这里已确认 MBAP 边界、事务号及 Unit ID，整帧已被完整消费；
    // 后续 PDU 语义错误（包括合法异常响应）不会污染下一事务的字节流。
    if (connection_reusable != nullptr) {
        *connection_reusable = true;
    }

    const auto function_code = response[7];
    if (function_code == static_cast<std::uint8_t>(expected_function_code | 0x80U)) {
        return parse_tcp_exception_response(mbap_length, response, error_message);
    }

    if (function_code != expected_function_code) {
        if (error_message != nullptr) {
            *error_message = "功能码不匹配";
        }
        return StatusCode::kProtocolError;
    }

    const auto expected_byte_count = static_cast<std::size_t>(expected_register_count) * 2U;
    const auto byte_count = static_cast<std::size_t>(response[8]);
    if (byte_count != expected_byte_count) {
        if (error_message != nullptr) {
            *error_message = "Byte Count 异常";
        }
        return StatusCode::kProtocolError;
    }

    const auto expected_response_size = kMbapHeaderSize + 2U + expected_byte_count;
    if (response.size() != expected_response_size) {
        if (error_message != nullptr) {
            *error_message = "Modbus TCP 响应数据长度异常";
        }
        return StatusCode::kProtocolError;
    }

    registers->reserve(expected_register_count);
    for (std::size_t offset = 0; offset < expected_byte_count; offset += 2) {
        const auto high = static_cast<std::uint16_t>(response[9 + offset]);
        const auto low = static_cast<std::uint16_t>(response[10 + offset]);
        registers->push_back(static_cast<std::uint16_t>((high << 8U) | low));
    }

    return StatusCode::kOk;
}

// 解析 Modbus TCP 读保持寄存器响应 ADU。
StatusCode ModbusTcpProtocol::parse_read_holding_registers_response(
    std::uint16_t expected_transaction_id,
    std::uint8_t expected_unit_id,
    std::uint16_t expected_register_count,
    const std::vector<std::uint8_t>& response,
    std::vector<std::uint16_t>* registers,
    std::string* error_message,
    bool* connection_reusable)
{
    return parse_read_registers_response(
        expected_transaction_id,
        expected_unit_id,
        kReadHoldingRegistersFunction,
        expected_register_count,
        response,
        registers,
        error_message,
        connection_reusable);
}

// 解析 Modbus TCP 写多个保持寄存器响应 ADU。
StatusCode ModbusTcpProtocol::parse_write_multiple_holding_registers_response(
    std::uint16_t expected_transaction_id,
    std::uint8_t expected_unit_id,
    std::uint16_t expected_start_register,
    std::uint16_t expected_register_count,
    const std::vector<std::uint8_t>& response,
    std::string* error_message,
    bool* connection_reusable)
{
    if (connection_reusable != nullptr) {
        *connection_reusable = false;
    }
    // 校验期望写入数量和响应基本长度。
    if (expected_register_count == 0 || expected_register_count > kMaxWriteMultipleRegisterCount) {
        if (error_message != nullptr) {
            *error_message = "单次写多个保持寄存器数量超出范围，允许范围为 1-123";
        }
        return StatusCode::kInvalidArgument;
    }

    if (response.size() < kMbapHeaderSize + 1U) {
        if (error_message != nullptr) {
            *error_message = "Modbus TCP 响应长度不足";
        }
        return StatusCode::kProtocolError;
    }

    // 校验 MBAP 事务标识、协议标识和长度字段。
    const auto transaction_id = read_u16_be(response, 0);
    if (transaction_id != expected_transaction_id) {
        if (error_message != nullptr) {
            *error_message = "MBAP Transaction ID 不匹配";
        }
        return StatusCode::kProtocolError;
    }

    const auto protocol_id = read_u16_be(response, 2);
    if (protocol_id != kProtocolId) {
        if (error_message != nullptr) {
            *error_message = "MBAP Protocol ID 异常";
        }
        return StatusCode::kProtocolError;
    }

    const auto mbap_length = read_u16_be(response, 4);
    if (mbap_length < 2 || response.size() != static_cast<std::size_t>(6U + mbap_length)) {
        if (error_message != nullptr) {
            *error_message = "MBAP Length 异常";
        }
        return StatusCode::kProtocolError;
    }

    // 校验 Unit ID，并优先处理 Modbus 异常响应。
    const auto unit_id = response[6];
    if (unit_id != expected_unit_id) {
        if (error_message != nullptr) {
            *error_message = "Unit ID 不匹配";
        }
        return StatusCode::kProtocolError;
    }

    if (connection_reusable != nullptr) {
        *connection_reusable = true;
    }

    const auto function_code = response[7];
    if (function_code == static_cast<std::uint8_t>(kWriteMultipleHoldingRegistersFunction | 0x80U)) {
        return parse_tcp_exception_response(mbap_length, response, error_message);
    }

    if (function_code != kWriteMultipleHoldingRegistersFunction) {
        if (error_message != nullptr) {
            *error_message = "功能码不匹配";
        }
        return StatusCode::kProtocolError;
    }

    // 正常响应的 PDU 长度固定，并应回显起始地址和寄存器数量。
    if (mbap_length != 6) {
        if (error_message != nullptr) {
            *error_message = "MBAP Length 异常";
        }
        return StatusCode::kProtocolError;
    }

    if (response.size() != 12) {
        if (error_message != nullptr) {
            *error_message = "Modbus TCP 响应数据长度异常";
        }
        return StatusCode::kProtocolError;
    }

    const auto start_register = read_u16_be(response, 8);
    const auto register_count = read_u16_be(response, 10);
    if (start_register != expected_start_register) {
        if (error_message != nullptr) {
            *error_message = "起始寄存器地址不匹配";
        }
        return StatusCode::kProtocolError;
    }

    if (register_count != expected_register_count) {
        if (error_message != nullptr) {
            *error_message = "写入寄存器数量不匹配";
        }
        return StatusCode::kProtocolError;
    }

    return StatusCode::kOk;
}

}  // namespace edge_controller
