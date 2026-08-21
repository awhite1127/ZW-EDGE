// 设备模板通用验证与运行时注册表。
#include "model/device_template.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "model/builtin_device_templates.h"

namespace edge_controller {

namespace {

constexpr std::uint64_t kModbusAddressSpaceSize = 65536;
constexpr std::uint32_t kMaxWriteMultipleRegisterCount = 123;

// 裁剪字符串副本，仅用于模型校验，不改变稳定标识。
std::string trim_copy(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 汇总读取模型错误，保持一次请求可返回全部明确问题。
void append_read_model_error(std::string* errors, const std::string& message)
{
    if (errors == nullptr || message.empty()) {
        return;
    }
    if (!errors->empty()) {
        *errors += "；";
    }
    *errors += message;
}

// 格式化设备类型规则数值。
std::string format_rule_number(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(15) << value;
    return stream.str();
}

struct RegisterRange {
    std::uint64_t start{0};
    std::uint64_t end{0};
    std::string parser_id;
    int bit_index{-1};
};

// 判断两个字节字段能否共享同一寄存器。
bool can_share_byte_register(
    const RegisterRange& existing,
    std::uint64_t start,
    std::uint64_t end,
    const std::string& parser_id)
{
    if (existing.start != start || existing.end != end || end != start + 1) {
        return false;
    }
    return (existing.parser_id == "scaled_high_uint8" &&
            parser_id == "scaled_low_uint8") ||
           (existing.parser_id == "scaled_low_uint8" &&
            parser_id == "scaled_high_uint8");
}

// 判断两个位字段能否共享同一寄存器。
bool can_share_bit_register(
    const RegisterRange& existing,
    std::uint64_t start,
    std::uint64_t end,
    const DeviceTemplateFieldDefinition& field)
{
    return existing.start == start && existing.end == end && end == start + 1 &&
           existing.parser_id == "bit_uint16" && field.parser_id == "bit_uint16" &&
           existing.bit_index != field.bit_index;
}

// 判断枚举值是否位于字段类型允许范围内。
bool enum_value_in_field_range(
    const DeviceTemplateFieldDefinition& field,
    std::int64_t value)
{
    if (field.data_type == "bool") return value >= 0 && value <= 1;
    if (field.parser_id == "scaled_high_uint8" || field.parser_id == "scaled_low_uint8") {
        return value >= 0 && value <= 255;
    }
    if (field.data_type == "uint16") return value >= 0 && value <= 65535;
    if (field.data_type == "int16") return value >= -32768 && value <= 32767;
    if (field.data_type == "uint32") {
        return value >= 0 && static_cast<std::uint64_t>(value) <= 4294967295ULL;
    }
    if (field.data_type == "int32") {
        return value >= std::numeric_limits<std::int32_t>::min() &&
               value <= std::numeric_limits<std::int32_t>::max();
    }
    return false;
}

// 获取模板互斥锁。
std::mutex& template_mutex()
{
    static std::mutex mutex;
    return mutex;
}

// 返回进程内共享的设备类型注册表。
std::shared_ptr<const std::vector<DeviceTemplateDefinition>>& template_registry()
{
    static auto templates = std::make_shared<const std::vector<DeviceTemplateDefinition>>(
        builtin_device_templates());
    return templates;
}

// 返回模板注册表世代计数器。
std::atomic<std::uint64_t>& template_registry_generation_counter()
{
    static std::atomic<std::uint64_t> generation{1};
    return generation;
}

}  // namespace

// 校验设备类型字段无效规则规则。
StatusCode validate_device_template_field_invalid_rule(
    const DeviceTemplateFieldDefinition& field,
    std::string* error_message)
{
    const auto& type = field.invalid_rule_type;
    if (type != "none" && type != "equal" && type != "greater_or_equal" &&
        type != "less_or_equal" && type != "inside_range") {
        if (error_message != nullptr) {
            *error_message =
                "无效规则类型仅支持 none、equal、greater_or_equal、less_or_equal 或 inside_range";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!std::isfinite(field.invalid_rule_value)) {
        if (error_message != nullptr) {
            *error_message = "无效规则比较值 invalid_rule_value 必须为有限数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!std::isfinite(field.invalid_rule_min) || !std::isfinite(field.invalid_rule_max)) {
        if (error_message != nullptr) {
            *error_message = "无效规则区间 invalid_rule_min/invalid_rule_max 必须为有限数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (field.data_type == "float32") {
        const auto normalized_value = static_cast<double>(static_cast<float>(field.invalid_rule_value));
        const auto normalized_min = static_cast<double>(static_cast<float>(field.invalid_rule_min));
        const auto normalized_max = static_cast<double>(static_cast<float>(field.invalid_rule_max));
        if (!std::isfinite(normalized_value) || !std::isfinite(normalized_min) ||
            !std::isfinite(normalized_max)) {
            if (error_message != nullptr) {
                *error_message = "float32 无效规则参数规范化后必须为有限数";
            }
            return StatusCode::kInvalidArgument;
        }
        if (type == "inside_range" && normalized_min > normalized_max) {
            if (error_message != nullptr) {
                *error_message = "inside_range 无效规则的 float32 规范化最小值不能大于最大值";
            }
            return StatusCode::kInvalidArgument;
        }
    }
    if (type == "inside_range" && field.invalid_rule_min > field.invalid_rule_max) {
        if (error_message != nullptr) {
            *error_message = "inside_range 无效规则要求 invalid_rule_min 不大于 invalid_rule_max";
        }
        return StatusCode::kInvalidArgument;
    }
    if (type == "none" &&
        (field.invalid_rule_value != 0.0 || field.invalid_rule_min != 0.0 ||
         field.invalid_rule_max != 0.0)) {
        if (error_message != nullptr) {
            *error_message = "none 无效规则要求 value/min/max 全部为 0";
        }
        return StatusCode::kInvalidArgument;
    }
    if ((type == "equal" || type == "greater_or_equal" || type == "less_or_equal") &&
        (field.invalid_rule_min != 0.0 || field.invalid_rule_max != 0.0)) {
        if (error_message != nullptr) {
            *error_message = "单值无效规则要求 invalid_rule_min/invalid_rule_max 为 0";
        }
        return StatusCode::kInvalidArgument;
    }
    if (type == "inside_range" && field.invalid_rule_value != 0.0) {
        if (error_message != nullptr) {
            *error_message = "inside_range 无效规则要求 invalid_rule_value 为 0";
        }
        return StatusCode::kInvalidArgument;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return StatusCode::kOk;
}

// 判断字段原始值是否命中配置的无效值规则。
bool device_template_field_raw_value_invalid(
    const DeviceTemplateFieldDefinition& field,
    double raw_value,
    std::string* reason)
{
    if (reason != nullptr) {
        reason->clear();
    }
    if (!std::isfinite(raw_value)) {
        if (reason != nullptr) {
            *reason = "解码后的原始值不是有限数（NaN/Inf）";
        }
        return true;
    }

    const auto normalize_rule_value = [&](double value) {
        return field.data_type == "float32"
                   ? static_cast<double>(static_cast<float>(value))
                   : value;
    };
    const auto rule_value = normalize_rule_value(field.invalid_rule_value);
    const auto rule_min = normalize_rule_value(field.invalid_rule_min);
    const auto rule_max = normalize_rule_value(field.invalid_rule_max);

    bool invalid = false;
    std::string condition;
    if (field.invalid_rule_type == "equal") {
        invalid = raw_value == rule_value;
        condition = "等于 " + format_rule_number(rule_value);
    } else if (field.invalid_rule_type == "greater_or_equal") {
        invalid = raw_value >= rule_value;
        condition = "大于或等于 " + format_rule_number(rule_value);
    } else if (field.invalid_rule_type == "less_or_equal") {
        invalid = raw_value <= rule_value;
        condition = "小于或等于 " + format_rule_number(rule_value);
    } else if (field.invalid_rule_type == "inside_range") {
        invalid = raw_value >= rule_min && raw_value <= rule_max;
        condition = "位于闭区间 [" + format_rule_number(rule_min) + ", " +
                    format_rule_number(rule_max) + "]";
    }

    if (invalid && reason != nullptr) {
        *reason = "原始值 " + format_rule_number(raw_value) +
                  " 命中字段无效规则：" + condition;
    }
    return invalid;
}

// 校验设备类型字段位枚举。
StatusCode validate_device_template_field_bit_and_enum(
    const DeviceTemplateFieldDefinition& field,
    std::string* error_message)
{
    std::string errors;
    const bool is_bit = field.data_type == "bool" || field.parser_id == "bit_uint16";
    if (is_bit) {
        if (field.data_type != "bool" || field.parser_id != "bit_uint16") {
            append_read_model_error(&errors, "bool 字段必须使用 parser_id=bit_uint16");
        }
        if (field.register_count != 1) {
            append_read_model_error(&errors, "bool 字段 register_count 必须为 1");
        }
        if (field.bit_index < 0 || field.bit_index > 15) {
            append_read_model_error(&errors, "bool 字段 bit_index 必须为 0～15");
        }
        if (field.scale != 1.0 || field.value_offset != 0.0 || field.precision != 0) {
            append_read_model_error(&errors, "bool 字段要求 scale=1、offset=0、precision=0");
        }
    } else if (field.bit_index != -1) {
        append_read_model_error(&errors, "非 bool 字段 bit_index 必须为 -1");
    }

    if (field.enum_items.size() > 32) {
        append_read_model_error(&errors, "单字段枚举项不能超过 32 条");
    }
    if (!field.enum_items.empty()) {
        if (field.data_type == "float32") {
            append_read_model_error(&errors, "float32 不支持枚举显示");
        }
        if (!enum_value_in_field_range(field, 0)) {
            append_read_model_error(&errors, "当前字段类型不支持枚举显示");
        }
        if (field.scale != 1.0 || field.value_offset != 0.0 || field.precision != 0) {
            append_read_model_error(&errors, "枚举字段要求 scale=1、offset=0、precision=0");
        }
        std::unordered_set<std::int64_t> values;
        for (std::size_t index = 0; index < field.enum_items.size(); ++index) {
            const auto& item = field.enum_items[index];
            const auto label = "枚举项[" + std::to_string(index + 1) + "]";
            if (!values.insert(item.value).second) {
                append_read_model_error(&errors, label + " 数值重复");
            }
            if (trim_copy(item.label).empty()) {
                append_read_model_error(&errors, label + " 显示文字不能为空");
            }
            if (!enum_value_in_field_range(field, item.value)) {
                append_read_model_error(&errors, label + " 数值超出字段数据类型范围");
            }
        }
    }

    if (!errors.empty()) {
        if (error_message != nullptr) *error_message = std::move(errors);
        return StatusCode::kInvalidArgument;
    }
    if (error_message != nullptr) error_message->clear();
    return StatusCode::kOk;
}

// 返回内置单区块模型使用的稳定读取区块标识。
const std::string& default_device_template_read_block_key()
{
    static const std::string value = "default";
    return value;
}

// 返回内置单区块模型的读取区块显示名称。
const std::string& default_device_template_read_block_display_name()
{
    static const std::string value = "默认读取区块";
    return value;
}

// 校验读取区块、地址跨度以及字段相对区块布局。
StatusCode validate_device_template_read_model(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    // 校验设备级地址跨度和读取区块是否存在。
    std::string errors;
    if (device_template.device_address_stride == 0) {
        append_read_model_error(&errors, "设备地址跨度必须大于 0");
    } else if (device_template.device_address_stride > kModbusAddressSpaceSize) {
        append_read_model_error(&errors, "设备地址跨度不能超过 Modbus 地址空间 65536");
    }
    if (device_template.read_blocks.empty()) {
        append_read_model_error(&errors, "设备类型至少需要一个读取区块");
    }

    struct ValidatedBlockRange {
        std::string block_key;
        std::uint32_t function_code{0};
        std::uint64_t start{0};
        std::uint64_t end{0};
    };
    // 校验各读取区块并建立字段引用索引。
    std::unordered_set<std::string> block_keys;
    std::unordered_map<std::string, const DeviceTemplateReadBlockDefinition*> blocks_by_key;
    std::vector<ValidatedBlockRange> block_ranges;
    block_ranges.reserve(device_template.read_blocks.size());

    for (std::size_t index = 0; index < device_template.read_blocks.size(); ++index) {
        const auto& block = device_template.read_blocks[index];
        const auto label = "读取区块[" + std::to_string(index + 1) + "]";
        const bool key_empty = trim_copy(block.block_key).empty();
        if (key_empty) {
            append_read_model_error(&errors, label + " 标识不能为空");
        } else if (!block_keys.insert(block.block_key).second) {
            append_read_model_error(&errors, "读取区块标识重复：" + block.block_key);
        } else {
            blocks_by_key.emplace(block.block_key, &block);
        }
        if (trim_copy(block.display_name).empty()) {
            append_read_model_error(&errors, label + " 显示名称不能为空");
        }
        const bool function_code_supported =
            device_template_read_function_code_supported(block.function_code);
        if (!function_code_supported) {
            append_read_model_error(
                &errors,
                label + " 功能码仅支持 Modbus FC03 或 FC04");
        }
        const bool register_count_valid =
            block.register_count >= 1 && block.register_count <= 125;
        if (!register_count_valid) {
            append_read_model_error(&errors, label + " 寄存器数量必须为 1～125");
        }

        const auto start = static_cast<std::uint64_t>(block.start_offset);
        const auto count = static_cast<std::uint64_t>(block.register_count);
        const auto end = start + count;
        const bool address_range_valid =
            start < kModbusAddressSpaceSize && end <= kModbusAddressSpaceSize;
        if (!address_range_valid) {
            append_read_model_error(&errors, label + " 超出 Modbus 地址空间");
        }
        if (device_template.device_address_stride > 0 &&
            end > static_cast<std::uint64_t>(device_template.device_address_stride)) {
            append_read_model_error(&errors, label + " 超出设备地址跨度");
        }
        if (!key_empty && function_code_supported && register_count_valid &&
            address_range_valid) {
            block_ranges.push_back({block.block_key, block.function_code, start, end});
        }
    }

    // 同一功能码的读取区块不得发生地址重叠。
    for (std::size_t left = 0; left < block_ranges.size(); ++left) {
        for (std::size_t right = left + 1; right < block_ranges.size(); ++right) {
            const auto& first = block_ranges[left];
            const auto& second = block_ranges[right];
            if (first.function_code == second.function_code &&
                first.start < second.end && second.start < first.end) {
                append_read_model_error(
                    &errors,
                    "相同功能码读取区块地址重叠：" + first.block_key + " 与 " +
                        second.block_key);
            }
        }
    }

    // 校验字段规则、区块引用、范围和寄存器占用关系。
    std::unordered_map<std::string, std::vector<RegisterRange>> field_ranges_by_block;
    for (std::size_t index = 0; index < device_template.fields.size(); ++index) {
        const auto& field = device_template.fields[index];
        const auto label = "采集点[" + std::to_string(index + 1) + "]";
        std::string invalid_rule_error;
        if (!is_ok(validate_device_template_field_invalid_rule(field, &invalid_rule_error))) {
            append_read_model_error(&errors, label + " " + invalid_rule_error);
        }
        std::string bit_enum_error;
        if (!is_ok(validate_device_template_field_bit_and_enum(field, &bit_enum_error))) {
            append_read_model_error(&errors, label + " " + bit_enum_error);
        }
        const auto block_it = blocks_by_key.find(field.read_block_key);
        if (block_it == blocks_by_key.end()) {
            append_read_model_error(
                &errors,
                label + " 引用的读取区块不存在：" +
                    (field.read_block_key.empty() ? std::string("<空>") : field.read_block_key));
            continue;
        }

        const auto start = static_cast<std::uint64_t>(field.register_offset);
        const auto end = start + static_cast<std::uint64_t>(field.register_count);
        if (end > static_cast<std::uint64_t>(block_it->second->register_count)) {
            append_read_model_error(&errors, label + " 超出所属读取区块范围");
        }

        auto& existing_ranges = field_ranges_by_block[field.read_block_key];
        for (const auto& existing : existing_ranges) {
            if (start < existing.end && existing.start < end &&
                !can_share_byte_register(existing, start, end, field.parser_id) &&
                !can_share_bit_register(existing, start, end, field)) {
                append_read_model_error(
                    &errors,
                    label + " 寄存器范围与同一读取区块内其他字段重叠");
                break;
            }
        }
        existing_ranges.push_back({start, end, field.parser_id, field.bit_index});
    }

    // 一次性返回累计的全部读取模型错误。
    if (!errors.empty()) {
        if (error_message != nullptr) {
            *error_message = std::move(errors);
        }
        return StatusCode::kInvalidArgument;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return StatusCode::kOk;
}

// 校验设备类型受控写命令定义。
StatusCode validate_device_template_write_commands(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    std::unordered_set<std::string> command_keys;
    for (std::size_t command_index = 0;
         command_index < device_template.write_commands.size();
         ++command_index) {
        const auto& command = device_template.write_commands[command_index];
        const auto command_key = trim_copy(command.key);
        const auto command_label = "设备模板 " +
                                   (device_template.template_id.empty()
                                        ? std::string("<空>")
                                        : device_template.template_id) +
                                   " 的控制命令 " +
                                   (command_key.empty()
                                        ? "[" + std::to_string(command_index + 1) + "]"
                                        : command_key);
        const auto fail = [&](const std::string& reason) {
            if (error_message != nullptr) {
                *error_message = command_label + " 定义无效：" + reason;
            }
            return StatusCode::kInvalidArgument;
        };

        if (command_key.empty()) {
            return fail("command key 不能为空");
        }
        if (!command_keys.insert(command.key).second) {
            return fail("command key 重复");
        }
        if (trim_copy(command.name).empty()) {
            return fail("命令名称不能为空");
        }
        if (command.function_code != 16) {
            return fail("当前仅支持 Modbus FC10（function code 16）");
        }
        if (command.register_count == 0 ||
            command.register_count > kMaxWriteMultipleRegisterCount) {
            return fail("register_count 必须为 1～123");
        }

        const bool has_fixed_values = !command.fixed_values.empty();
        const bool has_value_fields = !command.value_fields.empty();
        if (has_fixed_values == has_value_fields) {
            return fail("fixed_values 与 value_fields 必须且只能定义一种");
        }
        if (has_fixed_values && command.fixed_values.size() != command.register_count) {
            return fail("fixed_values 数量必须与 register_count 一致");
        }
        if (has_value_fields && command.value_fields.size() != command.register_count) {
            return fail("value_fields 数量必须与 register_count 一致");
        }

        const auto start_register = command.has_absolute_register
                                        ? static_cast<std::uint64_t>(command.absolute_register)
                                        : static_cast<std::uint64_t>(command.register_offset);
        if (start_register + static_cast<std::uint64_t>(command.register_count) >
            kModbusAddressSpaceSize) {
            return fail(command.has_absolute_register
                            ? "absolute_register 与 register_count 超出 Modbus 地址空间"
                            : "register_offset 与 register_count 超出 Modbus 地址空间");
        }

        std::unordered_set<std::string> field_keys;
        for (std::size_t field_index = 0;
             field_index < command.value_fields.size();
             ++field_index) {
            const auto& field = command.value_fields[field_index];
            const auto field_key = trim_copy(field.key);
            const auto field_label = "参数[" + std::to_string(field_index + 1) + "]";
            if (field_key.empty()) {
                return fail(field_label + " key 不能为空");
            }
            if (!field_keys.insert(field.key).second) {
                return fail("参数 key 重复：" + field.key);
            }
            if (field.type != "uint16" && field.type != "enum") {
                return fail("参数 " + field.key + " 类型仅支持 uint16 或 enum");
            }
            if (field.min > field.max) {
                return fail("参数 " + field.key + " 的 min 不能大于 max");
            }

            if (field.type == "uint16") {
                if (field.has_default_value &&
                    (field.default_value < field.min || field.default_value > field.max)) {
                    return fail("参数 " + field.key + " 的默认值超出 min/max 范围");
                }
                continue;
            }

            if (field.options.empty()) {
                return fail("枚举参数 " + field.key + " 至少需要一个选项");
            }
            std::unordered_set<std::uint16_t> option_values;
            bool default_value_found = !field.has_default_value;
            for (const auto& option : field.options) {
                if (!option_values.insert(option.value).second) {
                    return fail("枚举参数 " + field.key + " 的选项值重复");
                }
                if (option.value < field.min || option.value > field.max) {
                    return fail("枚举参数 " + field.key + " 的选项值超出 min/max 范围");
                }
                if (field.has_default_value && option.value == field.default_value) {
                    default_value_found = true;
                }
            }
            if (!default_value_found) {
                return fail("枚举参数 " + field.key + " 的默认值不在选项中");
            }
        }
    }

    if (error_message != nullptr) {
        error_message->clear();
    }
    return StatusCode::kOk;
}
// 返回当前设备类型注册表快照；内部加锁保护共享注册表。
std::vector<DeviceTemplateDefinition> device_templates()
{
    std::lock_guard<std::mutex> lock(template_mutex());
    return *template_registry();
}

// 替换设备类型注册表；内部加锁保护共享注册表。
void set_device_templates(std::vector<DeviceTemplateDefinition> templates)
{
    if (templates.empty()) {
        templates = builtin_device_templates();
    }
    std::lock_guard<std::mutex> lock(template_mutex());
    template_registry() = std::make_shared<const std::vector<DeviceTemplateDefinition>>(
        std::move(templates));
    template_registry_generation_counter().fetch_add(1U, std::memory_order_release);
}

// 返回当前设备模板注册表世代，稳态读取不获取模板注册表互斥锁。
std::uint64_t device_template_registry_generation()
{
    return template_registry_generation_counter().load(std::memory_order_acquire);
}

// 获取模板并让返回值持有当前不可变注册表快照，避免热更新使指针失效。
std::shared_ptr<const DeviceTemplateDefinition> find_device_template(const std::string& template_id)
{
    const auto& resolved_id = template_id.empty() ? default_device_template_id() : template_id;
    std::lock_guard<std::mutex> lock(template_mutex());
    const auto snapshot = template_registry();
    for (const auto& device_template : *snapshot) {
        if (device_template.template_id == resolved_id) {
            return std::shared_ptr<const DeviceTemplateDefinition>(snapshot, &device_template);
        }
    }
    return {};
}

}  // namespace edge_controller
