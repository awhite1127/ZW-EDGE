#include "modbus_server/modbus_register_encoder.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace edge_controller {
namespace {

// 写入错误信息并返回失败状态。
StatusCode fail(const std::string& message, std::string* error_message)
{
    if (error_message != nullptr) *error_message = message;
    return StatusCode::kInvalidArgument;
}

// 交换 16 位寄存器的高低字节。
std::uint16_t swap_bytes(std::uint16_t value)
{
    return static_cast<std::uint16_t>((value << 8U) | (value >> 8U));
}

// 将 16 位数值编码为 Modbus 寄存器。
void encode_16(std::uint16_t bits, ModbusByteOrder byte_order, EncodedModbusRegisters* output)
{
    output->registers[0] = byte_order == ModbusByteOrder::kLittleEndian ? swap_bytes(bits) : bits;
    output->registers[1] = 0;
    output->register_count = 1;
}

// 按字节序和字序编码 32 位数值。
void encode_32(
    std::uint32_t bits,
    ModbusByteOrder byte_order,
    ModbusWordOrder word_order,
    EncodedModbusRegisters* output)
{
    auto high = static_cast<std::uint16_t>(bits >> 16U);
    auto low = static_cast<std::uint16_t>(bits & 0xFFFFU);
    if (byte_order == ModbusByteOrder::kLittleEndian) {
        high = swap_bytes(high);
        low = swap_bytes(low);
    }
    if (word_order == ModbusWordOrder::kLowWordFirst) {
        output->registers[0] = low;
        output->registers[1] = high;
    } else {
        output->registers[0] = high;
        output->registers[1] = low;
    }
    output->register_count = 2;
}

}  // namespace

// 编码Modbus寄存器值。
StatusCode encode_modbus_register_value(
    double engineering_value,
    const ModbusRegisterMapping& mapping,
    EncodedModbusRegisters* output,
    std::string* error_message)
{
    if (output == nullptr) return fail("Modbus 编码输出参数为空", error_message);
    *output = EncodedModbusRegisters{};
    if (!std::isfinite(engineering_value)) return fail("Modbus 编码输入值不是有限数", error_message);
    if (!std::isfinite(mapping.value_multiplier) || mapping.value_multiplier == 0.0 ||
        !std::isfinite(mapping.value_offset)) {
        return fail("Modbus 编码 multiplier 必须是非零有限数，offset 必须是有限数", error_message);
    }
    if ((mapping.byte_order != ModbusByteOrder::kBigEndian &&
         mapping.byte_order != ModbusByteOrder::kLittleEndian) ||
        (mapping.word_order != ModbusWordOrder::kHighWordFirst &&
         mapping.word_order != ModbusWordOrder::kLowWordFirst)) {
        return fail("Modbus 编码字节序或字序不受支持", error_message);
    }
    const auto transformed = engineering_value * mapping.value_multiplier + mapping.value_offset;
    if (!std::isfinite(transformed)) return fail("Modbus 编码变换结果不是有限数", error_message);

    // 所有整数类型固定使用 std::round：恰好位于中间的值向远离 0 的方向舍入。
    switch (mapping.data_type) {
    case ModbusRegisterDataType::kUint16: {
        const auto rounded = std::round(transformed);
        if (rounded < 0.0 || rounded > static_cast<double>(std::numeric_limits<std::uint16_t>::max())) {
            return fail("Modbus uint16 编码整数越界", error_message);
        }
        encode_16(static_cast<std::uint16_t>(rounded), mapping.byte_order, output);
        return StatusCode::kOk;
    }
    case ModbusRegisterDataType::kInt16: {
        const auto rounded = std::round(transformed);
        if (rounded < static_cast<double>(std::numeric_limits<std::int16_t>::min()) ||
            rounded > static_cast<double>(std::numeric_limits<std::int16_t>::max())) {
            return fail("Modbus int16 编码整数越界", error_message);
        }
        const auto signed_value = static_cast<std::int16_t>(rounded);
        encode_16(static_cast<std::uint16_t>(signed_value), mapping.byte_order, output);
        return StatusCode::kOk;
    }
    case ModbusRegisterDataType::kUint32: {
        const auto rounded = std::round(transformed);
        if (rounded < 0.0 || rounded > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            return fail("Modbus uint32 编码整数越界", error_message);
        }
        encode_32(static_cast<std::uint32_t>(rounded), mapping.byte_order, mapping.word_order, output);
        return StatusCode::kOk;
    }
    case ModbusRegisterDataType::kInt32: {
        const auto rounded = std::round(transformed);
        if (rounded < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            rounded > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            return fail("Modbus int32 编码整数越界", error_message);
        }
        const auto signed_value = static_cast<std::int32_t>(rounded);
        encode_32(static_cast<std::uint32_t>(signed_value), mapping.byte_order, mapping.word_order, output);
        return StatusCode::kOk;
    }
    case ModbusRegisterDataType::kFloat32: {
        const auto float_value = static_cast<float>(transformed);
        if (!std::isfinite(float_value)) return fail("Modbus float32 编码溢出", error_message);
        std::uint32_t bits = 0;
        static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559,
            "Modbus float32 encoding requires 32-bit IEEE-754 float");
        std::memcpy(&bits, &float_value, sizeof(bits));
        encode_32(bits, mapping.byte_order, mapping.word_order, output);
        return StatusCode::kOk;
    }
    }
    return fail("Modbus 编码数据类型不受支持", error_message);
}

}  // namespace edge_controller
