// Modbus RTU 协议实现：构造/校验帧与 CRC，并通过 IChannel 完成传输，不拥有串口生命周期。
#include "communication/protocol/modbus_rtu_protocol.h"

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>

namespace edge_controller {

namespace {

// 计算指定前缀的 Modbus CRC16，解析响应时避免为去掉 CRC 尾部而复制整帧。
std::uint16_t crc16_prefix(const std::vector<std::uint8_t>& data, std::size_t size)
{
    std::uint16_t crc = 0xFFFF;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x0001U) != 0U
                      ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U)
                      : static_cast<std::uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

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
    return function_code == ModbusRtuProtocol::kReadHoldingRegistersFunction ||
           function_code == ModbusRtuProtocol::kReadInputRegistersFunction;
}

// 返回 Modbus 异常码的可读说明。
std::string modbus_exception_description(std::uint16_t code)
{
    switch (code) {
    case 0x0001:
        return "非法功能码";
    case 0x0002:
        return "寄存器不在有效范围内";
    case 0x0003:
        return "数据不在寄存器允许范围内";
    case 0x0004:
        return "从站设备执行失败";
    case 0x0005:
        return "从站已接受请求，正在处理";
    default:
        return "未知异常码";
    }
}

// 格式化 Modbus 异常响应消息。
std::string format_modbus_exception_message(std::uint16_t code, std::size_t code_byte_count)
{
    std::ostringstream stream;
    stream << "设备返回 Modbus 异常 0x"
           << std::uppercase << std::hex << std::setw(static_cast<int>(code_byte_count * 2U))
           << std::setfill('0') << code
           << "（" << modbus_exception_description(code) << "）";
    return stream.str();
}

// 解析 Modbus RTU 异常响应。
StatusCode parse_rtu_exception_response(
    const std::vector<std::uint8_t>& response,
    std::string* error_message,
    DiagnosisErrorCode* diagnosis)
{
    if (diagnosis) *diagnosis = DiagnosisErrorCode::kNone;
    const auto fail = [diagnosis](StatusCode status, DiagnosisErrorCode code) {
        if (diagnosis) *diagnosis = code;
        return status;
    };

    if (response.size() == 5) {
        const auto code = static_cast<std::uint16_t>(response[2]);
        if (error_message != nullptr) {
            *error_message = format_modbus_exception_message(code, 1);
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusExceptionResponse);
    }
    if (response.size() == 6) {
        const auto code = read_u16_be(response, 2);
        if (error_message != nullptr) {
            *error_message = format_modbus_exception_message(code, 2);
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusExceptionResponse);
    }
    if (error_message != nullptr) {
        *error_message = "Modbus 异常响应长度不匹配";
    }
    return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusLengthInvalid);
}

}  // namespace

// 计算 Modbus RTU CRC16 校验值。
std::uint16_t ModbusRtuProtocol::crc16(const std::vector<std::uint8_t>& data)
{
    // Modbus RTU 使用低位优先的 CRC16，多项式为 0xA001。
    return crc16_prefix(data, data.size());
}

