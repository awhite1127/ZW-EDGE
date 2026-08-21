// 无线内置设备定义。
#include "model/builtin_device_templates.h"

#include <string>

#include "model/builtin_device_templates_internal.h"
#include "model/data_item_keys.h"

namespace edge_controller {

namespace {

using builtin_detail::make_builtin_scalar_field;
using builtin_detail::make_builtin_single_read_block_template;

// 构造 R2 温度点位字段。
DeviceTemplateFieldDefinition r2_temperature_field()
{
    return make_builtin_scalar_field(
        "temperature",
        "温度",
        "scaled_uint16",
        "℃",
        "uint16",
        0,
        1,
        0.1,
        -40.0,
        1,
        true,
        true,
        true,
        1,
        "greater_or_equal",
        65535.0,
        0.0,
        0.0);
}

// 构造 R2 电池电压字段。
DeviceTemplateFieldDefinition r2_battery_voltage_field()
{
    return make_builtin_scalar_field(
        "battery_voltage",
        "电池电压",
        "scaled_high_uint8",
        "V",
        "uint16",
        1,
        1,
        0.1,
        0.0,
        1,
        true,
        true,
        true,
        2,
        "none",
        0.0,
        0.0,
        0.0);
}

// 构造 R2 信号强度字段。
DeviceTemplateFieldDefinition r2_signal_strength_field()
{
    return make_builtin_scalar_field(
        "signal_strength",
        "信号强度",
        "scaled_low_uint8",
        "dBm",
        "uint16",
        1,
        1,
        -1.0,
        0.0,
        0,
        true,
        true,
        true,
        3,
        "none",
        0.0,
        0.0,
        0.0);
}

// 构造 R2A 电阻点位字段。
DeviceTemplateFieldDefinition r2a_resistance_field()
{
    return make_builtin_scalar_field(
        kResistanceFieldKey,
        "接地电阻",
        "scaled_uint16",
        "Ω",
        "uint16",
        0,
        1,
        0.01,
        0.0,
        2,
        true,
        true,
        true,
        1,
        "greater_or_equal",
        65520.0,
        0.0,
        0.0);
}

// 构造 R2A 电池电压字段。
DeviceTemplateFieldDefinition r2a_battery_voltage_field(
    const std::string& parser_id,
    RegisterAddress register_offset,
    std::uint32_t display_order)
{
    return make_builtin_scalar_field(
        "battery_voltage",
        "电池电压",
        parser_id,
        "V",
        "uint16",
        register_offset,
        1,
        0.1,
        0.0,
        1,
        true,
        true,
        true,
        display_order,
        "greater_or_equal",
        65520.0,
        0.0,
        0.0);
}

// 构造 R2A 信号强度字段。
DeviceTemplateFieldDefinition r2a_signal_strength_field(
    const std::string& parser_id,
    RegisterAddress register_offset,
    std::uint32_t display_order)
{
    return make_builtin_scalar_field(
        "signal_strength",
        "信号强度",
        parser_id,
        "dBm",
        "uint16",
        register_offset,
        1,
        -1.0,
        0.0,
        0,
        true,
        true,
        true,
        display_order,
        "greater_or_equal",
        65520.0,
        0.0,
        0.0);
}

// 构造 R4 电阻点位字段。
DeviceTemplateFieldDefinition r4_resistance_field()
{
    return make_builtin_scalar_field(
        kResistanceFieldKey,
        "接地电阻值",
        "scaled_uint16",
        "Ω",
        "uint16",
        0,
        1,
        0.01,
        0.0,
        2,
        true,
        true,
        true,
        1,
        "greater_or_equal",
        65520.0,
        0.0,
        0.0);
}

// 构造 R4 温度点位字段。
DeviceTemplateFieldDefinition r4_temperature_field()
{
    return make_builtin_scalar_field(
        "temperature",
        "温度",
        "scaled_uint16",
        "℃",
        "uint16",
        0,
        1,
        0.1,
        -40.0,
        1,
        true,
        true,
        true,
        1,
        "greater_or_equal",
        65520.0,
        0.0,
        0.0);
}

// 构造 R4 电池电压字段。
DeviceTemplateFieldDefinition r4_battery_voltage_field()
{
    return make_builtin_scalar_field(
        "battery_voltage",
        "电池电压",
        "scaled_uint16",
        "V",
        "uint16",
        1,
        1,
        0.1,
        0.0,
        1,
        true,
        true,
        true,
        2,
        "none",
        0.0,
        0.0,
        0.0);
}

// 构造 R4 信号强度字段。
DeviceTemplateFieldDefinition r4_signal_strength_field()
{
    return make_builtin_scalar_field(
        "signal_strength",
        "信号强度",
        "scaled_uint16",
        "dBm",
        "uint16",
        2,
        1,
        -1.0,
        0.0,
        0,
        true,
        true,
        true,
        3,
        "none",
        0.0,
        0.0,
        0.0);
}

}  // namespace

// 返回 R2 温度点内置设备类型。
const DeviceTemplateDefinition& r2_temperature_point_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R2",
        "R2测温",
        "R2 接收模块下单个测温点模板。每个测温点占 2 个保持寄存器：第一个寄存器为温度，第二个寄存器高字节为电池电压、低字节为信号强度。",
        3,
        2,
        {
            r2_temperature_field(),
            r2_battery_voltage_field(),
            r2_signal_strength_field(),
        });
    return value;
}

