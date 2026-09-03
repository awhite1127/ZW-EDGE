// 有线内置设备定义。
#include "data/model/builtin_device_templates.h"

#include <cstdint>
#include <string>
#include <vector>

#include "data/model/builtin_device_templates_internal.h"
#include "data/model/data_item_keys.h"

namespace edge_controller {

namespace {

using builtin_detail::make_builtin_scalar_field;
using builtin_detail::make_builtin_single_read_block_template;

// 构造 RD100 电阻点位字段。
DeviceTemplateFieldDefinition rd100_resistance_field()
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
        1);
}
// 构造通信管理机 SF6 点位字段。
DeviceTemplateFieldDefinition cm_sf6_field(
    const std::string& field_key,
    const std::string& display_name,
    const std::string& unit,
    RegisterAddress register_offset,
    double scale,
    double value_offset,
    std::uint32_t precision,
    std::uint32_t display_order)
{
    return make_builtin_scalar_field(
        field_key,
        display_name,
        "scaled_uint16",
        unit,
        "uint16",
        register_offset,
        1,
        scale,
        value_offset,
        precision,
        true,
        true,
        true,
        display_order);
}

// 构造通信管理机无线测温字段。
DeviceTemplateFieldDefinition cm_wireless_temperature_field(
    const std::string& field_key,
    const std::string& display_name,
    RegisterAddress register_offset,
    std::uint32_t display_order)
{
    return make_builtin_scalar_field(
        field_key,
        display_name,
        "scaled_uint16",
        "℃",
        "uint16",
        register_offset,
        1,
        0.1,
        -40.0,
        1,
        true,
        true,
        true,
        display_order);
}

// 构造 EM100 无符号 16 位点位字段。
DeviceTemplateFieldDefinition em100_uint16_field(
    const std::string& field_key,
    const std::string& display_name,
    RegisterAddress register_offset,
    double scale,
    const std::string& unit,
    std::uint32_t precision,
    bool summary,
    bool show_in_realtime,
    bool history_enabled,
    std::uint32_t display_order)
{
    return make_builtin_scalar_field(
        field_key,
        display_name,
        "scaled_uint16",
        unit,
        "uint16",
        register_offset,
        1,
        scale,
        0.0,
        precision,
        summary,
        show_in_realtime,
        history_enabled,
        display_order);
}

DeviceTemplateFieldDefinition em100h_uint16_field(
    const std::string& field_key,
    const std::string& display_name,
    const std::string& block_key,
    RegisterAddress register_offset,
    double scale,
    const std::string& unit,
    std::uint32_t precision,
    bool summary,
    bool show_in_realtime,
    bool history_enabled,
    const std::string& realtime_group_id,
    std::uint32_t display_order)
{
    auto field = em100_uint16_field(
        field_key, display_name, register_offset, scale, unit, precision,
        summary, show_in_realtime, history_enabled, display_order);
    field.read_block_key = block_key;
    field.realtime_group_id = realtime_group_id;
    return field;
}

DeviceTemplateWriteCommandDefinition em100h_fixed_command(
    const std::string& key,
    const std::string& name,
    const std::string& description,
    RegisterAddress absolute_register,
    std::uint16_t value,
    const std::string& warning)
{
    return DeviceTemplateWriteCommandDefinition{
        key,
        name,
        description,
        "设备控制",
        16,
        0,
        1,
        true,
        absolute_register,
        {value},
        {},
        {warning},
        true,
        "确认执行“" + name + "”吗？",
        name + "命令已发送。",
    };
}