// 按指定 FC03/FC04 构造 Modbus RTU 连续寄存器读取请求帧。
std::vector<std::uint8_t> ModbusRtuProtocol::build_read_registers_request(
    std::uint8_t slave_address,
    std::uint8_t function_code,
    std::uint16_t start_register,
    std::uint16_t register_count)
{
    if (!is_supported_read_function(function_code) ||
        register_count == 0 || register_count > kMaxReadRegisterCount) {
        return {};
    }

    // 请求帧格式：addr | func(0x03/0x04) | start_hi | start_lo | count_hi | count_lo | crc_lo | crc_hi

    std::vector<std::uint8_t> frame{
        slave_address,
        function_code,
        static_cast<std::uint8_t>((start_register >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(start_register & 0xFFU),
        static_cast<std::uint8_t>((register_count >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(register_count & 0xFFU),
    };

    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return frame;
}

// 构造 Modbus RTU 读保持寄存器请求帧。
std::vector<std::uint8_t> ModbusRtuProtocol::build_read_holding_registers_request(
    std::uint8_t slave_address,
    std::uint16_t start_register,
    std::uint16_t register_count)
{
    return build_read_registers_request(
        slave_address,
        kReadHoldingRegistersFunction,
        start_register,
        register_count);
}

// 构造 Modbus RTU 写多个保持寄存器请求帧。
std::vector<std::uint8_t> ModbusRtuProtocol::build_write_multiple_holding_registers_request(
    std::uint8_t slave_address,
    std::uint16_t start_register,
    const std::vector<std::uint16_t>& values)
{
    if (values.empty() || values.size() > kMaxWriteMultipleRegisterCount) {
        return {};
    }

    const auto register_count = static_cast<std::uint16_t>(values.size());
    const auto byte_count = static_cast<std::uint8_t>(register_count * 2U);

    // 请求帧格式：addr | func(0x10) | start | count | byte_count | values... | crc_lo | crc_hi

    std::vector<std::uint8_t> frame;
    frame.reserve(9U + static_cast<std::size_t>(byte_count));
    frame.push_back(slave_address);
    frame.push_back(kWriteMultipleHoldingRegistersFunction);
    append_u16_be(&frame, start_register);
    append_u16_be(&frame, register_count);
    frame.push_back(byte_count);
    for (const auto value : values) {
        append_u16_be(&frame, value);
    }

    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    return frame;
}

// 按指定 FC03/FC04 解析 Modbus RTU 连续寄存器读取响应帧。
StatusCode ModbusRtuProtocol::parse_read_registers_response(
    std::uint8_t expected_slave_address,
    std::uint8_t expected_function_code,
    std::uint16_t expected_register_count,
    const std::vector<std::uint8_t>& response,
    std::vector<std::uint16_t>* registers,
    std::string* error_message,
    DiagnosisErrorCode* diagnosis)
{
    if (diagnosis) *diagnosis = DiagnosisErrorCode::kNone;
    const auto fail = [diagnosis](StatusCode status, DiagnosisErrorCode code) {
        if (diagnosis) *diagnosis = code;
        return status;
    };

    if (registers == nullptr) {
        if (error_message != nullptr) {
            *error_message = "寄存器输出参数为空";
        }
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid);
    }

    registers->clear();

    if (!is_supported_read_function(expected_function_code)) {
        if (error_message != nullptr) {
            *error_message = "读取寄存器功能码只支持 FC03 或 FC04";
        }
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid);
    }
    if (expected_register_count == 0 || expected_register_count > kMaxReadRegisterCount) {
        if (error_message != nullptr) {
            *error_message = "单次读取寄存器数量超出范围，允许范围为 1-125";
        }
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid);
    }

    if (response.size() < 5) {
        if (error_message != nullptr) {
            *error_message = "响应帧长度过短";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusLengthInvalid);
    }

    const bool is_expected_exception =
        response[1] == static_cast<std::uint8_t>(expected_function_code | 0x80U);
    std::size_t expected_frame_size = 0U;
    if (is_expected_exception) {
        expected_frame_size = 5U;
    } else if (response[1] == expected_function_code) {
        expected_frame_size = 5U + static_cast<std::size_t>(response[2]);
    }
    if (expected_frame_size != 0U && response.size() != expected_frame_size) {
        if (error_message != nullptr) {
            *error_message = response.size() < expected_frame_size
                ? "响应帧长度不足：预期 " + std::to_string(expected_frame_size) +
                      " 字节，实际 " + std::to_string(response.size()) + " 字节"
                : "响应帧长度不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusLengthInvalid);
    }

    // 只有确认标准响应帧完整后，才把末尾两字节作为 CRC 执行校验。
    const auto expected_crc = crc16_prefix(response, response.size() - 2U);
    const auto crc_low = static_cast<std::uint16_t>(response[response.size() - 2]);
    const auto crc_high = static_cast<std::uint16_t>(response[response.size() - 1]);
    const auto response_crc = static_cast<std::uint16_t>(crc_low | static_cast<std::uint16_t>(crc_high << 8U));
    if (expected_crc != response_crc) {
        if (error_message != nullptr) {
            *error_message = "CRC 校验失败";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusCrcError);
    }

    if (response[0] != expected_slave_address) {
        if (error_message != nullptr) {
            *error_message = "从站地址不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusAddressMismatch);
    }

    if (is_expected_exception) {
        return parse_rtu_exception_response(response, error_message, diagnosis);
    }

    if (response[1] != expected_function_code) {
        if (error_message != nullptr) {
            *error_message = "功能码不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusFunctionMismatch);
    }

    const auto expected_byte_count = static_cast<std::size_t>(expected_register_count) * 2U;
    if (response[2] != expected_byte_count) {
        if (error_message != nullptr) {
            *error_message = "字节数不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusLengthInvalid);
    }

    // FC03/FC04 数据区都按大端字节序拼成同一 16 位寄存器数组。

    registers->reserve(expected_register_count);
    for (std::size_t offset = 0; offset < expected_byte_count; offset += 2) {
        const auto high = static_cast<std::uint16_t>(response[3 + offset]);
        const auto low = static_cast<std::uint16_t>(response[4 + offset]);
        registers->push_back(static_cast<std::uint16_t>((high << 8U) | low));
    }

    return StatusCode::kOk;
}

// 解析 Modbus RTU 读保持寄存器响应帧。
StatusCode ModbusRtuProtocol::parse_read_holding_registers_response(
    std::uint8_t expected_slave_address,
    std::uint16_t expected_register_count,
    const std::vector<std::uint8_t>& response,
    std::vector<std::uint16_t>* registers,
    std::string* error_message,
    DiagnosisErrorCode* diagnosis)
{
    return parse_read_registers_response(
        expected_slave_address,
        kReadHoldingRegistersFunction,
        expected_register_count,
        response,
        registers,
        error_message, diagnosis);
}

// 解析 Modbus RTU 写多个保持寄存器响应帧。
StatusCode ModbusRtuProtocol::parse_write_multiple_holding_registers_response(
    std::uint8_t expected_slave_address,
    std::uint16_t expected_start_register,
    std::uint16_t expected_register_count,
    const std::vector<std::uint8_t>& response,
    std::string* error_message,
    DiagnosisErrorCode* diagnosis)
{
    if (diagnosis) *diagnosis = DiagnosisErrorCode::kNone;
    const auto fail = [diagnosis](StatusCode status, DiagnosisErrorCode code) {
        if (diagnosis) *diagnosis = code;
        return status;
    };

    if (expected_register_count == 0 || expected_register_count > kMaxWriteMultipleRegisterCount) {
        if (error_message != nullptr) {
            *error_message = "单次写多个保持寄存器数量超出范围，允许范围为 1-123";
        }
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid);
    }

    if (response.size() < 5) {
        if (error_message != nullptr) {
            *error_message = "响应帧长度过短";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusLengthInvalid);
    }

    const bool is_write_exception = response[1] ==
        static_cast<std::uint8_t>(kWriteMultipleHoldingRegistersFunction | 0x80U);
    const std::size_t expected_frame_size = is_write_exception
        ? 5U
        : (response[1] == kWriteMultipleHoldingRegistersFunction ? 8U : 0U);
    if (expected_frame_size != 0U && response.size() != expected_frame_size) {
        if (error_message != nullptr) {
            *error_message = response.size() < expected_frame_size
                ? "响应帧长度不足：预期 " + std::to_string(expected_frame_size) +
                      " 字节，实际 " + std::to_string(response.size()) + " 字节"
                : "响应帧长度不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusLengthInvalid);
    }

    const auto expected_crc = crc16_prefix(response, response.size() - 2U);
    const auto crc_low = static_cast<std::uint16_t>(response[response.size() - 2]);
    const auto crc_high = static_cast<std::uint16_t>(response[response.size() - 1]);
    const auto response_crc = static_cast<std::uint16_t>(crc_low | static_cast<std::uint16_t>(crc_high << 8U));
    if (expected_crc != response_crc) {
        if (error_message != nullptr) {
            *error_message = "CRC 校验失败";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusCrcError);
    }

    if (response[0] != expected_slave_address) {
        if (error_message != nullptr) {
            *error_message = "从站地址不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusAddressMismatch);
    }

    if (is_write_exception) {
        return parse_rtu_exception_response(response, error_message, diagnosis);
    }

    if (response[1] != kWriteMultipleHoldingRegistersFunction) {
        if (error_message != nullptr) {
            *error_message = "功能码不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusFunctionMismatch);
    }

    const auto start_register = read_u16_be(response, 2);
    const auto register_count = read_u16_be(response, 4);
    if (start_register != expected_start_register) {
        if (error_message != nullptr) {
            *error_message = "起始寄存器地址不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusWriteEchoMismatch);
    }

    if (register_count != expected_register_count) {
        if (error_message != nullptr) {
            *error_message = "写入寄存器数量不匹配";
        }
        return fail(StatusCode::kProtocolError, DiagnosisErrorCode::kModbusWriteEchoMismatch);
    }

    return StatusCode::kOk;
}

}  // namespace edge_controller