// 返回 R2A 接地电阻 0000H 内置设备类型。
const DeviceTemplateDefinition& r2a_0000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R2A-0000H",
        "R2A-0000H",
        "R2A 接地电阻监测设备 0000H 直读表模板。寄存器 0、1、2 为备用，默认从寄存器 3 开始；每个监测点占 2 个保持寄存器：接地电阻，以及电池电压/信号强度组合寄存器。",
        3,
        2,
        {
            r2a_resistance_field(),
            r2a_battery_voltage_field("scaled_high_uint8", 1, 2),
            r2a_signal_strength_field("scaled_low_uint8", 1, 3),
        });
    return value;
}

// 返回 R2A 接地电阻 1000H 内置设备类型。
const DeviceTemplateDefinition& r2a_1000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R2A-1000H",
        "R2A-1000H",
        "R2A 接地电阻监测设备 1000H 接地电阻快速表模板。每个监测点占 1 个保持寄存器，只包含接地电阻值。",
        4096,
        1,
        {
            r2a_resistance_field(),
        });
    return value;
}

// 返回 R2A 接地电阻 2000H 内置设备类型。
const DeviceTemplateDefinition& r2a_2000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R2A-2000H",
        "R2A-2000H",
        "R2A 接地电阻监测设备 2000H 详细数据表模板。每个监测点占 3 个保持寄存器：接地电阻、电池电压、信号强度。",
        8192,
        3,
        {
            r2a_resistance_field(),
            r2a_battery_voltage_field("scaled_uint16", 1, 2),
            r2a_signal_strength_field("scaled_uint16", 2, 3),
        });
    return value;
}

// 返回 R4 接地电阻 1000H 内置设备类型。
const DeviceTemplateDefinition& r4_rd_1000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R4-RD-1000H",
        "R4接地电阻值",
        "R4 1000H 基础数据值区，单点 1 寄存器，接地电阻按两位小数解析，单次读取寄存器数量建议不超过 70。原始值乘以 0.01；原始值大于等于 0xFFF0 时表示数据无效或设备故障。R4 参数区从 0xF100/0xF200 开始，本模板不维护参数；参数维护请依据厂家工具或完整维护规约执行。",
        4096,
        1,
        {
            r4_resistance_field(),
        });
    return value;
}

// 返回 R4 无线测温 1000H 内置设备类型。
const DeviceTemplateDefinition& r4_wt_1000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R4-WT-1000H",
        "R4无线测温值",
        "R4 1000H 基础数据值区，单点 1 寄存器，温度按 raw 减 400 后再除以 10 解析，单次读取寄存器数量建议不超过 70。温度 = (原始值 - 400) / 10；原始值大于等于 0xFFF0 时表示测温点故障。R4 参数区从 0xF100/0xF200 开始，本模板不维护参数；参数维护请依据厂家工具或完整维护规约执行。",
        4096,
        1,
        {
            r4_temperature_field(),
        });
    return value;
}

// 返回 R4 接地电阻 2000H 内置设备类型。
const DeviceTemplateDefinition& r4_rd_2000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R4-RD-2000H",
        "R4接地电阻详细",
        "R4 2000H 详细数据区，单点 3 寄存器，字段依次为接地电阻值、电池电压、信号强度。接地电阻原始值乘以 0.01；电池电压原始值乘以 0.1；信号强度原始值乘以 -1。R4 参数区从 0xF100/0xF200 开始，本模板不维护参数；参数维护请依据厂家工具或完整维护规约执行。",
        8192,
        3,
        {
            r4_resistance_field(),
            r4_battery_voltage_field(),
            r4_signal_strength_field(),
        });
    return value;
}

// 返回 R4 无线测温 2000H 内置设备类型。
const DeviceTemplateDefinition& r4_wt_2000h_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "R4-WT-2000H",
        "R4无线测温详细",
        "R4 2000H 详细数据区，单点 3 寄存器，字段依次为温度、电池电压、信号强度。温度 = (原始值 - 400) / 10；电池电压原始值乘以 0.1；信号强度原始值乘以 -1。R4 参数区从 0xF100/0xF200 开始，本模板不维护参数；参数维护请依据厂家工具或完整维护规约执行。",
        8192,
        3,
        {
            r4_temperature_field(),
            r4_battery_voltage_field(),
            r4_signal_strength_field(),
        });
    return value;
}

}  // namespace edge_controller