std::vector<DeviceTemplateWriteCommandDefinition> em100h_write_commands()
{
    // EM100H 协议命令键的逻辑值为 0xAA55，但厂家规定该命令键在线路上
    // 以小端字节顺序 55 AA 发送。因此这里按现有标准大端 FC10 编码器的
    // 输入形式保存为 0x55AA；这不是对通用 Modbus 寄存器字节序的修改。
    constexpr std::uint16_t kEm100hCommandKeyWireOrderWord = 0x55AA;
    return {
        em100h_fixed_command("clear_event_records", "清除事件记录", "清除设备内全部事件记录。", 0xFFE0, kEm100hCommandKeyWireOrderWord, "该操作会清除设备内事件记录，无法撤销。"),
        em100h_fixed_command("clear_test_records", "清除测试记录", "清除设备内全部绝缘测试记录。", 0xFFE1, kEm100hCommandKeyWireOrderWord, "该操作会清除设备内测试记录，无法撤销。"),
        em100h_fixed_command("restore_user_correction", "恢复用户校正数据", "清除用户校正数据并恢复设备基准。", 0xFFE2, kEm100hCommandKeyWireOrderWord, "执行后用户校正数据将被清除。"),
        em100h_fixed_command("start_auto_insulation_test", "启动自动绝缘测试", "启动设备自动绝缘测试流程。", 0xFFE6, kEm100hCommandKeyWireOrderWord, "执行前请确认线路与现场设备允许输出高压。"),
        em100h_fixed_command("stop_insulation_test", "停止绝缘测试", "停止当前绝缘测试流程。", 0xFFEE, kEm100hCommandKeyWireOrderWord, "请确认需要停止当前绝缘测试。"),
        em100h_fixed_command("start_aging_mode", "启动老化模式", "持续输出高压 48 小时。", 0xFFEF, kEm100hCommandKeyWireOrderWord, "危险操作：设备将持续输出高压 48 小时，请确认现场已满足老化测试安全条件。"),
    };
}

// 构造 EM100 无符号 16 位命令输入字段。
DeviceTemplateWriteCommandField em100_uint16_command_field(
    const std::string& key,
    const std::string& label,
    const std::string& unit)
{
    return DeviceTemplateWriteCommandField{
        key,
        label,
        "uint16",
        0,
        65535,
        unit,
        false,
        0,
        {},
    };
}

// 构造 EM100 参数设置命令定义。
DeviceTemplateWriteCommandDefinition em100_setting_command(
    const std::string& key,
    const std::string& name,
    const std::string& description,
    RegisterAddress absolute_register,
    const DeviceTemplateWriteCommandField& field,
    const std::string& default_hint = "")
{
    std::vector<std::string> warnings;
    if (!default_hint.empty()) {
        warnings.push_back(default_hint);
    }
    return DeviceTemplateWriteCommandDefinition{
        key,
        name,
        description,
        "系统设置",
        16,
        static_cast<RegisterAddress>(absolute_register - 0x1000U),
        1,
        true,
        absolute_register,
        {},
        std::vector<DeviceTemplateWriteCommandField>{field},
        warnings,
        false,
        "",
        "设备已返回设置写入确认。",
    };
}

// 构造 EM100 远程控制命令定义。
DeviceTemplateWriteCommandDefinition em100_remote_command(
    const std::string& key,
    const std::string& name,
    const std::string& description,
    RegisterAddress absolute_register,
    const std::string& group,
    const std::vector<std::string>& warnings,
    const std::string& success_hint,
    bool require_confirm = false,
    const std::string& confirm_text = "")
{
    return DeviceTemplateWriteCommandDefinition{
        key,
        name,
        description,
        group,
        16,
        0,
        1,
        true,
        absolute_register,
        std::vector<std::uint16_t>{0x55AA},
        {},
        warnings,
        require_confirm,
        confirm_text,
        success_hint,
    };
}

