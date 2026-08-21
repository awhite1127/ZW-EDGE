// 定义通道、协议、质量和日志等级等跨模块枚举及稳定字符串转换。
// 边界：保持低依赖、无业务状态，供上层单向复用。

#pragma once

#include <cstdint>

namespace edge_controller {

// 当前正式版本支持的通信通道类型。
enum class ChannelType : std::uint8_t {
    kUnknown = 0,
    kModbusRtuSerial = 1,
    kModbusTcp = 2,
};

enum class MasterProtocol : std::uint8_t {
    kUnknown = 0,
    kModbusRtu = 1,
    kModbusTcp = 2,
};

// Modbus RTU 串口使用的校验位配置。
enum class SerialParity : std::uint8_t {
    kNone = 0,
    kEven = 1,
    kOdd = 2,
};

// 最近一次采样值的基础质量标记。
enum class DataQuality : std::uint8_t {
    kUnknown = 0,
    kGood = 1,
    kStale = 2,
    kBad = 3,
    kPartial = 4,
};

// 后端进程输出使用的轻量级日志等级。
enum class LogLevel : std::uint8_t {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
};

// 将枚举值转换为字符串。
inline const char* to_string(ChannelType value)
{
    switch (value) {
    case ChannelType::kModbusRtuSerial:
        return "modbus_rtu_serial";
    case ChannelType::kModbusTcp:
        return "modbus_tcp";
    case ChannelType::kUnknown:
    default:
        return "unknown";
    }
}

// 将枚举值转换为字符串。
inline const char* to_string(MasterProtocol value)
{
    switch (value) {
    case MasterProtocol::kModbusRtu:
        return "modbus_rtu";
    case MasterProtocol::kModbusTcp:
        return "modbus_tcp";
    case MasterProtocol::kUnknown:
    default:
        return "unknown";
    }
}

// 将枚举值转换为字符串。
inline const char* to_string(SerialParity value)
{
    switch (value) {
    case SerialParity::kNone:
        return "none";
    case SerialParity::kEven:
        return "even";
    case SerialParity::kOdd:
        return "odd";
    default:
        return "unknown";
    }
}

// 将枚举值转换为字符串。
inline const char* to_string(DataQuality value)
{
    switch (value) {
    case DataQuality::kGood:
        return "good";
    case DataQuality::kStale:
        return "stale";
    case DataQuality::kBad:
        return "bad";
    case DataQuality::kPartial:
        return "partial";
    case DataQuality::kUnknown:
    default:
        return "unknown";
    }
}

// 将枚举值转换为字符串。
inline const char* to_string(LogLevel value)
{
    switch (value) {
    case LogLevel::kDebug:
        return "debug";
    case LogLevel::kInfo:
        return "info";
    case LogLevel::kWarn:
        return "warn";
    case LogLevel::kError:
        return "error";
    default:
        return "unknown";
    }
}

}  // namespace edge_controller
