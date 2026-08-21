// 将工程量预编码为北向 Modbus holding registers；不访问 SQLite 或南向采集状态。
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "common/status_code.h"
#include "model/modbus_server.h"

namespace edge_controller {

struct EncodedModbusRegisters {
    std::array<std::uint16_t, 2> registers{};
    std::uint16_t register_count{0};
};

// 按映射类型、字节序和字序编码 Modbus 寄存器值。
StatusCode encode_modbus_register_value(
    double engineering_value,
    const ModbusRegisterMapping& mapping,
    EncodedModbusRegisters* output,
    std::string* error_message = nullptr);

}  // namespace edge_controller