// 构造 EM100 支持的写命令列表。
std::vector<DeviceTemplateWriteCommandDefinition> em100_write_commands()
{
    return {
        em100_setting_command(
            "set_system_definition",
            "系统定义",
            "用位表达系统定义。当前按原始 WORD 值写入，后续可扩展为 bit 表单。",
            0xF012,
            em100_uint16_command_field("system_definition", "系统定义", "")),
        em100_setting_command(
            "set_start_delay_after_stop",
            "停机后延时启动测量",
            "停机后延时指定分钟数后开始测量",
            0xF013,
            em100_uint16_command_field("delay_minutes", "延时时间", "分钟")),
        em100_setting_command(
            "set_measure_interval",
            "第一次测量后间隔测量",
            "第一次测量后按指定间隔继续测量",
            0xF014,
            em100_uint16_command_field("interval_minutes", "测量间隔", "分钟")),
        em100_setting_command(
            "set_insulation_alarm_threshold",
            "绝缘电阻报警值",
            "绝缘电阻低于该值时报警",
            0xF015,
            em100_uint16_command_field("threshold", "报警值", "MΩ")),
        em100_setting_command(
            "set_residual_current_alarm_threshold",
            "剩余电流报警限值",
            "剩余电流超过该值时报警",
            0xF016,
            em100_uint16_command_field("threshold", "报警限值", "mA")),
        em100_setting_command(
            "set_insulation_correction_x0",
            "绝缘电阻值矫正系数 X0",
            "公式 X=KX+X0。当前按 16 位原始寄存器值写入；如需写入负数，请使用对应的 16 位补码值。",
            0xF017,
            em100_uint16_command_field("x0", "X0", "")),
        em100_setting_command(
            "set_insulation_correction_k",
            "绝缘电阻值矫正系数 K",
            "设置绝缘电阻值矫正系数 K",
            0xF018,
            em100_uint16_command_field("k", "K", "")),
        em100_setting_command(
            "set_residual_current_correction",
            "剩余电流值矫正系数",
            "设置剩余电流值矫正系数",
            0xF019,
            em100_uint16_command_field("coefficient", "矫正系数", "")),
        em100_setting_command(
            "set_min_discharge_ac_voltage",
            "放电时最小交流电压",
            "设置放电时最小的交流电压",
            0xF01A,
            em100_uint16_command_field("voltage", "交流电压", "V")),
        em100_setting_command(
            "set_min_discharge_dc_voltage",
            "放电时最小直流电压",
            "设置放电时最小的直流电压",
            0xF01B,
            em100_uint16_command_field("voltage", "直流电压", "V")),
        em100_setting_command(
            "set_min_discharge_time",
            "放电最短时间",
            "设置放电的最短时间",
            0xF01C,
            em100_uint16_command_field("minutes", "最短时间", "分钟"),
            "默认 5 分钟"),
        em100_setting_command(
            "set_max_discharge_time",
            "放电最长时间",
            "设置放电的最长时间",
            0xF01D,
            em100_uint16_command_field("minutes", "最长时间", "分钟"),
            "默认 10 分钟"),
        em100_remote_command(
            "clear_event_records",
            "清除事件记录",
            "清除设备内事件记录，执行后不可恢复。",
            0xFFE0,
            "记录维护",
            {
                "该操作会清除设备内事件记录，执行后不可恢复。",
            },
            "已下发清除事件记录命令。",
            true,
            "清除事件记录，执行后不可恢复"),
        em100_remote_command(
            "clear_test_records",
            "清除测试记录",
            "清除设备内测试记录，执行后不可恢复。",
            0xFFE1,
            "记录维护",
            {
                "该操作会清除设备内测试记录，执行后不可恢复。",
            },
            "已下发清除测试记录命令。",
            true,
            "清除测试记录，执行后不可恢复"),
        em100_remote_command(
            "remote_start_insulation_test",
            "遥控启动绝缘测试",
            "向设备下发启动绝缘测试命令",
            0xFFE6,
            "遥控命令",
            {
                "执行前请确认现场设备允许远程启动绝缘测试。",
                "如设备返回错误，请根据设备状态排查。",
            },
            "已下发遥控启动绝缘测试命令。"),
        em100_remote_command(
            "remote_stop_insulation_test",
            "遥控停止绝缘测试",
            "向设备下发停止绝缘测试命令",
            0xFFEE,
            "遥控命令",
            {
                "执行前请确认现场设备允许远程停止绝缘测试。",
            },
            "已下发遥控停止绝缘测试命令。"),
    };
}

}  // namespace

// 返回默认设备类型标识。
const std::string& default_device_template_id()
{
    static const std::string value = "RD100";
    return value;
}

// 返回默认设备类型定义。
const DeviceTemplateDefinition& default_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        default_device_template_id(),
        "接地电阻",
        "RD100 接地电阻采集模板；内置双字节数值状态中，0xFFFF 表示设备故障，0xFFFE 表示设备开路。",
        4096,
        1,
        {
            rd100_resistance_field(),
        });
    return value;
}

// 返回通信管理机 RD100 内置设备类型。
const DeviceTemplateDefinition& communication_manager_rd100_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "CM-RD100",
        "通讯管理机-RD100",
        "通讯管理机映射 RD100 接地电阻监测点模板；内置双字节数值状态中，0xFFFF 表示设备故障，0xFFFE 表示设备开路。",
        4096,
        1,
        {
            rd100_resistance_field(),
        });
    return value;
}
// 返回通信管理机 SF6 内置设备类型。
const DeviceTemplateDefinition& communication_manager_sf6_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "CM-SF6",
        "通讯管理机-SF6",
        "通讯管理机下挂 SF6 传感器模板。每个 SF6 传感器占 4 个保持寄存器：环境温度、环境湿度、氧气含量、SF6 浓度。",
        4096,
        4,
        {
            cm_sf6_field(
                "ambient_temperature",
                "环境温度",
                "℃",
                0,
                0.1,
                -40.0,
                1,
                1),
            cm_sf6_field(
                "ambient_humidity",
                "环境湿度",
                "%RH",
                1,
                0.1,
                0.0,
                1,
                2),
            cm_sf6_field(
                "oxygen_content",
                "氧气含量",
                "%",
                2,
                0.1,
                0.0,
                1,
                3),
            cm_sf6_field(
                "sf6_concentration",
                "SF6浓度",
                "ppm",
                3,
                1.0,
                0.0,
                0,
                4),
        });
    return value;
}

