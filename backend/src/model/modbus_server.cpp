#include "model/modbus_server.h"

#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace edge_controller {
namespace {

// 返回去除 ASCII 首尾空白的字符串副本。
std::string trim_ascii_copy(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return value.substr(begin, end - begin);
}

// 校验 IPv4 地址格式是否合法。
bool is_ipv4_address(const std::string& value)
{
    std::size_t begin = 0;
    for (int part_index = 0; part_index < 4; ++part_index) {
        const auto dot = value.find('.', begin);
        const auto end = dot == std::string::npos ? value.size() : dot;
        if (begin == end) return false;
        unsigned int part = 0;
        for (auto index = begin; index < end; ++index) {
            const auto ch = static_cast<unsigned char>(value[index]);
            if (std::isdigit(ch) == 0) return false;
            part = part * 10U + static_cast<unsigned int>(ch - '0');
            if (part > 255U) return false;
        }
        if (part_index < 3) {
            if (dot == std::string::npos) return false;
            begin = dot + 1;
        } else if (dot != std::string::npos) {
            return false;
        }
    }
    return true;
}

// 构造参数无效状态并写入错误信息。
StatusCode invalid(const std::string& message, std::string* error_message)
{
    if (error_message != nullptr) *error_message = message;
    return StatusCode::kInvalidArgument;
}

// 判断 Modbus 寄存器数据类型是否合法。
bool valid_type(ModbusRegisterDataType value)
{
    switch (value) {
    case ModbusRegisterDataType::kUint16:
    case ModbusRegisterDataType::kInt16:
    case ModbusRegisterDataType::kUint32:
    case ModbusRegisterDataType::kInt32:
    case ModbusRegisterDataType::kFloat32:
        return true;
    }
    return false;
}

// 判断 Modbus 字节序是否合法。
bool valid_byte_order(ModbusByteOrder value)
{
    return value == ModbusByteOrder::kBigEndian || value == ModbusByteOrder::kLittleEndian;
}

// 判断 Modbus 字序是否合法。
bool valid_word_order(ModbusWordOrder value)
{
    return value == ModbusWordOrder::kHighWordFirst || value == ModbusWordOrder::kLowWordFirst;
}

}  // namespace

// 将枚举值转换为稳定文本。
const char* to_string(ModbusRegisterDataType value)
{
    switch (value) {
    case ModbusRegisterDataType::kUint16: return "uint16";
    case ModbusRegisterDataType::kInt16: return "int16";
    case ModbusRegisterDataType::kUint32: return "uint32";
    case ModbusRegisterDataType::kInt32: return "int32";
    case ModbusRegisterDataType::kFloat32: return "float32";
    }
    return "unknown";
}

// 将枚举值转换为稳定文本。
const char* to_string(ModbusByteOrder value)
{
    switch (value) {
    case ModbusByteOrder::kBigEndian: return "big_endian";
    case ModbusByteOrder::kLittleEndian: return "little_endian";
    }
    return "unknown";
}

// 将枚举值转换为稳定文本。
const char* to_string(ModbusWordOrder value)
{
    switch (value) {
    case ModbusWordOrder::kHighWordFirst: return "high_word_first";
    case ModbusWordOrder::kLowWordFirst: return "low_word_first";
    }
    return "unknown";
}

// 解析Modbus寄存器数据类型。
bool parse_modbus_register_data_type(const std::string& value, ModbusRegisterDataType* output)
{
    if (output == nullptr) return false;
    if (value == "uint16") *output = ModbusRegisterDataType::kUint16;
    else if (value == "int16") *output = ModbusRegisterDataType::kInt16;
    else if (value == "uint32") *output = ModbusRegisterDataType::kUint32;
    else if (value == "int32") *output = ModbusRegisterDataType::kInt32;
    else if (value == "float32") *output = ModbusRegisterDataType::kFloat32;
    else return false;
    return true;
}

// 解析Modbus字节顺序。
bool parse_modbus_byte_order(const std::string& value, ModbusByteOrder* output)
{
    if (output == nullptr) return false;
    if (value == "big_endian") *output = ModbusByteOrder::kBigEndian;
    else if (value == "little_endian") *output = ModbusByteOrder::kLittleEndian;
    else return false;
    return true;
}

