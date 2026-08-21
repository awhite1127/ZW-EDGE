// Modbus 北向服务配置、寄存器映射和稳定质量码模型。
// 本文件只拥有稳定配置数据定义；Socket 生命周期和运行状态位于 modbus_server 运行时组件。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "common/types.h"

namespace edge_controller {

inline constexpr std::uint16_t kDefaultModbusServerPort = 1502;
inline constexpr std::uint8_t kDefaultModbusServerUnitId = 1;
inline constexpr std::uint32_t kDefaultModbusServerMaxClients = 8;
inline constexpr std::uint32_t kDefaultModbusServerIdleTimeoutSeconds = 60;
inline constexpr std::uint16_t kDefaultModbusServerMaxReadRegisters = 125;

struct ModbusServerSettings {
    bool enabled{false};
    std::string listen_address{"0.0.0.0"};
    std::uint16_t listen_port{kDefaultModbusServerPort};
    std::uint8_t unit_id{kDefaultModbusServerUnitId};
    bool strict_unit_id{false};
    std::uint32_t max_clients{kDefaultModbusServerMaxClients};
    std::uint32_t idle_timeout_seconds{kDefaultModbusServerIdleTimeoutSeconds};
    std::uint16_t max_read_registers{kDefaultModbusServerMaxReadRegisters};
};

enum class ModbusRegisterDataType {
    kUint16,
    kInt16,
    kUint32,
    kInt32,
    kFloat32,
};

enum class ModbusByteOrder {
    kBigEndian,
    kLittleEndian,
};

enum class ModbusWordOrder {
    kHighWordFirst,
    kLowWordFirst,
};

// 数值会直接写入质量寄存器，禁止重排或复用。
enum class ModbusExportQuality : std::uint16_t {
    kGood = 0,
    kNotProduced = 1,
    kStale = 2,
    kDeviceOffline = 3,
    kPointInvalid = 4,
    kTargetMissing = 5,
    kEncodeFailed = 6,
    kDisabled = 7,
};

static_assert(static_cast<std::uint16_t>(ModbusExportQuality::kGood) == 0, "quality code is persistent");
static_assert(static_cast<std::uint16_t>(ModbusExportQuality::kDisabled) == 7, "quality code is persistent");

struct ModbusRegisterMapping {
    std::string mapping_id;
    DeviceId device_id;
    std::string point_key;
    std::string device_name_snapshot;
    std::string point_name_snapshot;
    RegisterAddress start_address{0};
    ModbusRegisterDataType data_type{ModbusRegisterDataType::kUint16};
    double value_multiplier{1.0};
    double value_offset{0.0};
    ModbusByteOrder byte_order{ModbusByteOrder::kBigEndian};
    ModbusWordOrder word_order{ModbusWordOrder::kHighWordFirst};
    RegisterAddress quality_address{0};
    bool enabled{true};
    TimestampMs created_at_ms{0};
    TimestampMs updated_at_ms{0};
};

// 当前设备拓扑中可作为北向映射来源的稳定点位描述。名称只用于显示和快照，引用始终使用 device_id + point_key。
struct ModbusExportablePoint {
    DeviceId device_id;
    std::string device_name;
    std::string device_type_id;
    std::string device_type_name;
    std::string point_key;
    std::string point_name;
    std::string unit;
    bool summary{false};
};

// 返回 Modbus 寄存器数据类型的稳定名称。
const char* to_string(ModbusRegisterDataType value);
// 返回 Modbus 字节序的稳定名称。
const char* to_string(ModbusByteOrder value);
// 返回 Modbus 字序的稳定名称。
const char* to_string(ModbusWordOrder value);
// 解析 Modbus 寄存器数据类型。
bool parse_modbus_register_data_type(const std::string& value, ModbusRegisterDataType* output);
// 解析 Modbus 字节序。
bool parse_modbus_byte_order(const std::string& value, ModbusByteOrder* output);
// 解析 Modbus 字序。
bool parse_modbus_word_order(const std::string& value, ModbusWordOrder* output);
// 返回 Modbus 数据类型占用的寄存器数量。
std::uint16_t modbus_data_register_count(ModbusRegisterDataType type);

// 规范化 Modbus 服务设置。
void normalize_modbus_server_settings(ModbusServerSettings* settings);
// 校验 Modbus 服务设置。
StatusCode validate_modbus_server_settings(
    const ModbusServerSettings& settings,
    const std::string& source,
    std::string* error_message = nullptr);
// 规范化 Modbus 寄存器映射。
void normalize_modbus_register_mapping(ModbusRegisterMapping* mapping);
// 校验单条 Modbus 寄存器映射。
StatusCode validate_modbus_register_mapping(
    const ModbusRegisterMapping& mapping,
    const std::string& source,
    std::string* error_message = nullptr);
// 校验全部 Modbus 寄存器映射及地址冲突。
StatusCode validate_modbus_register_mappings(
    const std::vector<ModbusRegisterMapping>& mappings,
    std::string* error_message = nullptr);

}  // namespace edge_controller