// 返回通信管理机无线测温内置设备类型。
const DeviceTemplateDefinition& communication_manager_wireless_temperature_device_template()
{
    static const DeviceTemplateDefinition value = make_builtin_single_read_block_template(
        "CM-WT",
        "通讯管理机-无线测温",
        "通讯管理机下挂无线测温开关或测温单元模板。每个无线测温设备占 6 个保持寄存器：上触头 A/B/C 和下触头 A/B/C 温度。",
        4096,
        6,
        {
            cm_wireless_temperature_field(
                "upper_contact_a_temperature",
                "上触头A温度",
                0,
                1),
            cm_wireless_temperature_field(
                "upper_contact_b_temperature",
                "上触头B温度",
                1,
                2),
            cm_wireless_temperature_field(
                "upper_contact_c_temperature",
                "上触头C温度",
                2,
                3),
            cm_wireless_temperature_field(
                "lower_contact_a_temperature",
                "下触头A温度",
                3,
                4),
            cm_wireless_temperature_field(
                "lower_contact_b_temperature",
                "下触头B温度",
                4,
                5),
            cm_wireless_temperature_field(
                "lower_contact_c_temperature",
                "下触头C温度",
                5,
                6),
        });
    return value;
}

// 返回 EM100 绝缘监测仪内置设备类型。
const DeviceTemplateDefinition& em100_insulation_monitor_device_template()
{
    static const DeviceTemplateDefinition value = [] {
        auto result = make_builtin_single_read_block_template(
            "EM100",
            "绝缘监测",
            "EM100 绝缘监测设备模板。通过 FC03 读取 1000H~1027H 运行快照，通过 FC10 写入系统设置和遥控命令。",
            4096,
            40,
            {
                em100_uint16_field("insulation_resistance", "绝缘电阻值", 0, 1.0, "MΩ", 0, true, true, true, 1),
                em100_uint16_field("residual_current", "剩余电流值", 1, 1.0, "mA", 0, true, true, true, 2),
                em100_uint16_field("absorption_ratio", "吸收比", 2, 0.01, "", 2, true, true, true, 3),
                em100_uint16_field("polarization_index", "极化指标", 3, 0.01, "", 2, true, true, true, 4),
                em100_uint16_field("current_status", "当前状态（0正常待机，1检测线路残压，2绝缘电阻测试中，3释放线路残压）", 4, 1.0, "", 0, true, true, true, 5),
                em100_uint16_field("event_record_total", "事件记录总数 / 未读事件记录个数", 12, 1.0, "", 0, false, false, false, 20),
                em100_uint16_field("test_record_total", "测试记录总数 / 未读测试记录个数", 13, 1.0, "", 0, false, false, false, 21),
                em100_uint16_field("running_status_flags", "运行状态标志", 14, 1.0, "", 0, false, false, false, 22),
                em100_uint16_field("motor_stop_year", "电机停机时间-年", 15, 1.0, "", 0, false, false, false, 23),
                em100_uint16_field("motor_stop_month", "电机停机时间-月", 16, 1.0, "", 0, false, false, false, 24),
                em100_uint16_field("motor_stop_day", "电机停机时间-日", 17, 1.0, "", 0, false, false, false, 25),
                em100_uint16_field("motor_stop_hour", "电机停机时间-时", 18, 1.0, "", 0, false, false, false, 26),
                em100_uint16_field("motor_stop_minute", "电机停机时间-分", 19, 1.0, "", 0, false, false, false, 27),
                em100_uint16_field("motor_stop_second", "电机停机时间-秒", 20, 1.0, "", 0, false, false, false, 28),
                em100_uint16_field("insulation_measure_duration", "绝缘电阻测量时间", 21, 1.0, "秒", 0, false, false, false, 29),
                em100_uint16_field("discharge_duration", "放电时间", 22, 1.0, "秒", 0, false, false, false, 30),
                em100_uint16_field("insulation_resistance_current", "绝缘电阻当前值", 23, 1.0, "MΩ", 0, false, false, false, 31),
                em100_uint16_field("resistance_15s", "第15秒测量电阻值", 24, 1.0, "MΩ", 0, false, false, false, 32),
                em100_uint16_field("resistance_60s", "第60秒测量电阻值", 25, 1.0, "MΩ", 0, false, false, false, 33),
                em100_uint16_field("resistance_1min", "第1分钟测量电阻值", 26, 1.0, "MΩ", 0, false, false, false, 34),
                em100_uint16_field("resistance_10min", "第10分钟测量电阻值", 27, 1.0, "MΩ", 0, false, false, false, 35),
                em100_uint16_field("internal_hv_dc_voltage", "内部高压模块 DC 电压值", 28, 0.1, "V", 1, false, false, false, 36),
                em100_uint16_field("line_hv_dc_voltage", "线路侧高压 DC 电压值", 29, 0.1, "V", 1, false, false, false, 37),
                em100_uint16_field("line_ac_voltage", "线路侧交流电压值", 30, 1.0, "V", 0, false, false, false, 38),
                em100_uint16_field("line_ac_frequency", "线路侧交流电压频率", 31, 0.01, "Hz", 2, false, false, false, 39),
                em100_uint16_field("last_test_year", "最近一次绝缘测试时间-年", 34, 1.0, "", 0, false, false, false, 50),
                em100_uint16_field("last_test_month", "最近一次绝缘测试时间-月", 35, 1.0, "", 0, false, false, false, 51),
                em100_uint16_field("last_test_day", "最近一次绝缘测试时间-日", 36, 1.0, "", 0, false, false, false, 52),
                em100_uint16_field("last_test_hour", "最近一次绝缘测试时间-时", 37, 1.0, "", 0, false, false, false, 53),
                em100_uint16_field("last_test_minute", "最近一次绝缘测试时间-分", 38, 1.0, "", 0, false, false, false, 54),
                em100_uint16_field("last_test_second", "最近一次绝缘测试时间-秒", 39, 1.0, "", 0, false, false, false, 55),
            },
            em100_write_commands());
        result.fields[4].enum_items = {
            {0, "正常待机状态", 0},
            {1, "检测线路残压", 1},
            {2, "绝缘电阻测试中", 2},
            {3, "释放线路残压", 3},
        };
        return result;
    }();
    return value;
}