// 解析Modbus字顺序。
bool parse_modbus_word_order(const std::string& value, ModbusWordOrder* output)
{
    if (output == nullptr) return false;
    if (value == "high_word_first") *output = ModbusWordOrder::kHighWordFirst;
    else if (value == "low_word_first") *output = ModbusWordOrder::kLowWordFirst;
    else return false;
    return true;
}

// 返回 Modbus 数据类型占用的寄存器数量。
std::uint16_t modbus_data_register_count(ModbusRegisterDataType type)
{
    switch (type) {
    case ModbusRegisterDataType::kUint16:
    case ModbusRegisterDataType::kInt16:
        return 1;
    case ModbusRegisterDataType::kUint32:
    case ModbusRegisterDataType::kInt32:
    case ModbusRegisterDataType::kFloat32:
        return 2;
    }
    return 0;
}

// 规范化Modbus服务设置。
void normalize_modbus_server_settings(ModbusServerSettings* settings)
{
    if (settings == nullptr) return;
    settings->listen_address = trim_ascii_copy(settings->listen_address);
}

// 校验Modbus服务设置。
StatusCode validate_modbus_server_settings(
    const ModbusServerSettings& settings,
    const std::string& source,
    std::string* error_message)
{
    if (settings.listen_address.empty()) return invalid(source + ".listen_address 不能为空", error_message);
    if (settings.listen_address.size() > 255) return invalid(source + ".listen_address 不能超过 255 字节", error_message);
    if (!is_ipv4_address(settings.listen_address)) return invalid(source + ".listen_address 必须是 IPv4 地址", error_message);
    if (settings.listen_port == 0) return invalid(source + ".listen_port 必须在 1 到 65535 之间", error_message);
    if (settings.max_clients < 1 || settings.max_clients > 64) return invalid(source + ".max_clients 必须在 1 到 64 之间", error_message);
    if (settings.idle_timeout_seconds < 5 || settings.idle_timeout_seconds > 3600) return invalid(source + ".idle_timeout_seconds 必须在 5 到 3600 之间", error_message);
    if (settings.max_read_registers < 1 || settings.max_read_registers > 125) return invalid(source + ".max_read_registers 必须在 1 到 125 之间", error_message);
    return StatusCode::kOk;
}

// 规范化Modbus寄存器映射。
void normalize_modbus_register_mapping(ModbusRegisterMapping* mapping)
{
    if (mapping == nullptr) return;
    mapping->mapping_id = trim_ascii_copy(mapping->mapping_id);
    mapping->device_id = trim_ascii_copy(mapping->device_id);
    mapping->point_key = trim_ascii_copy(mapping->point_key);
    mapping->device_name_snapshot = trim_ascii_copy(mapping->device_name_snapshot);
    mapping->point_name_snapshot = trim_ascii_copy(mapping->point_name_snapshot);
    if (modbus_data_register_count(mapping->data_type) == 1) {
        mapping->word_order = ModbusWordOrder::kHighWordFirst;
    }
}

