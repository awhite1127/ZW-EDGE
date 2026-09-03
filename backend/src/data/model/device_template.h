// 设备模板字段、解析器、写命令和内置模板管理模型。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "shared/common/status_code.h"
#include "shared/common/types.h"

namespace edge_controller {

struct DeviceTemplateEnumItemDefinition {
    std::int64_t value{0};
    std::string label;
    int sort_order{0};
};

struct DeviceTemplateFieldDefinition {
    std::string field_key;
    std::string display_name;
    std::string parser_id{"scaled_uint16"};
    std::string unit;
    std::string data_type{"uint16"};
    RegisterAddress register_offset{0};
    RegisterCount register_count{1};
    double scale{1.0};
    double value_offset{0.0};
    std::uint32_t precision{0};
    bool summary{true};
    std::uint32_t display_order{0};
    // 自定义模板的字段无效规则基于解码后的原始数值，在 scale/value_offset 之前判定；
    // 内置产品模板不配置此规则，其有效性由统一的设备数值状态模块判定。
    std::string invalid_rule_type{"none"};
    double invalid_rule_value{0.0};
    double invalid_rule_min{0.0};
    double invalid_rule_max{0.0};
    bool show_in_realtime{true};
    // Modbus 32 位字段的字节序与字序；16 位字段保持默认标准大端排列。
    std::string byte_order{"big_endian"};
    std::string word_order{"high_word_first"};
    // 字段偏移相对于所属读取区块；配置必须显式绑定有效区块。
    std::string read_block_key{};
    // bool/bit_uint16 使用 0～15；其他字段固定为 -1，避免残留位配置。
    int bit_index{-1};
    // 仅整数与单比特字段可配置；映射只改变显示文本，不改变数值。
    std::vector<DeviceTemplateEnumItemDefinition> enum_items{};
    // 历史记录能力独立于关键数据(summary)和实时展示。
    bool history_enabled{true};
    // 仅用于实时监控展示组织；不参与采集、历史、告警或北向输出。
    std::string realtime_group_id;
};

// 判断设备类型字段是否应在实时页面显示。
inline bool device_template_field_show_in_realtime(const DeviceTemplateFieldDefinition& field)
{
    return field.show_in_realtime;
}

// 判断设备类型字段是否启用历史记录。
inline bool device_template_field_history_enabled(const DeviceTemplateFieldDefinition& field)
{
    return field.history_enabled;
}

struct DeviceTemplateWriteCommandOption {
    std::string label;
    std::uint16_t value{0};
};

struct DeviceTemplateWriteCommandField {
    std::string key;
    std::string label;
    std::string type{"uint16"};
    std::uint16_t min{0};
    std::uint16_t max{65535};
    std::string unit;
    bool has_default_value{false};
    std::uint16_t default_value{0};
    std::vector<DeviceTemplateWriteCommandOption> options;
};

struct DeviceTemplateWriteCommandDefinition {
    std::string key;
    std::string name;
    std::string description;
    std::string group;
    std::uint32_t function_code{16};
    RegisterAddress register_offset{0};
    RegisterCount register_count{1};
    bool has_absolute_register{false};
    RegisterAddress absolute_register{0};
    std::vector<std::uint16_t> fixed_values;
    std::vector<DeviceTemplateWriteCommandField> value_fields;
    std::vector<std::string> warnings;
    bool require_confirm{false};
    std::string confirm_text;
    std::string success_hint;
};

// 设备类型内一个稳定标识的连续 Modbus 读取区块。
struct DeviceTemplateReadBlockDefinition {
    std::string block_key;
    std::string display_name;
    std::uint32_t function_code{3};
    std::uint32_t start_offset{0};
    std::uint32_t register_count{1};
    int sort_order{0};
};

struct DeviceTemplateRealtimeGroupDefinition {
    std::string group_id;
    std::string display_name;
    int sort_order{0};
};

struct DeviceTemplateDefinition {
    std::string template_id;
    std::string display_name;
    std::string description;
    RegisterAddress default_start_register{0};
    bool builtin{false};
    std::vector<DeviceTemplateFieldDefinition> fields;
    std::vector<DeviceTemplateWriteCommandDefinition> write_commands;
    // 相邻设备基地址之间的距离；读取区块偏移均相对于该设备基地址。
    std::uint32_t device_address_stride{1};
    // 运行时与配置页面读取模型的权威区块集合。
    std::vector<DeviceTemplateReadBlockDefinition> read_blocks{};
    bool realtime_grouping_enabled{false};
    std::vector<DeviceTemplateRealtimeGroupDefinition> realtime_groups{};
};

// 校验读取区块 FC03/FC04 的共享辅助。
inline bool device_template_read_function_code_supported(std::uint32_t function_code)
{
    return function_code == 3 || function_code == 4;
}

// 内置单区块设备类型使用的稳定区块标识和显示名称。
const std::string& default_device_template_read_block_key();
// 返回默认读取区块的显示名称。
const std::string& default_device_template_read_block_display_name();

// 校验读取区块、设备地址跨度、字段所属区块和区块内字段范围/重叠。
StatusCode validate_device_template_read_model(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message);

// 校验内置设备受控 FC10 命令定义的关键结构与地址边界。
StatusCode validate_device_template_write_commands(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message);

// 校验字段级无效规则的稳定类型、有限数和闭区间边界。
StatusCode validate_device_template_field_invalid_rule(
    const DeviceTemplateFieldDefinition& field,
    std::string* error_message);

// 校验单比特字段和整数枚举的类型范围、规范值与结构约束。
StatusCode validate_device_template_field_bit_and_enum(
    const DeviceTemplateFieldDefinition& field,
    std::string* error_message);

// 在解码后的原始值上判断字段是否无效；调用前应先通过规则定义校验。
bool device_template_field_raw_value_invalid(
    const DeviceTemplateFieldDefinition& field,
    double raw_value,
    std::string* reason);

// 返回当前注册的全部设备类型定义。
std::vector<DeviceTemplateDefinition> device_templates();
// 设置设备模板。
void set_device_templates(std::vector<DeviceTemplateDefinition> templates);
// 返回设备模板注册表世代；每次替换注册表后单调递增，供运行时缓存按需刷新。
std::uint64_t device_template_registry_generation();
// 查找设备模板。
// 返回带快照所有权的模板引用；注册表热更新后，调用方正在使用的旧定义仍然有效。
std::shared_ptr<const DeviceTemplateDefinition> find_device_template(const std::string& template_id);

}  // namespace edge_controller