// 返回 EM100H 一体式高压绝缘检测仪内置设备类型。
const DeviceTemplateDefinition& em100h_insulation_monitor_device_template()
{
    static const DeviceTemplateDefinition value = [] {
        DeviceTemplateDefinition result;
        result.template_id = "EM100H";
        result.display_name = "一体式高压绝缘检测仪";
        result.description =
            "EM100H 内置设备类型。通过两个独立 FC03 区块读取实时数据和运行状态，"
            "通过固定 FC10 命令执行记录清理及绝缘测试控制。";
        result.default_start_register = 0x1000;
        result.builtin = true;
        result.device_address_stride = 35;
        result.read_blocks = {
            {"realtime_data", "实时数据区", 3, 0, 10, 0},
            {"runtime_status", "运行状态数据区", 3, 10, 25, 1},
        };
        result.realtime_grouping_enabled = true;
        result.realtime_groups = {{"core_measurement", "核心测量", 0}};
        result.fields = {
            em100h_uint16_field("insulation_resistance", "绝缘电阻", "realtime_data", 0, 1.0, "MΩ", 0, true, true, true, "core_measurement", 1),
            em100h_uint16_field("residual_current", "剩余电流", "realtime_data", 1, 0.01, "mA", 2, true, true, true, "core_measurement", 2),
            em100h_uint16_field("absorption_ratio", "吸收比", "realtime_data", 2, 0.01, "", 2, false, true, true, "core_measurement", 3),
            em100h_uint16_field("polarization_index", "极化指数", "realtime_data", 3, 1.0, "", 0, false, true, true, "core_measurement", 4),
            em100h_uint16_field("current_status", "当前状态", "realtime_data", 4, 1.0, "", 0, false, true, true, "core_measurement", 5),
        };
        result.fields[4].enum_items = {
            {0, "检测线路残余电压", 0},
            {1, "绝缘电阻测试中", 1},
            {2, "释放线路残余电压", 2},
            {3, "正常待机", 3},
            {4, "电机运行中", 4},
            {5, "高压设定/校准中", 5},
            {6, "保留", 6},
            {7, "保留", 7},
        };
        result.write_commands = em100h_write_commands();
        return result;
    }();
    return value;
}

}  // namespace edge_controller
