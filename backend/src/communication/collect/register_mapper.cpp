// 寄存器映射器将原始 Modbus 寄存器转换为模板数据项，集中处理字节序、比例和有效性。
#include "communication/collect/register_mapper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "data/model/data_item_keys.h"
#include "data/model/device_value_status.h"
#include "data/model/device_template.h"

namespace edge_controller {

namespace {

// 构造设备错误信息。
std::string build_device_error(
    const MasterNodeConfig& master_config,
    const DeviceConfig& device_config,
    const std::string& reason)
{
    return "主控 " + master_config.master_id +
           " 的设备 " + device_config.device_id +
           " 映射失败: " + reason;
}

// 构造采集失败时的设备状态。
DeviceStatus build_failed_status(
    const DeviceConfig& device_config,
    TimestampMs update_time_ms)
{
    DeviceStatus status;
    status.device_id = device_config.device_id;
    status.device_name = device_config.device_name;
    status.master_id = device_config.master_id;
    status.online = false;
    status.last_collect_success = false;
    status.communication_quality = DataQuality::kBad;
    status.updated_at_ms = update_time_ms;
    status.last_failure_time_ms = update_time_ms;
    status.diagnosis = make_diagnosis(
        DiagnosisLevel::kDevice,
        device_config.device_id,
        device_config.device_name,
        DiagnosisRunStatus::kError,
        DiagnosisErrorCode::kDeviceParseFailed,
        status.last_success_time_ms,
        update_time_ms,
        status.diagnosis.consecutive_failures + 1);
    status.last_error_message = status.diagnosis.message;
    return status;
}

// 按字段显示顺序和稳定键进行排序比较。
bool field_order_less(
    const DeviceTemplateFieldDefinition* left,
    const DeviceTemplateFieldDefinition* right)
{
    if (left == nullptr || right == nullptr) {
        return left != nullptr;
    }
    if (left->display_order != right->display_order) {
        return left->display_order < right->display_order;
    }
    return left->field_key < right->field_key;
}

// 与模板校验层保持相同的数值文本格式，仅在计划构建或命中无效规则时调用。
std::string format_rule_number(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(15) << value;
    return stream.str();
}

// 构造点位值。
PointValue build_point_value(
    const DeviceTemplateFieldDefinition& field,
    double value,
    double raw_value,
    DataQuality quality,
    bool valid,
    const std::string& message,
    TimestampMs update_time_ms,
    const std::string& display_text = {})
{
    PointValue point;
    point.key = field.field_key;
    point.name = field.display_name;
    point.value = value;
    point.unit = field.unit;
    point.precision = field.precision;
    point.summary = field.summary;
    point.history_enabled = device_template_field_history_enabled(field);
    point.display_order = field.display_order;
    point.quality = quality;
    point.valid = valid;
    point.raw_value = raw_value;
    point.display_text = display_text;
    if (valid) {
        const auto integer_raw = static_cast<std::int64_t>(raw_value);
        const auto item = std::find_if(field.enum_items.begin(), field.enum_items.end(), [&](const auto& candidate) {
            return candidate.value == integer_raw;
        });
        if (item != field.enum_items.end()) {
            point.display_text = item->label;
        } else if (field.data_type == "bool") {
            point.display_text = integer_raw == 0 ? "否" : "是";
        }
    }
    point.message = message;
    point.sample_time_ms = update_time_ms;
    return point;
}

struct FieldDecodeResult {
    double raw_value{0.0};
    double value{0.0};
    bool valid{false};
    // 定义错误会让本次映射结果失败；设备返回的无效数据只降级字段和设备质量。
    bool definition_valid{true};
    DeviceValueStatusCode value_status_code{DeviceValueStatusCode::kNone};
    DiagnosisErrorCode value_status_error_code{DiagnosisErrorCode::kNone};
    DiagnosisRunStatus value_status_run_status{DiagnosisRunStatus::kNormal};
    std::string display_text;
    std::string message;
};

// 构造设备类型定义错误。
FieldDecodeResult make_definition_error(std::string message)
{
    FieldDecodeResult result;
    result.definition_valid = false;
    result.message = std::move(message);
    return result;
}

// 判断数据类型。
bool is_supported_data_type(const std::string& data_type)
{
    return data_type == "uint16" ||
           data_type == "int16" ||
           data_type == "uint32" ||
           data_type == "int32" ||
           data_type == "float32" ||
           data_type == "bool";
}

// 判断字段解析器标识是否受支持。
bool is_supported_parser_id(const std::string& parser_id)
{
    return parser_id == "scaled_uint16" ||
           parser_id == "scaled_int16" ||
           parser_id == "scaled_high_uint8" ||
           parser_id == "scaled_low_uint8" ||
           parser_id == "scaled_uint32" ||
           parser_id == "scaled_int32" ||
           parser_id == "scaled_float32" ||
           parser_id == "bit_uint16";
}

// 同时校验 data_type 与 parser_id，避免合法名称被错误组合后悄悄按另一种类型解释。
bool resolve_decoder_kind(
    const DeviceTemplateFieldDefinition& field,
    RegisterFieldDecoderKind* decoder_kind,
    std::string* message)
{
    if (!is_supported_data_type(field.data_type)) {
        *message = "未知数据类型: " + field.data_type;
        return false;
    }
    if (!is_supported_parser_id(field.parser_id)) {
        *message = "未知解析器: " + field.parser_id;
        return false;
    }

    if (field.data_type == "uint16" && field.parser_id == "scaled_uint16") {
        *decoder_kind = RegisterFieldDecoderKind::kUint16;
        return true;
    }
    if (field.data_type == "int16" && field.parser_id == "scaled_int16") {
        *decoder_kind = RegisterFieldDecoderKind::kInt16;
        return true;
    }
    if (field.data_type == "uint16" && field.parser_id == "scaled_high_uint8") {
        *decoder_kind = RegisterFieldDecoderKind::kHighUint8;
        return true;
    }
    if (field.data_type == "uint16" && field.parser_id == "scaled_low_uint8") {
        *decoder_kind = RegisterFieldDecoderKind::kLowUint8;
        return true;
    }
    if (field.data_type == "uint32" && field.parser_id == "scaled_uint32") {
        *decoder_kind = RegisterFieldDecoderKind::kUint32;
        return true;
    }
    if (field.data_type == "int32" && field.parser_id == "scaled_int32") {
        *decoder_kind = RegisterFieldDecoderKind::kInt32;
        return true;
    }
    if (field.data_type == "float32" && field.parser_id == "scaled_float32") {
        *decoder_kind = RegisterFieldDecoderKind::kFloat32;
        return true;
    }
    if (field.data_type == "bool" && field.parser_id == "bit_uint16") {
        *decoder_kind = RegisterFieldDecoderKind::kBitUint16;
        return true;
    }

    *message = "数据类型与解析器不匹配: " + field.data_type + " / " + field.parser_id;
    return false;
}

// 返回字段类型预期占用的寄存器数量。
RegisterCount expected_register_count(RegisterFieldDecoderKind decoder_kind)
{
    switch (decoder_kind) {
    case RegisterFieldDecoderKind::kUint32:
    case RegisterFieldDecoderKind::kInt32:
    case RegisterFieldDecoderKind::kFloat32:
        return 2;
    case RegisterFieldDecoderKind::kUint16:
    case RegisterFieldDecoderKind::kInt16:
    case RegisterFieldDecoderKind::kHighUint8:
    case RegisterFieldDecoderKind::kLowUint8:
    case RegisterFieldDecoderKind::kBitUint16:
        return 1;
    }
    return 0;
}

// 把模板字段的静态解析规则编译为配置世代内可复用的运行时元数据。
RegisterMapperFieldRuntime compile_field_runtime(
    const DeviceTemplateFieldDefinition* field,
    bool builtin_template)
{
    RegisterMapperFieldRuntime runtime;
    runtime.definition = field;
    if (field == nullptr) {
        runtime.definition_error = "字段运行时定义为空";
        return runtime;
    }

    std::string definition_error;
    if (!resolve_decoder_kind(*field, &runtime.decoder_kind, &definition_error)) {
        runtime.definition_error = std::move(definition_error);
        return runtime;
    }

    runtime.required_register_count = expected_register_count(runtime.decoder_kind);
    if (field->register_count != runtime.required_register_count) {
        runtime.definition_error =
            "register_count 不匹配，期望 " +
            std::to_string(runtime.required_register_count) +
            "，实际 " + std::to_string(field->register_count);
        return runtime;
    }
    if (field->byte_order != "big_endian" &&
        field->byte_order != "little_endian") {
        runtime.definition_error = "非法 byte_order: " + field->byte_order;
        return runtime;
    }
    if (field->word_order != "high_word_first" &&
        field->word_order != "low_word_first") {
        runtime.definition_error = "非法 word_order: " + field->word_order;
        return runtime;
    }
    if (runtime.required_register_count == 1 &&
        (field->byte_order != "big_endian" ||
         field->word_order != "high_word_first")) {
        runtime.definition_error = "16 位字段仅支持默认数据排列";
        return runtime;
    }
    if (!std::isfinite(field->scale) || !std::isfinite(field->value_offset)) {
        runtime.definition_error = "比例系数或数值偏移不是有限数";
        return runtime;
    }
    std::string invalid_rule_error;
    if (!is_ok(validate_device_template_field_invalid_rule(
            *field, &invalid_rule_error))) {
        runtime.definition_error =
            "字段无效规则定义错误：" + invalid_rule_error;
        return runtime;
    }

    const auto normalize_rule_value = [&](double value) {
        return runtime.decoder_kind == RegisterFieldDecoderKind::kFloat32
                   ? static_cast<double>(static_cast<float>(value))
                   : value;
    };
    runtime.invalid_rule_value =
        normalize_rule_value(field->invalid_rule_value);
    runtime.invalid_rule_min =
        normalize_rule_value(field->invalid_rule_min);
    runtime.invalid_rule_max =
        normalize_rule_value(field->invalid_rule_max);
    if (field->invalid_rule_type == "equal") {
        runtime.invalid_rule_kind = RegisterInvalidRuleKind::kEqual;
        runtime.invalid_rule_condition =
            "等于 " + format_rule_number(runtime.invalid_rule_value);
    } else if (field->invalid_rule_type == "greater_or_equal") {
        runtime.invalid_rule_kind =
            RegisterInvalidRuleKind::kGreaterOrEqual;
        runtime.invalid_rule_condition =
            "大于或等于 " +
            format_rule_number(runtime.invalid_rule_value);
    } else if (field->invalid_rule_type == "less_or_equal") {
        runtime.invalid_rule_kind =
            RegisterInvalidRuleKind::kLessOrEqual;
        runtime.invalid_rule_condition =
            "小于或等于 " +
            format_rule_number(runtime.invalid_rule_value);
    } else if (field->invalid_rule_type == "inside_range") {
        runtime.invalid_rule_kind =
            RegisterInvalidRuleKind::kInsideRange;
        runtime.invalid_rule_condition =
            "位于闭区间 [" +
            format_rule_number(runtime.invalid_rule_min) + ", " +
            format_rule_number(runtime.invalid_rule_max) + "]";
    }

    std::string bit_enum_error;
    if (!is_ok(validate_device_template_field_bit_and_enum(
            *field, &bit_enum_error))) {
        runtime.definition_error =
            "单比特或枚举定义错误：" + bit_enum_error;
        return runtime;
    }

    runtime.swap_bytes = field->byte_order == "little_endian";
    runtime.low_word_first = field->word_order == "low_word_first";
    if (builtin_template && runtime.decoder_kind == RegisterFieldDecoderKind::kUint16) {
        runtime.value_status_scope = DeviceValueStatusScope::kBuiltinUint16;
    }
    runtime.definition_valid = true;
    return runtime;
}

// 交换 16 位寄存器的高低字节。
std::uint16_t swap_bytes(std::uint16_t value)
{
    return static_cast<std::uint16_t>((value << 8U) | (value >> 8U));
}

// 按字序组合两个寄存器为 32 位值。
std::uint32_t combine_32_bits(
    std::uint16_t first_register,
    std::uint16_t second_register,
    bool swap_register_bytes,
    bool low_word_first)
{
    if (swap_register_bytes) {
        first_register = swap_bytes(first_register);
        second_register = swap_bytes(second_register);
    }
    const auto high_word = low_word_first ? second_register : first_register;
    const auto low_word = low_word_first ? first_register : second_register;
    return (static_cast<std::uint32_t>(high_word) << 16U) |
           static_cast<std::uint32_t>(low_word);
}

// 构造已解析的数值字段结果。
FieldDecodeResult build_numeric_result(
    const RegisterMapperFieldRuntime& field_runtime,
    double raw_value)
{
    FieldDecodeResult result;
    if (field_runtime.definition == nullptr) {
        return make_definition_error("字段运行时定义为空");
    }
    const auto& field = *field_runtime.definition;

    // 内置设备的协议哨兵值由数值状态模块直接判定，不再依赖模板先把它标为无效。
    // 规则在 scale/value_offset 之前匹配原始值，命中即给出统一的无效原因和诊断语义。
    if (std::isfinite(raw_value)) {
        const auto value_status = classify_device_value_status(
            field_runtime.value_status_scope,
            raw_value);
        if (value_status.matched()) {
            const auto& definition = diagnosis_definition(value_status.error_code);
            result.raw_value = raw_value;
            result.value = 0.0;
            result.value_status_code = value_status.status_code;
            result.value_status_error_code = value_status.error_code;
            result.value_status_run_status = value_status.run_status;
            result.display_text = definition.message;
            result.message = std::string(definition.message) +
                             "（原始值 " + format_rule_number(raw_value) + "）";
            return result;
        }
    }

    bool raw_value_invalid = !std::isfinite(raw_value);
    if (!raw_value_invalid) {
        switch (field_runtime.invalid_rule_kind) {
        case RegisterInvalidRuleKind::kNone:
            break;
        case RegisterInvalidRuleKind::kEqual:
            raw_value_invalid =
                raw_value == field_runtime.invalid_rule_value;
            break;
        case RegisterInvalidRuleKind::kGreaterOrEqual:
            raw_value_invalid =
                raw_value >= field_runtime.invalid_rule_value;
            break;
        case RegisterInvalidRuleKind::kLessOrEqual:
            raw_value_invalid =
                raw_value <= field_runtime.invalid_rule_value;
            break;
        case RegisterInvalidRuleKind::kInsideRange:
            raw_value_invalid =
                raw_value >= field_runtime.invalid_rule_min &&
                raw_value <= field_runtime.invalid_rule_max;
            break;
        }
    }
    if (raw_value_invalid) {
        // raw/value 都使用有限占位，禁止 NaN/Inf 进入 JSON、历史、告警、MQTT 和北向映射。
        result.raw_value = std::isfinite(raw_value) ? raw_value : 0.0;
        result.value = 0.0;
        if (std::isfinite(raw_value)) {
            result.message =
                "原始值 " + format_rule_number(raw_value) +
                " 命中字段无效规则：" +
                field_runtime.invalid_rule_condition;
        } else {
            result.message = "解码后的原始值不是有限数（NaN/Inf）";
        }
        return result;
    }

    result.raw_value = raw_value;
    result.value = raw_value * field.scale + field.value_offset;
    if (!std::isfinite(result.value)) {
        // PointValue 可能进入 JSON、历史、告警、MQTT 和北向映射，禁止非有限数离开映射层。
        result.value = 0.0;
        result.message = "工程值无效";
        return result;
    }

    result.valid = true;
    return result;
}

// 从完整寄存器块安全解码字段。所有 parser/type/count/order 校验均先于任何索引访问。
FieldDecodeResult decode_field_value(
    const RegisterMapperFieldRuntime& field_runtime,
    const std::vector<std::uint16_t>& registers,
    std::size_t register_offset,
    std::size_t device_registers_available)
{
    if (field_runtime.definition == nullptr) {
        return make_definition_error("字段运行时定义为空");
    }
    if (!field_runtime.definition_valid) {
        return make_definition_error(field_runtime.definition_error);
    }

    const auto& field = *field_runtime.definition;
    const auto decoder_kind = field_runtime.decoder_kind;
    const auto count = static_cast<std::size_t>(field_runtime.required_register_count);
    if (count > device_registers_available) {
        return make_definition_error("字段寄存器越界");
    }
    if (register_offset > registers.size() || count > registers.size() - register_offset) {
        return make_definition_error("字段寄存器越界");
    }

    const auto first_register = registers[register_offset];
    switch (decoder_kind) {
    case RegisterFieldDecoderKind::kUint16:
        return build_numeric_result(field_runtime, static_cast<double>(first_register));
    case RegisterFieldDecoderKind::kInt16: {
        std::int16_t signed_value = 0;
        static_assert(sizeof(signed_value) == sizeof(first_register), "int16 decoding requires 16-bit integers");
        std::memcpy(&signed_value, &first_register, sizeof(signed_value));
        return build_numeric_result(field_runtime, static_cast<double>(signed_value));
    }
    case RegisterFieldDecoderKind::kHighUint8: {
        const auto byte_value = static_cast<std::uint8_t>((first_register >> 8U) & 0xFFU);
        return build_numeric_result(field_runtime, static_cast<double>(byte_value));
    }
    case RegisterFieldDecoderKind::kLowUint8: {
        const auto byte_value = static_cast<std::uint8_t>(first_register & 0xFFU);
        return build_numeric_result(field_runtime, static_cast<double>(byte_value));
    }
    case RegisterFieldDecoderKind::kBitUint16: {
        const auto bit_value = static_cast<std::uint16_t>(
            (first_register >> static_cast<unsigned int>(field.bit_index)) & 0x01U);
        return build_numeric_result(field_runtime, static_cast<double>(bit_value));
    }
    case RegisterFieldDecoderKind::kUint32:
    case RegisterFieldDecoderKind::kInt32:
    case RegisterFieldDecoderKind::kFloat32:
        break;
    }

    const auto raw_bits = combine_32_bits(
        first_register,
        registers[register_offset + 1],
        field_runtime.swap_bytes,
        field_runtime.low_word_first);
    if (decoder_kind == RegisterFieldDecoderKind::kUint32) {
        const auto raw_value = static_cast<double>(raw_bits);
        return build_numeric_result(field_runtime, raw_value);
    }
    if (decoder_kind == RegisterFieldDecoderKind::kInt32) {
        std::int32_t signed_value = 0;
        static_assert(sizeof(signed_value) == sizeof(raw_bits), "int32 decoding requires 32-bit integers");
        std::memcpy(&signed_value, &raw_bits, sizeof(signed_value));
        const auto raw_value = static_cast<double>(signed_value);
        return build_numeric_result(field_runtime, raw_value);
    }

    float float_value = 0.0F;
    static_assert(
        sizeof(float_value) == sizeof(raw_bits) && std::numeric_limits<float>::is_iec559,
        "float32 decoding requires 32-bit IEEE-754 float");
    std::memcpy(&float_value, &raw_bits, sizeof(float_value));
    const auto raw_value = static_cast<double>(float_value);
    return build_numeric_result(field_runtime, raw_value);
}

// 构造带稳定 key 和显示名称的区块上下文，确保局部失败可定位。
std::string read_block_context(const DeviceTemplateReadBlockDefinition& block)
{
    if (block.display_name.empty()) {
        return "读取区块 " + block.block_key;
    }
    return "读取区块 " + block.block_key + "（" + block.display_name + "）";
}

// 返回读取区块显示标签。
std::string read_block_label(const DeviceTemplateReadBlockDefinition& block)
{
    return block.display_name.empty() ? block.block_key : block.display_name;
}

// 设备级摘要只保留第一个详情或最多三个区块名，避免随区块数无界增长。
std::string compact_block_failure_summary(
    const std::vector<std::string>& failed_block_labels,
    const std::string& first_failure_detail,
    bool any_block_succeeded)
{
    if (failed_block_labels.empty()) return {};
    if (failed_block_labels.size() == 1 && !first_failure_detail.empty()) {
        return first_failure_detail;
    }

    std::string summary = any_block_succeeded
                              ? std::to_string(failed_block_labels.size()) + " 个读取区块失败："
                              : "所有读取区块失败（" +
                                    std::to_string(failed_block_labels.size()) + " 个）：";
    const auto shown = std::min<std::size_t>(failed_block_labels.size(), 3);
    for (std::size_t index = 0; index < shown; ++index) {
        if (index > 0) summary += "、";
        summary += failed_block_labels[index];
    }
    if (failed_block_labels.size() > shown) summary += "等";
    return summary;
}

// 构造初始设备成功状态；任一区块或字段失败后再独立降级。
DeviceStatus build_initial_status(
    const DeviceConfig& device_config,
    const DeviceTemplateDefinition& device_template,
    TimestampMs update_time_ms)
{
    DeviceStatus status;
    status.device_id = device_config.device_id;
    status.device_name = device_config.device_name;
    status.master_id = device_config.master_id;
    status.template_id = device_template.template_id;
    status.template_name = device_template.display_name;
    status.online = true;
    status.last_collect_success = true;
    status.communication_quality = DataQuality::kGood;
    status.updated_at_ms = update_time_ms;
    status.last_success_time_ms = update_time_ms;
    status.diagnosis = make_normal_diagnosis(
        DiagnosisLevel::kDevice,
        device_config.device_id,
        device_config.device_name,
        update_time_ms);
    status.last_error_message.clear();
    return status;
}

// 选择区块失败使用的诊断错误码。
DiagnosisErrorCode effective_block_error_code(const DeviceReadBlockResult* block_result)
{
    if (block_result != nullptr &&
        block_result->diagnosis_error_code != DiagnosisErrorCode::kNone) {
        return block_result->diagnosis_error_code;
    }
    return DiagnosisErrorCode::kDeviceParseFailed;
}

}  // namespace

// 预编译模板字段顺序、解析器和静态校验结果。
RegisterMapperRuntimePlan RegisterMapper::build_runtime_plan(
    std::shared_ptr<const DeviceTemplateDefinition> device_template)
{
    RegisterMapperRuntimePlan plan;
    plan.device_template = std::move(device_template);
    if (plan.device_template == nullptr) {
        return plan;
    }

    std::vector<const DeviceTemplateFieldDefinition*> ordered_fields;
    ordered_fields.reserve(plan.device_template->fields.size());
    for (const auto& field : plan.device_template->fields) {
        ordered_fields.push_back(&field);
    }
    std::sort(ordered_fields.begin(), ordered_fields.end(), field_order_less);

    plan.ordered_fields.reserve(ordered_fields.size());
    for (const auto* field : ordered_fields) {
        plan.ordered_fields.push_back(compile_field_runtime(
            field,
            plan.device_template->builtin));
    }
    return plan;
}

// 兼容直接调用者：临时构建一次运行计划；轮询服务使用下方重载跨周期复用。
RegisterMapperResult RegisterMapper::map_devices(
    const MasterNodeConfig& master_config,
    const std::vector<const DeviceConfig*>& devices,
    const std::vector<DeviceReadResult>& device_read_results,
    TimestampMs update_time_ms)
{
    return map_devices(
        master_config,
        devices,
        build_runtime_plan(find_device_template(master_config.device_template)),
        device_read_results,
        update_time_ms);
}

// 将独立区块结果按 device_id + block_key 映射，不构造跨度大小的稀疏数组。
RegisterMapperResult RegisterMapper::map_devices(
    const MasterNodeConfig& master_config,
    const std::vector<const DeviceConfig*>& devices,
    const RegisterMapperRuntimePlan& runtime_plan,
    const std::vector<DeviceReadResult>& device_read_results,
    TimestampMs update_time_ms)
{
    // 按设备标识索引读取结果，同时记录重复结果。
    RegisterMapperResult result;
    result.success = true;
    result.device_statuses.reserve(devices.size());

    std::unordered_map<std::string, const DeviceReadResult*> read_results_by_device;
    std::unordered_set<std::string> duplicate_device_results;
    read_results_by_device.reserve(device_read_results.size());
    for (const auto& device_result : device_read_results) {
        const auto inserted = read_results_by_device.emplace(
            device_result.device_id,
            &device_result);
        if (!inserted.second) {
            inserted.first->second = nullptr;
            duplicate_device_results.insert(device_result.device_id);
            result.success = false;
            result.errors.push_back(
                "主控 " + master_config.master_id + " 的设备读取结果重复: " +
                device_result.device_id);
        }
    }

    // 获取设备类型；缺失时为每台设备生成统一的配置错误状态。
    if (runtime_plan.device_template == nullptr) {
        result.success = false;
        for (const auto* device : devices) {
            if (device == nullptr) {
                result.errors.push_back("主控 " + master_config.master_id + " 存在空设备配置");
                continue;
            }
            result.errors.push_back(build_device_error(
                master_config,
                *device,
                "所属主控未配置有效设备模板"));
            result.device_statuses.push_back(build_failed_status(*device, update_time_ms));
        }
        return result;
    }

    const auto& device_template = *runtime_plan.device_template;

    struct ResolvedBlock {
        const DeviceTemplateReadBlockDefinition* definition{nullptr};
        const DeviceReadBlockResult* result{nullptr};
        bool usable{false};
        bool internal_error{false};
        std::string message;
    };

    // 逐台设备校验读取结果的索引和基地址。
    for (std::size_t device_index = 0; device_index < devices.size(); ++device_index) {
        const auto* device = devices[device_index];
        if (device == nullptr) {
            result.success = false;
            result.errors.push_back("主控 " + master_config.master_id + " 存在空设备配置");
            continue;
        }

        auto status = build_initial_status(*device, device_template, update_time_ms);
        status.points.reserve(runtime_plan.ordered_fields.size());
        DeviceValueStatusCode value_status_code = DeviceValueStatusCode::kNone;
        DiagnosisErrorCode value_status_error_code = DiagnosisErrorCode::kNone;
        DiagnosisRunStatus value_status_run_status = DiagnosisRunStatus::kNormal;
        std::string value_status_message;

        const DeviceReadResult* device_result = nullptr;
        std::string device_result_error;
        const auto device_result_it = read_results_by_device.find(device->device_id);
        if (device_result_it == read_results_by_device.end()) {
            device_result_error = "缺少设备读取结果";
            result.success = false;
        } else if (device_result_it->second == nullptr ||
                   duplicate_device_results.find(device->device_id) !=
                       duplicate_device_results.end()) {
            device_result_error = "设备读取结果重复";
            result.success = false;
        } else {
            device_result = device_result_it->second;
            const auto expected_device_base =
                static_cast<std::uint64_t>(master_config.block_start_register) +
                static_cast<std::uint64_t>(device_index) *
                    static_cast<std::uint64_t>(device_template.device_address_stride);
            if (device_result->device_index != device_index) {
                device_result_error =
                    "设备序号不匹配，期望 " + std::to_string(device_index) +
                    "，实际 " + std::to_string(device_result->device_index);
                result.success = false;
            } else if (expected_device_base > std::numeric_limits<std::uint32_t>::max() ||
                       device_result->device_base_address != expected_device_base) {
                device_result_error =
                    "设备基地址不匹配，期望 " + std::to_string(expected_device_base) +
                    "，实际 " + std::to_string(device_result->device_base_address);
                result.success = false;
            }
        }

        std::unordered_map<std::string, ResolvedBlock> resolved_blocks;
        resolved_blocks.reserve(device_template.read_blocks.size());
        std::size_t usable_block_count = 0;
        std::vector<std::string> failed_block_labels;
        std::string first_block_error;
        DiagnosisErrorCode first_block_error_code = DiagnosisErrorCode::kDeviceParseFailed;

        // 解析并校验各读取区块，区分通信失败与内部结果不一致。
        for (const auto& block : device_template.read_blocks) {
            ResolvedBlock resolved;
            resolved.definition = &block;
            if (!device_result_error.empty()) {
                resolved.internal_error = true;
                resolved.message = read_block_context(block) + "：" + device_result_error;
            } else {
                const auto block_it = device_result->read_blocks.find(block.block_key);
                if (block_it == device_result->read_blocks.end()) {
                    resolved.internal_error = true;
                    resolved.message = read_block_context(block) + "：缺少该区块采集结果";
                } else {
                    resolved.result = &block_it->second;
                    const auto& block_result = block_it->second;
                    if (!block_result.success) {
                        const auto detail = block_result.error_message.empty()
                                                ? std::string("读取失败")
                                                : block_result.error_message;
                        resolved.message = read_block_context(block) + " 读取失败：" + detail;
                    } else if (block_result.block_key != block.block_key) {
                        resolved.internal_error = true;
                        resolved.message =
                            read_block_context(block) + "：结果 block_key 不匹配，实际=" +
                            (block_result.block_key.empty()
                                 ? std::string("<空>")
                                 : block_result.block_key);
                    } else if (block_result.function_code != block.function_code) {
                        resolved.internal_error = true;
                        resolved.message =
                            read_block_context(block) + "：结果功能码不匹配，期望 FC" +
                            std::to_string(block.function_code) + "，实际 FC" +
                            std::to_string(block_result.function_code);
                    } else if (block_result.register_count != block.register_count) {
                        resolved.internal_error = true;
                        resolved.message =
                            read_block_context(block) + "：请求寄存器数量不匹配，期望 " +
                            std::to_string(block.register_count) + "，实际 " +
                            std::to_string(block_result.register_count);
                    } else {
                        const auto expected_address =
                            static_cast<std::uint64_t>(device_result->device_base_address) +
                            static_cast<std::uint64_t>(block.start_offset);
                        if (expected_address > std::numeric_limits<std::uint32_t>::max() ||
                            block_result.request_address != expected_address) {
                            resolved.internal_error = true;
                            resolved.message =
                                read_block_context(block) + "：请求地址不匹配，期望 " +
                                std::to_string(expected_address) + "，实际 " +
                                std::to_string(block_result.request_address);
                        } else if (block_result.registers.size() != block.register_count) {
                            resolved.internal_error = true;
                            resolved.message =
                                read_block_context(block) + "：响应寄存器数量不匹配，期望 " +
                                std::to_string(block.register_count) + "，实际 " +
                                std::to_string(block_result.registers.size());
                        } else {
                            resolved.usable = true;
                            ++usable_block_count;
                        }
                    }
                }
            }

            if (!resolved.usable) {
                if (resolved.internal_error) {
                    result.success = false;
                }
                result.errors.push_back(build_device_error(
                    master_config,
                    *device,
                    resolved.message));
                failed_block_labels.push_back(read_block_label(block));
                if (first_block_error.empty()) {
                    first_block_error = resolved.message;
                    first_block_error_code = effective_block_error_code(resolved.result);
                }
            }
            const auto inserted = resolved_blocks.emplace(block.block_key, std::move(resolved));
            if (!inserted.second) {
                result.success = false;
                const auto error_message =
                    read_block_context(block) + "：设备模板区块标识重复";
                result.errors.push_back(build_device_error(
                    master_config,
                    *device,
                    error_message));
                inserted.first->second.usable = false;
                inserted.first->second.internal_error = true;
                inserted.first->second.message = error_message;
            }
        }

        // 从可用区块解码字段；不可用区块仍生成质量为坏的点位。
        for (const auto& field_runtime : runtime_plan.ordered_fields) {
            const auto* field = field_runtime.definition;
            if (field == nullptr) {
                continue;
            }
            const auto resolved_it = resolved_blocks.find(field->read_block_key);
            if (resolved_it == resolved_blocks.end()) {
                const auto point_error =
                    "字段 " + field->field_key + " 引用的读取区块不存在：" +
                    field->read_block_key;
                result.success = false;
                result.errors.push_back(build_device_error(
                    master_config,
                    *device,
                    point_error));
                status.points.push_back(build_point_value(
                    *field,
                    0.0,
                    0.0,
                    DataQuality::kBad,
                    false,
                    point_error,
                    update_time_ms));
                continue;
            }

            const auto& resolved = resolved_it->second;
            if (!resolved.usable || resolved.result == nullptr) {
                status.points.push_back(build_point_value(
                    *field,
                    0.0,
                    0.0,
                    DataQuality::kBad,
                    false,
                    resolved.message,
                    update_time_ms));
                if (field->field_key == kResistanceFieldKey) {
                    status.has_resistance = true;
                    status.resistance_value = 0.0;
                }
                continue;
            }

            const auto field_offset = static_cast<std::size_t>(field->register_offset);
            const auto read_block_size =
                static_cast<std::size_t>(resolved.definition->register_count);
            const auto block_registers_available =
                field_offset <= read_block_size
                    ? read_block_size - field_offset
                    : 0;
            auto decoded = decode_field_value(
                field_runtime,
                resolved.result->registers,
                field_offset,
                block_registers_available);

            std::string point_message = decoded.message;
            if (!decoded.definition_valid) {
                point_message =
                    read_block_context(*resolved.definition) + "：" + decoded.message;
                const auto error_message = build_device_error(
                    master_config,
                    *device,
                    "设备模板字段定义无效: " + field->field_key + "（" +
                        point_message + "）");
                result.success = false;
                result.errors.push_back(error_message);
            }
            const auto point_quality = decoded.valid ? DataQuality::kGood : DataQuality::kBad;
            status.points.push_back(build_point_value(
                *field,
                decoded.value,
                decoded.raw_value,
                point_quality,
                decoded.valid,
                point_message,
                update_time_ms,
                decoded.display_text));
            if (decoded.value_status_code != DeviceValueStatusCode::kNone &&
                device_value_status_priority(decoded.value_status_code) >
                    device_value_status_priority(value_status_code)) {
                value_status_code = decoded.value_status_code;
                value_status_error_code = decoded.value_status_error_code;
                value_status_run_status = decoded.value_status_run_status;
                value_status_message = field->display_name + "：" + point_message;
            }
            if (field->field_key == kResistanceFieldKey) {
                status.has_resistance = true;
                status.resistance_value = decoded.value;
            }
        }

        // 汇总区块成功率并设置设备级诊断和数据质量。
        const auto expected_block_count = device_template.read_blocks.size();
        if (usable_block_count == expected_block_count &&
            value_status_code != DeviceValueStatusCode::kNone) {
            // 数值状态码表示设备已经应答，但设备自身处于故障态；不能误降级为通讯离线。
            status.online = true;
            status.last_collect_success = true;
            status.communication_quality = DataQuality::kGood;
            status.last_failure_time_ms = update_time_ms;
            status.last_error_message = value_status_message;
            status.diagnosis = make_diagnosis(
                DiagnosisLevel::kDevice,
                device->device_id,
                device->device_name,
                value_status_run_status,
                value_status_error_code,
                status.last_success_time_ms,
                update_time_ms,
                1);
            status.diagnosis.message = value_status_message;
        } else if (usable_block_count < expected_block_count) {
            const auto any_block_succeeded = usable_block_count > 0;
            const auto block_failure_summary = compact_block_failure_summary(
                failed_block_labels,
                first_block_error,
                any_block_succeeded);
            status.online = any_block_succeeded;
            status.last_collect_success = false;
            status.communication_quality = any_block_succeeded
                                      ? DataQuality::kPartial
                                      : DataQuality::kBad;
            status.last_failure_time_ms = update_time_ms;
            if (!any_block_succeeded) {
                status.last_success_time_ms = 0;
            }
            status.last_error_message = block_failure_summary;
            status.diagnosis = make_diagnosis(
                DiagnosisLevel::kDevice,
                device->device_id,
                device->device_name,
                any_block_succeeded
                    ? DiagnosisRunStatus::kWarning
                    : DiagnosisRunStatus::kOffline,
                first_block_error_code,
                status.last_success_time_ms,
                update_time_ms,
                status.diagnosis.consecutive_failures + 1);
            status.diagnosis.message = block_failure_summary;
        }

        result.device_statuses.push_back(std::move(status));
    }

    return result;
}

}  // namespace edge_controller