// 校验Modbus寄存器映射。
StatusCode validate_modbus_register_mapping(
    const ModbusRegisterMapping& mapping,
    const std::string& source,
    std::string* error_message)
{
    if (mapping.mapping_id.empty()) return invalid(source + ".mapping_id 不能为空", error_message);
    if (mapping.mapping_id.size() > 128) return invalid(source + ".mapping_id 不能超过 128 字节", error_message);
    if (mapping.device_id.empty()) return invalid(source + ".device_id 不能为空", error_message);
    if (mapping.device_id.size() > 256) return invalid(source + ".device_id 不能超过 256 字节", error_message);
    if (mapping.point_key.empty()) return invalid(source + ".point_key 不能为空", error_message);
    if (mapping.point_key.size() > 128) return invalid(source + ".point_key 不能超过 128 字节", error_message);
    if (mapping.device_name_snapshot.size() > 256) return invalid(source + ".device_name_snapshot 不能超过 256 字节", error_message);
    if (mapping.point_name_snapshot.size() > 256) return invalid(source + ".point_name_snapshot 不能超过 256 字节", error_message);
    if (!std::isfinite(mapping.value_multiplier) || mapping.value_multiplier == 0.0) return invalid(source + ".value_multiplier 必须是非零有限数", error_message);
    if (!std::isfinite(mapping.value_offset)) return invalid(source + ".value_offset 必须是有限数", error_message);
    if (!valid_type(mapping.data_type)) return invalid(source + ".data_type 不受支持", error_message);
    if (!valid_byte_order(mapping.byte_order)) return invalid(source + ".byte_order 不受支持", error_message);
    if (!valid_word_order(mapping.word_order)) return invalid(source + ".word_order 不受支持", error_message);
    const auto count = modbus_data_register_count(mapping.data_type);
    const auto end_address = static_cast<std::uint32_t>(mapping.start_address) + count;
    if (count == 0 || end_address > 65536U) return invalid(source + ".start_address 的数据寄存器范围超过 65535", error_message);
    const auto quality = static_cast<std::uint32_t>(mapping.quality_address);
    const auto start = static_cast<std::uint32_t>(mapping.start_address);
    if (quality >= start && quality < end_address) return invalid(source + ".quality_address 与本映射的数据地址重叠", error_message);
    if (mapping.created_at_ms == 0) return invalid(source + ".created_at_ms 必须大于 0", error_message);
    if (mapping.created_at_ms > static_cast<TimestampMs>(std::numeric_limits<std::int64_t>::max()) ||
        mapping.updated_at_ms > static_cast<TimestampMs>(std::numeric_limits<std::int64_t>::max())) {
        return invalid(source + " 的时间戳超过 SQLite INTEGER 范围", error_message);
    }
    if (mapping.updated_at_ms < mapping.created_at_ms) return invalid(source + ".updated_at_ms 不能早于 created_at_ms", error_message);
    return StatusCode::kOk;
}

// 校验Modbus寄存器映射。
StatusCode validate_modbus_register_mappings(
    const std::vector<ModbusRegisterMapping>& mappings,
    std::string* error_message)
{
    struct AddressOwner {
        std::int32_t mapping_index{-1};
        bool quality{false};
    };
    std::vector<AddressOwner> owner(65536);
    std::unordered_map<std::string, std::size_t> mapping_ids;
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        const auto& mapping = mappings[index];
        const auto status = validate_modbus_register_mapping(
            mapping, "modbus_register_mappings[" + std::to_string(index) + "]", error_message);
        if (!is_ok(status)) return status;
        const auto inserted = mapping_ids.emplace(mapping.mapping_id, index);
        if (!inserted.second) {
            if (error_message != nullptr) {
                *error_message = "Modbus mapping_id 重复：" + mapping.mapping_id + "（索引 " +
                    std::to_string(inserted.first->second) + " 与 " + std::to_string(index) + "）";
            }
            return StatusCode::kInvalidArgument;
        }

        const auto reserve = [&](std::uint32_t address, bool quality) -> StatusCode {
            const auto previous = owner[address];
            if (previous.mapping_index >= 0) {
                if (error_message != nullptr) {
                    const auto& existing = mappings[static_cast<std::size_t>(previous.mapping_index)];
                    const auto describe = [](const ModbusRegisterMapping& item) {
                        const auto name = item.device_name_snapshot + " / " + item.point_name_snapshot;
                        return name == " / " ? item.mapping_id : name + "（" + item.mapping_id + "）";
                    };
                    *error_message = "Modbus 寄存器地址 " + std::to_string(address) + " 冲突：当前映射 " +
                        describe(mapping) + " 的" + (quality ? "质量地址" : "数据地址") + "与已存在映射 " +
                        describe(existing) + " 的" + (previous.quality ? "质量地址" : "数据地址") + "重叠";
                }
                return StatusCode::kConflict;
            }
            owner[address] = AddressOwner{static_cast<std::int32_t>(index), quality};
            return StatusCode::kOk;
        };

        const auto count = modbus_data_register_count(mapping.data_type);
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            const auto status_at_address = reserve(static_cast<std::uint32_t>(mapping.start_address) + offset, false);
            if (!is_ok(status_at_address)) return status_at_address;
        }
        const auto quality_status = reserve(mapping.quality_address, true);
        if (!is_ok(quality_status)) return quality_status;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
