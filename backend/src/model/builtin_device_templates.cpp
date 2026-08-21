// 汇总有线与无线内置设备定义，并提供共享构造辅助。
#include "model/builtin_device_templates.h"

#include <utility>

#include "model/builtin_device_templates_internal.h"

namespace edge_controller {

namespace builtin_detail {

DeviceTemplateFieldDefinition make_builtin_scalar_field(
    const std::string& field_key,
    const std::string& display_name,
    const std::string& parser_id,
    const std::string& unit,
    const std::string& data_type,
    RegisterAddress register_offset,
    RegisterCount register_count,
    double scale,
    double value_offset,
    std::uint32_t precision,
    bool summary,
    bool show_in_realtime,
    bool history_enabled,
    std::uint32_t display_order,
    const std::string& invalid_rule_type,
    double invalid_rule_value,
    double invalid_rule_min,
    double invalid_rule_max)
{
    DeviceTemplateFieldDefinition field;
    field.field_key = field_key;
    field.display_name = display_name;
    field.parser_id = parser_id;
    field.unit = unit;
    field.data_type = data_type;
    field.register_offset = register_offset;
    field.register_count = register_count;
    field.scale = scale;
    field.value_offset = value_offset;
    field.precision = precision;
    field.summary = summary;
    field.display_order = display_order;
    field.invalid_rule_type = invalid_rule_type;
    field.invalid_rule_value = invalid_rule_value;
    field.invalid_rule_min = invalid_rule_min;
    field.invalid_rule_max = invalid_rule_max;
    field.show_in_realtime = show_in_realtime;
    field.byte_order = "big_endian";
    field.word_order = "high_word_first";
    field.read_block_key.clear();
    field.bit_index = -1;
    field.enum_items.clear();
    field.history_enabled = history_enabled;
    field.realtime_group_id.clear();
    return field;
}

DeviceTemplateDefinition make_builtin_single_read_block_template(
    std::string template_id,
    std::string display_name,
    std::string description,
    RegisterAddress default_start_register,
    std::uint32_t register_count,
    std::vector<DeviceTemplateFieldDefinition> fields,
    std::vector<DeviceTemplateWriteCommandDefinition> write_commands)
{
    for (auto& field : fields) {
        field.read_block_key = default_device_template_read_block_key();
    }
    return DeviceTemplateDefinition{
        std::move(template_id),
        std::move(display_name),
        std::move(description),
        default_start_register,
        true,
        std::move(fields),
        std::move(write_commands),
        register_count,
        {{default_device_template_read_block_key(),
          default_device_template_read_block_display_name(),
          3,
          0,
          register_count,
          0}},
    };
}

}  // namespace builtin_detail

// 构造内置设备模板列表。
std::vector<DeviceTemplateDefinition> builtin_device_templates()
{
    std::vector<DeviceTemplateDefinition> result{
        default_device_template(),
        communication_manager_rd100_device_template(),
        r2_temperature_point_device_template(),
        r2a_0000h_device_template(),
        r2a_1000h_device_template(),
        r2a_2000h_device_template(),
        r4_rd_1000h_device_template(),
        r4_wt_1000h_device_template(),
        r4_rd_2000h_device_template(),
        r4_wt_2000h_device_template(),
        communication_manager_sf6_device_template(),
        communication_manager_wireless_temperature_device_template(),
        em100_insulation_monitor_device_template(),
        em100h_insulation_monitor_device_template(),
    };
    return result;
}

}  // namespace edge_controller
