// IPC 协议层：实现长度前缀帧、请求/响应包络和消息尺寸上限，不执行具体业务方法。
#include "application/interface/ipc_protocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "communication/protocol/modbus_rtu_protocol.h"

namespace edge_controller::ipc_protocol {

namespace {

std::string error_domain(const std::string& code)
{
    if (code == "invalid_request" || code == "invalid_argument" || code == "method_not_found") {
        return "request";
    }
    if (code == "not_found") return "resource";
    if (code == "conflict" || code == "invalid_state") return "state";
    if (code == "timeout" || code == "io_error" || code == "protocol_error") return "transport";
    return "service";
}

bool error_retryable(const std::string& code)
{
    return code == "timeout" || code == "io_error" || code == "server_busy" ||
           code == "internal_error";
}

std::string to_lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

StatusCode parse_serial_parity(const std::string& text, SerialParity* output)
{
    const auto normalized = to_lower_copy(text);
    if (normalized == "none" || normalized == "n") { *output = SerialParity::kNone; return StatusCode::kOk; }
    if (normalized == "even" || normalized == "e") { *output = SerialParity::kEven; return StatusCode::kOk; }
    if (normalized == "odd" || normalized == "o") { *output = SerialParity::kOdd; return StatusCode::kOk; }
    return StatusCode::kInvalidArgument;
}

StatusCode parse_channel_type(const std::string& text, ChannelType* output)
{
    const auto normalized = to_lower_copy(text);
    if (normalized == "modbus_rtu_serial") { *output = ChannelType::kModbusRtuSerial; return StatusCode::kOk; }
    if (normalized == "modbus_tcp") { *output = ChannelType::kModbusTcp; return StatusCode::kOk; }
    return StatusCode::kInvalidArgument;
}

StatusCode parse_master_protocol(const std::string& text, MasterProtocol* output)
{
    const auto normalized = to_lower_copy(text);
    if (normalized == "modbus_rtu") { *output = MasterProtocol::kModbusRtu; return StatusCode::kOk; }
    if (normalized == "modbus_tcp") { *output = MasterProtocol::kModbusTcp; return StatusCode::kOk; }
    return StatusCode::kInvalidArgument;
}

// 查找字段。
const nlohmann::json* find_field(const nlohmann::json& object, const std::string& field_name)
{
    if (!object.is_object()) {
        return nullptr;
    }
    const auto iterator = object.find(field_name);
    return iterator == object.end() ? nullptr : &*iterator;
}

// 返回 IPC 请求中的参数对象。
const nlohmann::json* request_params_object(const nlohmann::json& request)
{
    const auto* params = find_field(request, "params");
    if (params == nullptr || !params->is_object()) {
        return nullptr;
    }
    return params;
}

// 将 JSON 数值安全转换为双精度浮点数。
bool number_as_double(const nlohmann::json& value, double* output)
{
    if (!value.is_number() || output == nullptr) {
        return false;
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number)) {
        return false;
    }
    *output = number;
    return true;
}

// 将 JSON 数值安全转换为 64 位无符号整数。
bool number_as_uint64(const nlohmann::json& value, std::uint64_t* output)
{
    if (!value.is_number() || output == nullptr) {
        return false;
    }
    if (value.is_number_unsigned()) {
        *output = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < 0) {
            return false;
        }
        *output = static_cast<std::uint64_t>(number);
        return true;
    }
    double number = 0.0;
    constexpr double kUint64ExclusiveUpperBound = 18446744073709551616.0;
    if (!number_as_double(value, &number) || number < 0.0 ||
        number >= kUint64ExclusiveUpperBound || std::floor(number) != number) {
        return false;
    }
    *output = static_cast<std::uint64_t>(number);
    return true;
}

// 将 JSON 数值安全转换为 64 位有符号整数。
bool number_as_int64(const nlohmann::json& value, std::int64_t* output)
{
    if (output == nullptr) return false;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        *output = static_cast<std::int64_t>(number);
        return true;
    }
    if (value.is_number_integer()) {
        *output = value.get<std::int64_t>();
        return true;
    }
    return false;
}

// 在范围校验后将 JSON 数值转换为 32 位无符号整数。
bool number_as_uint32(const nlohmann::json& value, std::uint32_t max_value, std::uint32_t* output)
{
    if (!value.is_number() || output == nullptr) {
        return false;
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > max_value) {
            return false;
        }
        *output = static_cast<std::uint32_t>(number);
        return true;
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < 0 || static_cast<std::uint64_t>(number) > max_value) {
            return false;
        }
        *output = static_cast<std::uint32_t>(number);
        return true;
    }
    double number = 0.0;
    if (!number_as_double(value, &number) || number < 0.0 || std::floor(number) != number ||
        number > static_cast<double>(max_value)) {
        return false;
    }
    *output = static_cast<std::uint32_t>(number);
    return true;
}

// 读取并校验字符串字段。
StatusCode require_string_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::string* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !value->is_string()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + field_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<std::string>();
    return StatusCode::kOk;
}

// 读取并校验布尔字段。
StatusCode require_bool_field(
    const nlohmann::json& object,
    const std::string& field_name,
    bool* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !value->is_boolean()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + field_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<bool>();
    return StatusCode::kOk;
}

// 读取并校验无符号整数字段。
StatusCode require_uint_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::uint32_t* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !value->is_number()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + field_name;
        }
        return StatusCode::kInvalidArgument;
    }
    std::uint32_t parsed = 0;
    if (!number_as_uint32(*value, std::numeric_limits<std::uint32_t>::max(), &parsed)) {
        if (error_message != nullptr) {
            *error_message =
                "params." + field_name + " 的值非法: " + value->dump() +
                "，必须是 0 到 " + std::to_string(std::numeric_limits<std::uint32_t>::max()) + " 范围内的整数";
        }
        return StatusCode::kInvalidArgument;
    }
    *output = parsed;
    return StatusCode::kOk;
}

// 读取并校验 JSON 对象中的可选 64 位无符号整数字段。
StatusCode optional_uint64_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::uint64_t* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr) {
        return StatusCode::kOk;
    }
    if (!number_as_uint64(*value, output)) {
        if (error_message != nullptr) {
            *error_message = "params." + field_name + " 必须是非负整数";
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 读取并校验 64 位无符号整数字段。
StatusCode require_uint64_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::uint64_t* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !number_as_uint64(*value, output)) {
        if (error_message != nullptr) *error_message = "缺少或非法的 params." + field_name + "，必须是非负整数";
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 读取并校验限定范围的无符号整数字段。
StatusCode require_uint_range_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::uint32_t min_value,
    std::uint32_t max_value,
    std::uint32_t* output,
    std::string* error_message)
{
    std::uint32_t value = 0;
    const auto status = require_uint_field(object, field_name, &value, error_message);
    if (!is_ok(status)) {
        return status;
    }
    if (value < min_value || value > max_value) {
        if (error_message != nullptr) {
            *error_message =
                "params." + field_name + " 的值非法: " + std::to_string(value) +
                "，必须是 " + std::to_string(min_value) + " 到 " + std::to_string(max_value) + " 范围内的整数";
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value;
    return StatusCode::kOk;
}

// 读取并校验 JSON 对象中的可选字符串字段。
StatusCode optional_string_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::string* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr) {
        return StatusCode::kOk;
    }
    if (!value->is_string()) {
        if (error_message != nullptr) {
            *error_message = "params." + field_name + " 必须是字符串";
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<std::string>();
    return StatusCode::kOk;
}

// 读取并校验限定范围的可选无符号整数字段。
StatusCode optional_uint_range_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::uint32_t min_value,
    std::uint32_t max_value,
    std::uint32_t* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr) {
        return StatusCode::kOk;
    }
    return require_uint_range_field(object, field_name, min_value, max_value, output, error_message);
}

// 读取并校验 JSON 对象中的可选布尔字段。
StatusCode optional_bool_field(
    const nlohmann::json& object,
    const std::string& field_name,
    bool* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr) {
        return StatusCode::kOk;
    }
    if (!value->is_boolean()) {
        if (error_message != nullptr) {
            *error_message = "params." + field_name + " 必须是布尔值";
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<bool>();
    return StatusCode::kOk;
}

// 从多个兼容字段名中查找首个存在的字段。
const nlohmann::json* find_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names)
{
    for (const auto* field_name : field_names) {
        const auto* value = find_field(object, field_name);
        if (value != nullptr) {
            return value;
        }
    }
    return nullptr;
}

// 从多个兼容字段名中读取字符串。
StatusCode require_string_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    std::string* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    if (value == nullptr || !value->is_string()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + display_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<std::string>();
    return StatusCode::kOk;
}

// 从多个兼容字段名中读取可选字符串。
StatusCode optional_string_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    std::string* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    if (value == nullptr) {
        return StatusCode::kOk;
    }
    if (!value->is_string()) {
        if (error_message != nullptr) {
            *error_message = "params." + display_name + " 必须是字符串";
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<std::string>();
    return StatusCode::kOk;
}

// 从多个兼容字段名中读取 16 位无符号整数。
StatusCode require_uint16_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    std::uint16_t* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    std::uint32_t parsed = 0;
    if (value == nullptr ||
        !number_as_uint32(*value, std::numeric_limits<std::uint16_t>::max(), &parsed)) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + display_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = static_cast<std::uint16_t>(parsed);
    return StatusCode::kOk;
}

// 从多个兼容字段名中读取 32 位无符号整数。
StatusCode require_uint32_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    std::uint32_t* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    std::uint32_t parsed = 0;
    if (value == nullptr ||
        !number_as_uint32(*value, std::numeric_limits<std::uint32_t>::max(), &parsed)) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + display_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = parsed;
    return StatusCode::kOk;
}

// 从多个兼容字段名中读取双精度浮点数。
StatusCode require_double_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    double* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    double parsed = 0.0;
    if (value == nullptr || !number_as_double(*value, &parsed)) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + display_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = parsed;
    return StatusCode::kOk;
}

// 从多个兼容字段名中读取可选浮点数。
StatusCode optional_double_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    double* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    if (value == nullptr) {
        return StatusCode::kOk;
    }
    double parsed = 0.0;
    if (!number_as_double(*value, &parsed)) {
        if (error_message != nullptr) {
            *error_message = "params." + display_name + " 必须是有效数字";
        }
        return StatusCode::kInvalidArgument;
    }
    *output = parsed;
    return StatusCode::kOk;
}

// 从多个兼容字段名中读取布尔值。
StatusCode require_bool_any_field(
    const nlohmann::json& object,
    std::initializer_list<const char*> field_names,
    const std::string& display_name,
    bool* output,
    std::string* error_message)
{
    const auto* value = find_any_field(object, field_names);
    if (value == nullptr || !value->is_boolean()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + display_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value->get<bool>();
    return StatusCode::kOk;
}

// 读取并校验字符串数组字段。
StatusCode require_string_array_field(
    const nlohmann::json& object,
    const std::string& field_name,
    std::vector<std::string>* output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !value->is_array()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + field_name;
        }
        return StatusCode::kInvalidArgument;
    }

    std::vector<std::string> values;
    values.reserve(value->size());
    for (std::size_t index = 0; index < value->size(); ++index) {
        const auto& item = (*value)[index];
        if (!item.is_string()) {
            if (error_message != nullptr) {
                *error_message = "params." + field_name + "[" + std::to_string(index) + "] 必须是字符串";
            }
            return StatusCode::kInvalidArgument;
        }
        values.push_back(item.get<std::string>());
    }

    if (output != nullptr) {
        *output = std::move(values);
    }
    return StatusCode::kOk;
}

// 规范化输入并返回稳定结果。
std::uint32_t normalized_communication_trace_limit(const nlohmann::json* value)
{
    if (value == nullptr) {
        return kCommunicationTraceDefaultLimit;
    }

    std::uint32_t parsed = 0;
    if (!number_as_uint32(*value, std::numeric_limits<std::uint32_t>::max(), &parsed) || parsed == 0) {
        return kCommunicationTraceDefaultLimit;
    }

    if (parsed > kCommunicationTraceMaxLimit) {
        return kCommunicationTraceMaxLimit;
    }

    return parsed;
}

// 读取并校验 JSON 对象字段。
StatusCode require_object_field(
    const nlohmann::json& object,
    const std::string& field_name,
    const nlohmann::json** output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !value->is_object()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + field_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value;
    return StatusCode::kOk;
}

// 读取并校验 JSON 数组字段。
StatusCode require_array_field(
    const nlohmann::json& object,
    const std::string& field_name,
    const nlohmann::json** output,
    std::string* error_message)
{
    const auto* value = find_field(object, field_name);
    if (value == nullptr || !value->is_array()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params." + field_name;
        }
        return StatusCode::kInvalidArgument;
    }
    *output = value;
    return StatusCode::kOk;
}

// 解析输入并写入结构化结果。
StatusCode parse_system_settings_object(
    const nlohmann::json& object,
    SystemSettings* settings,
    std::string* error_message)
{
    if (!is_ok(require_string_field(object, "device_name", &settings->device_name, error_message)) ||
        !is_ok(require_string_field(object, "site_location", &settings->site_location, error_message)) ||
        !is_ok(require_string_field(object, "display_name", &settings->display_name, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 解析输入并写入结构化结果。
StatusCode parse_network_settings_object(
    const nlohmann::json& object,
    NetworkSettings* settings,
    std::string* error_message)
{
    if (!is_ok(require_string_field(object, "mode", &settings->mode, error_message)) ||
        !is_ok(require_string_field(object, "interface_name", &settings->interface_name, error_message)) ||
        !is_ok(require_string_field(object, "ip_address", &settings->ip_address, error_message)) ||
        !is_ok(require_string_field(object, "netmask", &settings->netmask, error_message)) ||
        !is_ok(require_string_field(object, "gateway", &settings->gateway, error_message)) ||
        !is_ok(require_string_array_field(object, "dns_servers", &settings->dns_servers, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 从 JSON 对象解析时间设置。
StatusCode parse_time_settings_object(
    const nlohmann::json& object,
    TimeSettings* settings,
    std::string* error_message)
{
    if (!is_ok(require_string_field(object, "timezone", &settings->timezone, error_message)) ||
        !is_ok(require_bool_field(object, "ntp_enabled", &settings->ntp_enabled, error_message)) ||
        !is_ok(require_string_field(object, "ntp_primary", &settings->ntp_primary, error_message)) ||
        !is_ok(require_string_field(object, "ntp_secondary", &settings->ntp_secondary, error_message)) ||
        !is_ok(require_uint64_field(object, "updated_at", &settings->updated_at, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 从 JSON 对象解析 MQTT 设置。
StatusCode parse_mqtt_settings_object(
    const nlohmann::json& object,
    MqttSettings* settings,
    std::string* error_message)
{
    std::uint32_t broker_port = settings->broker_port;
    std::uint32_t publish_interval = settings->publish_interval_seconds;
    std::uint32_t keep_alive = settings->keep_alive_seconds;
    std::uint32_t qos = static_cast<std::uint32_t>(settings->qos);
    if (!is_ok(require_bool_field(object, "enabled", &settings->enabled, error_message)) ||
        !is_ok(require_string_field(object, "broker_host", &settings->broker_host, error_message)) ||
        !is_ok(require_uint_range_field(object, "broker_port", 1, 65535, &broker_port, error_message)) ||
        !is_ok(require_string_field(object, "client_id", &settings->client_id, error_message)) ||
        !is_ok(require_string_field(object, "node_id", &settings->node_id, error_message)) ||
        !is_ok(require_string_field(object, "username", &settings->username, error_message)) ||
        !is_ok(require_string_field(object, "topic_prefix", &settings->topic_prefix, error_message)) ||
        !is_ok(require_uint_range_field(object, "publish_interval_seconds", 1, 3600, &publish_interval, error_message)) ||
        !is_ok(require_uint_range_field(object, "qos", 0, 1, &qos, error_message)) ||
        !is_ok(require_bool_field(object, "retain_status", &settings->retain_status, error_message)) ||
        !is_ok(require_uint_range_field(object, "keep_alive_seconds", 10, 300, &keep_alive, error_message)) ||
        !is_ok(require_bool_field(object, "tls_enabled", &settings->tls_enabled, error_message)) ||
        !is_ok(require_string_field(object, "tls_ca_file", &settings->tls_ca_file, error_message)) ||
        !is_ok(require_string_field(object, "tls_client_cert_file", &settings->tls_client_cert_file, error_message)) ||
        !is_ok(require_string_field(object, "tls_client_key_file", &settings->tls_client_key_file, error_message)) ||
        !is_ok(require_bool_field(object, "tls_insecure", &settings->tls_insecure, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    settings->broker_port = static_cast<std::uint16_t>(broker_port);
    settings->publish_interval_seconds = publish_interval;
    settings->qos = static_cast<int>(qos);
    settings->keep_alive_seconds = keep_alive;
    settings->password.clear();
    return StatusCode::kOk;
}

// 从 JSON 对象解析 Modbus 服务设置。
StatusCode parse_modbus_server_settings_object(
    const nlohmann::json& object,
    ModbusServerSettings* settings,
    std::string* error_message)
{
    std::uint32_t listen_port = 0, unit_id = 0, max_clients = 0, idle_timeout = 0, max_read = 0;
    if (!is_ok(require_bool_field(object, "enabled", &settings->enabled, error_message)) ||
        !is_ok(require_string_field(object, "listen_address", &settings->listen_address, error_message)) ||
        !is_ok(require_uint_range_field(object, "listen_port", 1, 65535, &listen_port, error_message)) ||
        !is_ok(require_uint_range_field(object, "unit_id", 0, 255, &unit_id, error_message)) ||
        !is_ok(require_bool_field(object, "strict_unit_id", &settings->strict_unit_id, error_message)) ||
        !is_ok(require_uint_range_field(object, "max_clients", 1, 64, &max_clients, error_message)) ||
        !is_ok(require_uint_range_field(object, "idle_timeout_seconds", 5, 3600, &idle_timeout, error_message)) ||
        !is_ok(require_uint_range_field(object, "max_read_registers", 1, 125, &max_read, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    settings->listen_port = static_cast<std::uint16_t>(listen_port);
    settings->unit_id = static_cast<std::uint8_t>(unit_id);
    settings->max_clients = max_clients;
    settings->idle_timeout_seconds = idle_timeout;
    settings->max_read_registers = static_cast<std::uint16_t>(max_read);
    normalize_modbus_server_settings(settings);
    return validate_modbus_server_settings(*settings, "modbus_server_settings", error_message);
}

// 从 JSON 对象解析 Modbus 寄存器映射。
StatusCode parse_modbus_mapping_object(
    const nlohmann::json& object,
    ModbusRegisterMapping* mapping,
    std::string* error_message)
{
    std::string data_type, byte_order, word_order;
    std::uint32_t start_address = 0, quality_address = 0;
    if (!is_ok(require_string_field(object, "mapping_id", &mapping->mapping_id, error_message)) ||
        !is_ok(require_string_field(object, "device_id", &mapping->device_id, error_message)) ||
        !is_ok(require_string_field(object, "point_key", &mapping->point_key, error_message)) ||
        !is_ok(require_string_field(object, "device_name_snapshot", &mapping->device_name_snapshot, error_message)) ||
        !is_ok(require_string_field(object, "point_name_snapshot", &mapping->point_name_snapshot, error_message)) ||
        !is_ok(require_uint_range_field(object, "start_address", 0, 65535, &start_address, error_message)) ||
        !is_ok(require_string_field(object, "data_type", &data_type, error_message)) ||
        !is_ok(require_double_any_field(object, {"value_multiplier"}, "value_multiplier", &mapping->value_multiplier, error_message)) ||
        !is_ok(require_double_any_field(object, {"value_offset"}, "value_offset", &mapping->value_offset, error_message)) ||
        !is_ok(require_string_field(object, "byte_order", &byte_order, error_message)) ||
        !is_ok(require_string_field(object, "word_order", &word_order, error_message)) ||
        !is_ok(require_uint_range_field(object, "quality_address", 0, 65535, &quality_address, error_message)) ||
        !is_ok(require_bool_field(object, "enabled", &mapping->enabled, error_message)) ||
        !is_ok(require_uint64_field(object, "created_at_ms", &mapping->created_at_ms, error_message)) ||
        !is_ok(require_uint64_field(object, "updated_at_ms", &mapping->updated_at_ms, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (!parse_modbus_register_data_type(data_type, &mapping->data_type) ||
        !parse_modbus_byte_order(byte_order, &mapping->byte_order) ||
        !parse_modbus_word_order(word_order, &mapping->word_order)) {
        if (error_message != nullptr) *error_message = "Modbus 映射包含不支持的数据类型、字节序或字序";
        return StatusCode::kInvalidArgument;
    }
    mapping->start_address = static_cast<RegisterAddress>(start_address);
    mapping->quality_address = static_cast<RegisterAddress>(quality_address);
    normalize_modbus_register_mapping(mapping);
    return validate_modbus_register_mapping(*mapping, "modbus_register_mapping", error_message);
}

// 从 JSON 对象解析通道配置。
StatusCode parse_channel_config_object(
    const nlohmann::json& object,
    ChannelConfig* config,
    std::string* error_message)
{
    std::string channel_type;
    std::string parity;
    std::uint32_t tcp_port = 0;
    std::uint32_t data_bits = 0;
    std::uint32_t stop_bits = 0;
    if (!is_ok(require_string_field(object, "channel_id", &config->channel_id, error_message)) ||
        !is_ok(require_string_field(object, "channel_name", &config->channel_name, error_message)) ||
        !is_ok(require_bool_field(object, "enabled", &config->enabled, error_message)) ||
        !is_ok(require_string_field(object, "channel_type", &channel_type, error_message)) ||
        !is_ok(require_string_field(object, "device_path", &config->device_path, error_message)) ||
        !is_ok(require_string_field(object, "port_name", &config->port_name, error_message)) ||
        !is_ok(require_string_field(object, "tcp_host", &config->tcp_host, error_message)) ||
        !is_ok(require_uint_range_field(object, "tcp_port", 0, 65535, &tcp_port, error_message)) ||
        !is_ok(require_uint_field(object, "connect_timeout_ms", &config->connect_timeout_ms, error_message)) ||
        !is_ok(require_uint_field(object, "baud_rate", &config->baud_rate, error_message)) ||
        !is_ok(require_uint_range_field(object, "data_bits", 5, 8, &data_bits, error_message)) ||
        !is_ok(require_string_field(object, "parity", &parity, error_message)) ||
        !is_ok(require_uint_range_field(object, "stop_bits", 1, 2, &stop_bits, error_message)) ||
        !is_ok(require_uint_field(object, "response_timeout_ms", &config->response_timeout_ms, error_message)) ||
        !is_ok(require_uint_field(object, "retry_count", &config->retry_count, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(parse_channel_type(channel_type, &config->channel_type)) ||
        !is_ok(parse_serial_parity(parity, &config->parity))) {
        if (error_message != nullptr) {
            *error_message = "通道配置类型或校验位非法";
        }
        return StatusCode::kInvalidArgument;
    }
    if (config->port_name.empty()) config->port_name = config->device_path;
    config->tcp_port = static_cast<std::uint16_t>(tcp_port);
    config->data_bits = static_cast<std::uint8_t>(data_bits);
    config->stop_bits = static_cast<std::uint8_t>(stop_bits);
    return StatusCode::kOk;
}

// 从 JSON 对象解析主站配置。
StatusCode parse_master_config_object(
    const nlohmann::json& object,
    MasterNodeConfig* config,
    std::string* error_message)
{
    std::string protocol;
    std::uint32_t target_address = 0;
    std::uint32_t block_start = 0;
    std::uint32_t device_count = 0;
    if (!is_ok(require_string_field(object, "master_id", &config->master_id, error_message)) ||
        !is_ok(require_string_field(object, "master_name", &config->master_name, error_message)) ||
        !is_ok(require_bool_field(object, "enabled", &config->enabled, error_message)) ||
        !is_ok(require_string_field(object, "protocol", &protocol, error_message)) ||
        !is_ok(require_string_field(object, "channel_id", &config->channel_id, error_message)) ||
        !is_ok(require_uint_range_field(object, "target_address", 1, 247, &target_address, error_message)) ||
        !is_ok(require_uint_field(object, "poll_interval_ms", &config->poll_interval_ms, error_message)) ||
        !is_ok(require_uint_field(object, "response_timeout_ms", &config->response_timeout_ms, error_message)) ||
        !is_ok(require_uint_field(object, "retry_count", &config->retry_count, error_message)) ||
        !is_ok(require_string_field(object, "remark", &config->remark, error_message)) ||
        !is_ok(require_string_field(object, "device_template", &config->device_template, error_message)) ||
        !is_ok(require_uint_range_field(object, "block_start_register", 0, 65535, &block_start, error_message)) ||
        !is_ok(require_uint_range_field(
            object, "device_count", 1, kMaxDevicesPerMaster, &device_count, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(parse_master_protocol(protocol, &config->protocol))) {
        if (error_message != nullptr) {
            *error_message = "主站协议类型非法";
        }
        return StatusCode::kInvalidArgument;
    }
    config->target_address = static_cast<std::uint16_t>(target_address);
    config->block_start_register = static_cast<std::uint16_t>(block_start);
    config->device_count = device_count;
    return StatusCode::kOk;
}

// 从 JSON 对象解析告警规则。
StatusCode parse_alarm_rule_object(
    const nlohmann::json& object,
    AlarmRule* rule,
    std::string* error_message)
{
    std::uint64_t updated_at_ms = 0;
    std::uint32_t trigger_count = 0;
    std::uint32_t recovery_count = 0;
    if (!is_ok(require_string_field(object, "device_id", &rule->device_id, error_message)) ||
        !is_ok(require_string_field(object, "point_key", &rule->point_key, error_message)) ||
        !is_ok(require_bool_field(object, "enabled", &rule->enabled, error_message)) ||
        !is_ok(require_bool_field(object, "high_enabled", &rule->high_enabled, error_message)) ||
        !is_ok(require_double_any_field(object, {"high_threshold"}, "high_threshold", &rule->high_threshold, error_message)) ||
        !is_ok(require_bool_field(object, "low_enabled", &rule->low_enabled, error_message)) ||
        !is_ok(require_double_any_field(object, {"low_threshold"}, "low_threshold", &rule->low_threshold, error_message)) ||
        !is_ok(require_string_field(object, "level", &rule->level, error_message)) ||
        !is_ok(require_double_any_field(object, {"hysteresis"}, "hysteresis", &rule->hysteresis, error_message)) ||
        !is_ok(require_uint_field(object, "trigger_count", &trigger_count, error_message)) ||
        !is_ok(require_uint_field(object, "recovery_count", &recovery_count, error_message)) ||
        !is_ok(optional_uint64_field(object, "updated_at_ms", &updated_at_ms, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    rule->trigger_count = trigger_count;
    rule->recovery_count = recovery_count;
    rule->updated_at_ms = updated_at_ms;
    return StatusCode::kOk;
}

}  // namespace

// 校验载荷长度。
StatusCode validate_payload_length(std::uint32_t length, std::string* error_message)
{
    if (length == 0) {
        if (error_message != nullptr) {
            *error_message = "IPC 帧长度不能为 0";
        }
        return StatusCode::kProtocolError;
    }

    if (length > kMaxPayloadBytes) {
        if (error_message != nullptr) {
            *error_message =
                "IPC 帧长度超过上限: " + std::to_string(length) +
                "，允许的最大长度为 " + std::to_string(kMaxPayloadBytes);
        }
        return StatusCode::kProtocolError;
    }

    return StatusCode::kOk;
}

// 解析请求JSON。
StatusCode parse_request_json(
    const std::string& request_json,
    nlohmann::json* request,
    std::string* error_message)
{
    if (request == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 IPC 请求解析输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    try {
        *request = nlohmann::json::parse(request_json);
    } catch (const nlohmann::json::parse_error&) {
        if (error_message != nullptr) {
            *error_message = "请求 JSON 非法";
        }
        return StatusCode::kInvalidArgument;
    }

    if (!request->is_object()) {
        if (error_message != nullptr) {
            *error_message = "请求 JSON 非法";
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 提取 IPC 请求方法名。
StatusCode extract_request_method(
    const nlohmann::json& request,
    std::string* method,
    std::string* error_message)
{
    const auto* value = find_field(request, "method");
    if (method == nullptr || value == nullptr || !value->is_string()) {
        if (error_message != nullptr) {
            *error_message = "缺少 method 字段";
        }
        return StatusCode::kInvalidArgument;
    }
    *method = value->get<std::string>();
    return StatusCode::kOk;
}

// 提取请求标识；缺失或无效时返回 JSON null。
nlohmann::json request_id_json_or_null(const nlohmann::json& request)
{
    const auto* id = find_field(request, "id");
    if (id == nullptr) {
        return nullptr;
    }
    if (id->is_string()) {
        return *id;
    }
    if (id->is_number_integer() || id->is_number_unsigned()) {
        return *id;
    }
    if (id->is_number_float() && std::isfinite(id->get<double>())) {
        return *id;
    }
    return nullptr;
}

// 提取通道标识。
std::string extract_channel_id(const nlohmann::json& request)
{
    const auto* params = request_params_object(request);
    if (params == nullptr) {
        return {};
    }

    const auto* channel_id = find_field(*params, "channel_id");
    if (channel_id == nullptr || !channel_id->is_string()) {
        return {};
    }

    return channel_id->get<std::string>();
}

// 构造成功响应。
std::string build_success_response(const nlohmann::json& id, const nlohmann::json& result)
{
    return nlohmann::json{
        {"id", id},
        {"success", true},
        {"result", result},
    }.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// 构造错误信息响应。
std::string build_error_response(
    const nlohmann::json& id,
    const std::string& code,
    const std::string& message)
{
    return nlohmann::json{
        {"id", id},
        {"success", false},
        {"error", {
            {"code", code},
            {"domain", error_domain(code)},
            {"message", message},
            {"params", nlohmann::json::object()},
            {"retryable", error_retryable(code)},
        }},
    }.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// 提取主站标识。
std::string extract_master_id(const nlohmann::json& request)
{
    const auto* params = request_params_object(request);
    if (params == nullptr) {
        return {};
    }

    const auto* master_id = find_field(*params, "master_id");
    if (master_id == nullptr || !master_id->is_string()) {
        return {};
    }

    return master_id->get<std::string>();
}

// 提取设备标识。
std::string extract_device_id(const nlohmann::json& request)
{
    const auto* params = request_params_object(request);
    if (params == nullptr) {
        return {};
    }

    const auto* device_id = find_field(*params, "device_id");
    if (device_id == nullptr || !device_id->is_string()) {
        return {};
    }

    return device_id->get<std::string>();
}

// 提取设备历史数据查询参数。
StatusCode extract_device_history_query_request(
    const nlohmann::json& request,
    DeviceHistoryQuery* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少设备历史查询请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    DeviceHistoryQuery parsed;
    if (!is_ok(require_string_field(*params, "device_id", &parsed.device_id, error_message)) ||
        !is_ok(optional_uint_range_field(*params, "days", 1, 3650, &parsed.days, error_message)) ||
        !is_ok(optional_string_field(*params, "start_date", &parsed.start_date, error_message)) ||
        !is_ok(optional_string_field(*params, "end_date", &parsed.end_date, error_message)) ||
        !is_ok(optional_string_field(*params, "point_key", &parsed.point_key, error_message)) ||
        !is_ok(optional_string_field(*params, "sample_period", &parsed.sample_period, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (parsed.sample_period.empty()) {
        parsed.sample_period = "day";
    }
    if (parsed.sample_period != "hour" && parsed.sample_period != "day") {
        if (error_message != nullptr) {
            *error_message = "params.sample_period 非法，必须是 hour 或 day";
        }
        return StatusCode::kInvalidArgument;
    }

    *result = parsed;
    return StatusCode::kOk;
}

// 提取通道通讯报文查询参数。
StatusCode extract_channel_communication_trace_query_request(
    const nlohmann::json& request,
    ChannelId* channel_id,
    std::uint32_t* limit,
    std::string* error_message)
{
    if (channel_id == nullptr || limit == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少通讯报文查询请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    ChannelId parsed_channel_id;
    if (!is_ok(require_string_field(*params, "channel_id", &parsed_channel_id, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (parsed_channel_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "params.channel_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    *channel_id = std::move(parsed_channel_id);
    *limit = normalized_communication_trace_limit(find_field(*params, "limit"));
    return StatusCode::kOk;
}

// 提取通道通讯报文清理参数。
StatusCode extract_channel_communication_trace_clear_request(
    const nlohmann::json& request,
    ChannelId* channel_id,
    std::string* error_message)
{
    if (channel_id == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少通讯报文清空请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    ChannelId parsed_channel_id;
    if (!is_ok(require_string_field(*params, "channel_id", &parsed_channel_id, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (parsed_channel_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "params.channel_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    *channel_id = std::move(parsed_channel_id);
    return StatusCode::kOk;
}

// 提取设备命令执行请求。
StatusCode extract_device_command_execute_request(
    const nlohmann::json& request,
    DeviceCommandExecuteRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少设备命令执行请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    DeviceCommandExecuteRequest parsed;
    if (!is_ok(require_string_field(*params, "device_id", &parsed.device_id, error_message)) ||
        !is_ok(require_string_field(*params, "command_key", &parsed.command_key, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (parsed.device_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "params.device_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (parsed.command_key.empty()) {
        if (error_message != nullptr) {
            *error_message = "params.command_key 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* values = find_field(*params, "values");
    if (values == nullptr || !values->is_object()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params.values，必须是对象";
        }
        return StatusCode::kInvalidArgument;
    }

    for (auto iterator = values->cbegin(); iterator != values->cend(); ++iterator) {
        std::uint32_t parsed_value = 0;
        if (!number_as_uint32(iterator.value(), std::numeric_limits<std::uint16_t>::max(), &parsed_value)) {
            if (error_message != nullptr) {
                *error_message = "params.values." + iterator.key() + " 非法，必须是 0 到 65535 范围内的整数";
            }
            return StatusCode::kInvalidArgument;
        }
        parsed.values[iterator.key()] = static_cast<std::uint16_t>(parsed_value);
    }

    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取通道配置更新请求。
StatusCode extract_channel_config_update_request(
    const nlohmann::json& request,
    ChannelConfigUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少通道更新请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    ChannelConfigUpdateRequest parsed;
    std::uint32_t data_bits = parsed.data_bits;
    std::uint32_t stop_bits = parsed.stop_bits;
    std::uint32_t timeout_ms = 0;
    std::uint32_t retry_count = 0;
    std::uint32_t tcp_port = 502;
    std::uint32_t connect_timeout_ms = 3000;
    std::string parity_text = "none";
    std::string channel_type_text = "modbus_rtu_serial";

    if (!is_ok(require_string_field(*params, "channel_id", &parsed.channel_id, error_message)) ||
        !is_ok(require_string_field(*params, "channel_name", &parsed.channel_name, error_message)) ||
        !is_ok(require_bool_field(*params, "enabled", &parsed.enabled, error_message)) ||
        !is_ok(optional_string_field(*params, "channel_type", &channel_type_text, error_message)) ||
        !is_ok(optional_string_field(*params, "port_name", &parsed.port_name, error_message)) ||
        !is_ok(optional_uint_range_field(*params, "baud_rate", 1, 10000000, &parsed.baud_rate, error_message)) ||
        !is_ok(optional_uint_range_field(*params, "data_bits", 5, 8, &data_bits, error_message)) ||
        !is_ok(optional_string_field(*params, "parity", &parity_text, error_message)) ||
        !is_ok(optional_uint_range_field(*params, "stop_bits", 1, 2, &stop_bits, error_message)) ||
        !is_ok(require_uint_field(*params, "response_timeout_ms", &timeout_ms, error_message)) ||
        !is_ok(require_uint_field(*params, "retry_count", &retry_count, error_message)) ||
        !is_ok(optional_string_field(*params, "tcp_host", &parsed.tcp_host, error_message)) ||
        !is_ok(optional_uint_range_field(*params, "tcp_port", 1, 65535, &tcp_port, error_message)) ||
        !is_ok(optional_uint_range_field(*params, "connect_timeout_ms", 1, 60000, &connect_timeout_ms, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    if (!is_ok(parse_channel_type(channel_type_text, &parsed.channel_type))) {
        if (error_message != nullptr) {
            *error_message = "params.channel_type 非法";
        }
        return StatusCode::kInvalidArgument;
    }

    if (!is_ok(parse_serial_parity(parity_text, &parsed.parity))) {
        if (error_message != nullptr) {
            *error_message = "params.parity 非法";
        }
        return StatusCode::kInvalidArgument;
    }

    parsed.data_bits = static_cast<std::uint8_t>(data_bits);
    parsed.stop_bits = static_cast<std::uint8_t>(stop_bits);
    parsed.response_timeout_ms = timeout_ms;
    parsed.retry_count = retry_count;
    parsed.tcp_port = static_cast<std::uint16_t>(tcp_port);
    parsed.connect_timeout_ms = connect_timeout_ms;

    *result = parsed;
    return StatusCode::kOk;
}

// 提取主站配置更新请求。
StatusCode extract_master_config_update_request(
    const nlohmann::json& request,
    MasterNodeConfigUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少主站配置请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    MasterNodeConfigUpdateRequest parsed;
    std::uint32_t target_address = 0;
    std::uint32_t poll_interval_ms = 0;
    std::uint32_t response_timeout_ms = 0;
    std::uint32_t retry_count = 0;
    std::uint32_t block_start_register = 0;
    std::uint32_t device_count = parsed.device_count;
    std::string protocol_text;

    if (!is_ok(require_string_field(*params, "master_id", &parsed.master_id, error_message)) ||
        !is_ok(require_string_field(*params, "master_name", &parsed.master_name, error_message)) ||
        !is_ok(require_bool_field(*params, "enabled", &parsed.enabled, error_message)) ||
        !is_ok(require_string_field(*params, "protocol", &protocol_text, error_message)) ||
        !is_ok(require_string_field(*params, "channel_id", &parsed.channel_id, error_message)) ||
        !is_ok(require_uint_range_field(*params, "target_address", 1, 247, &target_address, error_message)) ||
        !is_ok(require_uint_field(*params, "poll_interval_ms", &poll_interval_ms, error_message)) ||
        !is_ok(require_uint_field(*params, "response_timeout_ms", &response_timeout_ms, error_message)) ||
        !is_ok(require_uint_field(*params, "retry_count", &retry_count, error_message)) ||
        !is_ok(require_string_field(*params, "remark", &parsed.remark, error_message)) ||
        !is_ok(require_string_field(*params, "device_template", &parsed.device_template, error_message)) ||
        !is_ok(require_uint_range_field(*params, "block_start_register", 0, 65535, &block_start_register, error_message)) ||
        !is_ok(require_uint_range_field(
            *params,
            "device_count",
            1,
            kMaxDevicesPerMaster,
            &device_count,
            error_message))) {
        return StatusCode::kInvalidArgument;
    }

    if (!is_ok(parse_master_protocol(protocol_text, &parsed.protocol))) {
        if (error_message != nullptr) {
            *error_message = "params.protocol 非法";
        }
        return StatusCode::kInvalidArgument;
    }

    parsed.target_address = static_cast<std::uint16_t>(target_address);
    parsed.poll_interval_ms = poll_interval_ms;
    parsed.response_timeout_ms = response_timeout_ms;
    parsed.retry_count = retry_count;
    parsed.block_start_register = static_cast<std::uint16_t>(block_start_register);
    parsed.device_count = device_count;

    *result = parsed;
    return StatusCode::kOk;
}

// 提取设备类型创建请求。
StatusCode extract_device_template_create_request(
    const nlohmann::json& request,
    DeviceTemplateDefinition* result,
    std::string* error_message)
{
    // 校验输出对象和 params 请求体。
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少设备模板创建请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    // 解析设备类型基础标识、地址范围和描述信息。
    DeviceTemplateDefinition parsed;
    std::uint32_t default_start_register = 0;
    std::uint32_t device_address_stride = 0;
    if (!is_ok(require_string_any_field(*params, {"template_id", "id"}, "template_id", &parsed.template_id, error_message)) ||
        !is_ok(require_string_any_field(*params, {"template_name", "name", "display_name"}, "template_name", &parsed.display_name, error_message)) ||
        !is_ok(optional_string_any_field(*params, {"description"}, "description", &parsed.description, error_message)) ||
        !is_ok(optional_uint_range_field(
            *params,
            "default_start_register",
            0,
            std::numeric_limits<std::uint16_t>::max(),
            &default_start_register,
            error_message))) {
        return StatusCode::kInvalidArgument;
    }
    parsed.default_start_register = static_cast<RegisterAddress>(default_start_register);
    if (!is_ok(require_uint_range_field(
            *params,
            "device_address_stride",
            1,
            65536,
            &device_address_stride,
            error_message))) {
        return StatusCode::kInvalidArgument;
    }
    parsed.device_address_stride = device_address_stride;

    // 解析实时展示分组配置。
    if (find_field(*params, "realtime_grouping_enabled") != nullptr &&
        !is_ok(optional_bool_field(
            *params,
            "realtime_grouping_enabled",
            &parsed.realtime_grouping_enabled,
            error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (const auto* groups = find_field(*params, "realtime_groups"); groups != nullptr) {
        if (!groups->is_array()) {
            if (error_message != nullptr) *error_message = "params.realtime_groups 必须是数组";
            return StatusCode::kInvalidArgument;
        }
        parsed.realtime_groups.reserve(groups->size());
        for (std::size_t index = 0; index < groups->size(); ++index) {
            const auto& item = (*groups)[index];
            DeviceTemplateRealtimeGroupDefinition group;
            std::uint32_t sort_order = 0;
            if (!item.is_object() ||
                !is_ok(require_string_any_field(item, {"id", "group_id"}, "realtime_groups.id", &group.group_id, error_message)) ||
                !is_ok(require_string_any_field(item, {"name", "display_name"}, "realtime_groups.name", &group.display_name, error_message)) ||
                !is_ok(require_uint_range_field(
                    item,
                    "order",
                    0,
                    static_cast<std::uint32_t>(std::numeric_limits<int>::max()),
                    &sort_order,
                    error_message))) {
                if (error_message != nullptr && !error_message->empty()) {
                    *error_message = "实时展示分组[" + std::to_string(index + 1) + "]：" + *error_message;
                }
                return StatusCode::kInvalidArgument;
            }
            group.sort_order = static_cast<int>(sort_order);
            parsed.realtime_groups.push_back(std::move(group));
        }
    }

    // 自定义设备类型是第三方只读采集定义，不能通过 IPC 注入 FC10 控制操作。
    if (const auto* commands = find_field(*params, "write_commands"); commands != nullptr) {
        if (!commands->is_array()) {
            if (error_message != nullptr) *error_message = "params.write_commands 必须是数组";
            return StatusCode::kInvalidArgument;
        }
        if (!commands->empty()) {
            if (error_message != nullptr) {
                *error_message = "自定义设备类型仅支持 FC03 / FC04 数据采集，不能配置 write_commands";
            }
            return StatusCode::kInvalidArgument;
        }
    }

    // 解析读取区块并检查区块标识唯一性。
    const auto* read_blocks = find_field(*params, "read_blocks");
    if (read_blocks == nullptr || !read_blocks->is_array() || read_blocks->empty()) {
        if (error_message != nullptr) {
            *error_message = "params.read_blocks 必须是至少包含一个读取区块的数组";
        }
        return StatusCode::kInvalidArgument;
    }
    {
        std::set<std::string> block_keys;
        parsed.read_blocks.reserve(read_blocks->size());
        for (std::size_t index = 0; index < read_blocks->size(); ++index) {
            const auto& item = (*read_blocks)[index];
            if (!item.is_object()) {
                if (error_message != nullptr) {
                    *error_message = "params.read_blocks[" + std::to_string(index) + "] 必须是对象";
                }
                return StatusCode::kInvalidArgument;
            }

            DeviceTemplateReadBlockDefinition block;
            std::uint32_t function_code = 0;
            std::uint32_t start_offset = 0;
            std::uint32_t register_count = 0;
            std::uint32_t sort_order = 0;
            if (!is_ok(require_string_field(item, "block_key", &block.block_key, error_message)) ||
                !is_ok(require_string_field(item, "display_name", &block.display_name, error_message)) ||
                !is_ok(require_uint_range_field(item, "function_code", 3, 4, &function_code, error_message)) ||
                !is_ok(require_uint_range_field(item, "start_offset", 0, 65535, &start_offset, error_message)) ||
                !is_ok(require_uint_range_field(
                    item,
                    "register_count",
                    1,
                    ModbusRtuProtocol::kMaxReadRegisterCount,
                    &register_count,
                    error_message)) ||
                !is_ok(require_uint_range_field(
                    item,
                    "sort_order",
                    0,
                    static_cast<std::uint32_t>(std::numeric_limits<int>::max()),
                    &sort_order,
                    error_message))) {
                if (error_message != nullptr && !error_message->empty()) {
                    *error_message = "读取区块[" + std::to_string(index + 1) + "]：" + *error_message;
                }
                return StatusCode::kInvalidArgument;
            }
            if (block.block_key.empty()) {
                if (error_message != nullptr) *error_message = "读取区块标识不能为空";
                return StatusCode::kInvalidArgument;
            }
            if (!block_keys.insert(block.block_key).second) {
                if (error_message != nullptr) *error_message = "读取区块标识重复：" + block.block_key;
                return StatusCode::kInvalidArgument;
            }
            block.function_code = function_code;
            block.start_offset = start_offset;
            block.register_count = register_count;
            block.sort_order = static_cast<int>(sort_order);
            parsed.read_blocks.push_back(std::move(block));
        }
    }

    // 解析字段、位序号、枚举项和无效值规则。
    const auto* fields = find_field(*params, "fields");
    if (fields == nullptr || !fields->is_array()) {
        if (error_message != nullptr) {
            *error_message = "缺少或非法的 params.fields";
        }
        return StatusCode::kInvalidArgument;
    }

    parsed.fields.reserve(fields->size());
    for (std::size_t index = 0; index < fields->size(); ++index) {
        const auto& item = (*fields)[index];
        if (!item.is_object()) {
            if (error_message != nullptr) {
                *error_message = "params.fields[" + std::to_string(index) + "] 必须是对象";
            }
            return StatusCode::kInvalidArgument;
        }

        DeviceTemplateFieldDefinition field;
        field.parser_id = "scaled_uint16";
        field.display_order = static_cast<std::uint32_t>(index + 1);
        if (find_field(item, "invalid_threshold") != nullptr ||
            find_field(item, "invalid_message") != nullptr ||
            find_field(item, "raw_greater_than") != nullptr) {
            if (error_message != nullptr) {
                *error_message =
                    "params.fields[" + std::to_string(index) +
                    "] 包含已废弃的字段无效规则，请使用 invalid_rule_type/value/min/max";
            }
            return StatusCode::kInvalidArgument;
        }
        if (!is_ok(require_string_any_field(item, {"key", "field_key"}, "fields.key", &field.field_key, error_message)) ||
            !is_ok(require_string_any_field(item, {"name", "field_name", "display_name"}, "fields.name", &field.display_name, error_message)) ||
            !is_ok(optional_string_any_field(item, {"unit"}, "fields.unit", &field.unit, error_message)) ||
            !is_ok(require_string_any_field(item, {"data_type"}, "fields.data_type", &field.data_type, error_message)) ||
            !is_ok(optional_string_any_field(item, {"parser_id"}, "fields.parser_id", &field.parser_id, error_message)) ||
            !is_ok(require_string_any_field(
                item, {"read_block_key"}, "fields.read_block_key", &field.read_block_key, error_message)) ||
            !is_ok(require_uint16_any_field(item, {"register_offset"}, "fields.register_offset", &field.register_offset, error_message)) ||
            !is_ok(require_uint16_any_field(item, {"register_count"}, "fields.register_count", &field.register_count, error_message)) ||
            !is_ok(optional_string_any_field(item, {"byte_order"}, "fields.byte_order", &field.byte_order, error_message)) ||
            !is_ok(optional_string_any_field(item, {"word_order"}, "fields.word_order", &field.word_order, error_message)) ||
            !is_ok(require_double_any_field(item, {"scale"}, "fields.scale", &field.scale, error_message)) ||
            !is_ok(require_double_any_field(item, {"value_offset", "offset"}, "fields.offset", &field.value_offset, error_message)) ||
            !is_ok(require_uint32_any_field(item, {"precision"}, "fields.precision", &field.precision, error_message)) ||
            !is_ok(require_bool_any_field(item, {"summary"}, "fields.summary", &field.summary, error_message)) ||
            !is_ok(require_bool_any_field(item, {"show_in_realtime"}, "fields.show_in_realtime", &field.show_in_realtime, error_message)) ||
            !is_ok(require_bool_any_field(item, {"history_enabled"}, "fields.history_enabled", &field.history_enabled, error_message)) ||
            !is_ok(require_uint32_any_field(item, {"display_order"}, "fields.display_order", &field.display_order, error_message)) ||
            !is_ok(optional_string_any_field(item, {"invalid_rule_type"}, "fields.invalid_rule_type", &field.invalid_rule_type, error_message)) ||
            !is_ok(optional_double_any_field(item, {"invalid_rule_value"}, "fields.invalid_rule_value", &field.invalid_rule_value, error_message)) ||
            !is_ok(optional_double_any_field(item, {"invalid_rule_min"}, "fields.invalid_rule_min", &field.invalid_rule_min, error_message)) ||
            !is_ok(optional_double_any_field(item, {"invalid_rule_max"}, "fields.invalid_rule_max", &field.invalid_rule_max, error_message))) {
            return StatusCode::kInvalidArgument;
        }
        if (!is_ok(optional_string_any_field(
                item,
                {"realtime_group_id"},
                "fields.realtime_group_id",
                &field.realtime_group_id,
                error_message))) {
            return StatusCode::kInvalidArgument;
        }
        if (const auto* bit_index = find_field(item, "bit_index"); bit_index != nullptr) {
            std::int64_t parsed_bit_index = -1;
            if (!number_as_int64(*bit_index, &parsed_bit_index) ||
                parsed_bit_index < -1 || parsed_bit_index > 15) {
                if (error_message != nullptr) {
                    *error_message = "params.fields[" + std::to_string(index) +
                                     "].bit_index 必须为 -1 或 0～15 的整数";
                }
                return StatusCode::kInvalidArgument;
            }
            field.bit_index = static_cast<int>(parsed_bit_index);
        }
        if (const auto* enum_items = find_field(item, "enum_items"); enum_items != nullptr) {
            if (!enum_items->is_array()) {
                if (error_message != nullptr) {
                    *error_message = "params.fields[" + std::to_string(index) + "].enum_items 必须是数组";
                }
                return StatusCode::kInvalidArgument;
            }
            field.enum_items.reserve(enum_items->size());
            for (std::size_t enum_index = 0; enum_index < enum_items->size(); ++enum_index) {
                const auto& enum_item = (*enum_items)[enum_index];
                const auto* value = find_field(enum_item, "value");
                const auto* label = find_field(enum_item, "label");
                const auto* sort_order = find_field(enum_item, "sort_order");
                std::int64_t parsed_value = 0;
                std::int64_t parsed_order = static_cast<std::int64_t>(enum_index);
                if (!enum_item.is_object() || value == nullptr || label == nullptr ||
                    !number_as_int64(*value, &parsed_value) || !label->is_string() ||
                    (sort_order != nullptr && !number_as_int64(*sort_order, &parsed_order)) ||
                    parsed_order < std::numeric_limits<int>::min() ||
                    parsed_order > std::numeric_limits<int>::max()) {
                    if (error_message != nullptr) {
                        *error_message = "params.fields[" + std::to_string(index) + "].enum_items[" +
                                         std::to_string(enum_index) + "] 格式无效";
                    }
                    return StatusCode::kInvalidArgument;
                }
                field.enum_items.push_back({
                    parsed_value,
                    label->get<std::string>(),
                    static_cast<int>(parsed_order),
                });
            }
        }
        // IPC 只解析 JSON 类型、数值表示和字段形状；无效值规则、bit/枚举
        // 兼容性等设备模板业务不变量由 DeviceTemplateStore 的领域校验统一负责。
        parsed.fields.push_back(std::move(field));
    }

    // 返回完成基础解析的定义，完整模型规则由服务层继续校验。
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取设备类型更新请求。
StatusCode extract_device_template_update_request(
    const nlohmann::json& request,
    DeviceTemplateDefinition* result,
    std::string* error_message)
{
    const auto status = extract_device_template_create_request(request, result, error_message);
    if (!is_ok(status) && error_message != nullptr && error_message->empty()) {
        *error_message = "设备模板更新请求无效";
    }
    return status;
}

// 提取设备类型删除请求。
StatusCode extract_device_template_delete_request(
    const nlohmann::json& request,
    std::string* template_id,
    std::string* error_message)
{
    if (template_id == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少设备模板删除请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(require_string_any_field(*params, {"template_id", "id"}, "template_id", template_id, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 从 IPC 请求中提取系统设置更新参数。
StatusCode extract_system_settings_update_request(
    const nlohmann::json& request,
    SystemSettingsUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少系统设置请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    SystemSettingsUpdateRequest parsed;
    if (!is_ok(require_string_field(*params, "device_name", &parsed.device_name, error_message)) ||
        !is_ok(require_string_field(*params, "site_location", &parsed.site_location, error_message)) ||
        !is_ok(require_string_field(*params, "display_name", &parsed.display_name, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    *result = parsed;
    return StatusCode::kOk;
}

// 从 IPC 请求中提取网络设置更新参数。
StatusCode extract_network_settings_update_request(
    const nlohmann::json& request,
    NetworkSettingsUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少网络配置请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    NetworkSettingsUpdateRequest parsed;
    if (!is_ok(require_string_field(*params, "mode", &parsed.mode, error_message)) ||
        !is_ok(require_string_field(*params, "interface_name", &parsed.interface_name, error_message)) ||
        !is_ok(require_string_field(*params, "ip_address", &parsed.ip_address, error_message)) ||
        !is_ok(require_string_field(*params, "netmask", &parsed.netmask, error_message)) ||
        !is_ok(require_string_field(*params, "gateway", &parsed.gateway, error_message)) ||
        !is_ok(require_string_array_field(*params, "dns_servers", &parsed.dns_servers, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 从 IPC 请求中提取时间设置更新参数。
StatusCode extract_time_settings_update_request(
    const nlohmann::json& request,
    TimeSettingsUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) *error_message = "缺少时间设置请求输出参数";
        return StatusCode::kInvalidArgument;
    }
    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 params 对象";
        return StatusCode::kInvalidArgument;
    }
    TimeSettingsUpdateRequest parsed;
    if (!is_ok(require_string_field(*params, "timezone", &parsed.timezone, error_message)) ||
        !is_ok(require_bool_field(*params, "ntp_enabled", &parsed.ntp_enabled, error_message)) ||
        !is_ok(require_string_field(*params, "ntp_primary", &parsed.ntp_primary, error_message)) ||
        !is_ok(require_string_field(*params, "ntp_secondary", &parsed.ntp_secondary, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取手动设置系统时间的请求。
StatusCode extract_manual_time_set_request(
    const nlohmann::json& request,
    ManualTimeSetRequest* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    const auto* params = request_params_object(request);
    if (params == nullptr || !is_ok(optional_uint64_field(*params, "epoch_ms", &result->epoch_ms, error_message)) || result->epoch_ms == 0) {
        if (error_message != nullptr && error_message->empty()) *error_message = "缺少或非法的 params.epoch_ms";
        return StatusCode::kInvalidArgument;
    }
    const auto source = params->find("source");
    if (source != params->end()) {
        if (!source->is_string()) {
            if (error_message != nullptr) *error_message = "params.source 必须是字符串";
            return StatusCode::kInvalidArgument;
        }
        result->source = source->get<std::string>();
    }
    return StatusCode::kOk;
}

// 从 IPC 请求中提取 MQTT 设置更新参数。
StatusCode extract_mqtt_settings_update_request(
    const nlohmann::json& request,
    MqttSettingsUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 MQTT 配置请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    MqttSettingsUpdateRequest parsed;
    std::uint32_t broker_port = parsed.broker_port;
    std::uint32_t publish_interval_seconds = parsed.publish_interval_seconds;
    std::uint32_t keep_alive_seconds = parsed.keep_alive_seconds;
    std::uint32_t qos = static_cast<std::uint32_t>(parsed.qos);
    if (!is_ok(require_bool_field(*params, "enabled", &parsed.enabled, error_message)) ||
        !is_ok(require_string_field(*params, "broker_host", &parsed.broker_host, error_message)) ||
        !is_ok(require_uint_range_field(*params, "broker_port", 1, 65535, &broker_port, error_message)) ||
        !is_ok(require_string_field(*params, "client_id", &parsed.client_id, error_message)) ||
        !is_ok(require_string_field(*params, "node_id", &parsed.node_id, error_message)) ||
        !is_ok(require_string_field(*params, "username", &parsed.username, error_message)) ||
        !is_ok(optional_string_field(*params, "password", &parsed.password, error_message)) ||
        !is_ok(require_string_field(*params, "topic_prefix", &parsed.topic_prefix, error_message)) ||
        !is_ok(require_uint_range_field(
            *params,
            "publish_interval_seconds",
            1,
            3600,
            &publish_interval_seconds,
            error_message)) ||
        !is_ok(require_uint_range_field(*params, "qos", 0, 1, &qos, error_message)) ||
        !is_ok(require_bool_field(*params, "retain_status", &parsed.retain_status, error_message)) ||
        !is_ok(require_uint_range_field(*params, "keep_alive_seconds", 10, 300, &keep_alive_seconds, error_message)) ||
        !is_ok(require_bool_field(*params, "tls_enabled", &parsed.tls_enabled, error_message)) ||
        !is_ok(require_string_field(*params, "tls_ca_file", &parsed.tls_ca_file, error_message)) ||
        !is_ok(require_string_field(*params, "tls_client_cert_file", &parsed.tls_client_cert_file, error_message)) ||
        !is_ok(require_string_field(*params, "tls_client_key_file", &parsed.tls_client_key_file, error_message)) ||
        !is_ok(require_bool_field(*params, "tls_insecure", &parsed.tls_insecure, error_message)) ||
        !is_ok(optional_bool_field(*params, "clear_password", &parsed.clear_password, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    parsed.broker_port = static_cast<std::uint16_t>(broker_port);
    parsed.publish_interval_seconds = publish_interval_seconds;
    parsed.keep_alive_seconds = keep_alive_seconds;
    parsed.qos = static_cast<int>(qos);
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取配置导入请求。
StatusCode extract_config_import_request(
    const nlohmann::json& request,
    ConfigImportRequest* result,
    std::string* error_message)
{
    // 校验输出对象、params 请求体、配置格式和版本。
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少配置导入请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    ConfigImportRequest parsed;
    std::uint32_t version = 0;
    if (!is_ok(require_string_field(*params, "format", &parsed.bundle.format, error_message)) ||
        !is_ok(require_uint_field(*params, "version", &version, error_message)) ||
        !is_ok(require_uint64_field(*params, "exported_at_ms", &parsed.bundle.exported_at_ms, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    parsed.bundle.version = version;
    if (version != 4) {
        if (error_message != nullptr) *error_message = "配置文件版本不受支持，仅支持当前 version 4";
        return StatusCode::kInvalidArgument;
    }
    // 获取第四版配置包要求的全部顶层对象和数组。
    const nlohmann::json* system_settings = nullptr;
    const nlohmann::json* time_settings = nullptr;
    const nlohmann::json* network_settings = nullptr;
    const nlohmann::json* mqtt_settings = nullptr;
    const nlohmann::json* custom_device_types = nullptr;
    const nlohmann::json* channels = nullptr;
    const nlohmann::json* masters = nullptr;
    const nlohmann::json* alarm_rules = nullptr;
    if (!is_ok(require_object_field(*params, "system_settings", &system_settings, error_message)) ||
        !is_ok(require_object_field(*params, "time_settings", &time_settings, error_message)) ||
        !is_ok(require_object_field(*params, "network_settings", &network_settings, error_message)) ||
        !is_ok(require_object_field(*params, "mqtt_settings", &mqtt_settings, error_message)) ||
        !is_ok(require_array_field(*params, "custom_device_types", &custom_device_types, error_message)) ||
        !is_ok(require_array_field(*params, "channels", &channels, error_message)) ||
        !is_ok(require_array_field(*params, "masters", &masters, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(parse_system_settings_object(*system_settings, &parsed.bundle.system_settings, error_message)) ||
        !is_ok(parse_network_settings_object(*network_settings, &parsed.bundle.network_settings, error_message)) ||
        !is_ok(parse_mqtt_settings_object(*mqtt_settings, &parsed.bundle.mqtt_settings, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(parse_time_settings_object(*time_settings, &parsed.bundle.time_settings, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    // 逐项解析自定义设备类型。
    parsed.bundle.custom_device_types.reserve(custom_device_types->size());
    for (std::size_t index = 0; index < custom_device_types->size(); ++index) {
        const auto& item = (*custom_device_types)[index];
        if (!item.is_object()) {
            if (error_message != nullptr) {
                *error_message = "params.custom_device_types[" + std::to_string(index) + "] 必须是对象";
            }
            return StatusCode::kInvalidArgument;
        }
        DeviceTemplateDefinition device_template;
        const nlohmann::json template_request{{"params", item}};
        std::string template_error;
        if (!is_ok(extract_device_template_create_request(
                template_request, &device_template, &template_error))) {
            if (error_message != nullptr) {
                *error_message = "自定义设备类型[" + std::to_string(index + 1) + "]无效：" + template_error;
            }
            return StatusCode::kInvalidArgument;
        }
        device_template.builtin = false;
        parsed.bundle.custom_device_types.push_back(std::move(device_template));
    }

    // 解析通道和主站配置。
    parsed.bundle.channels.reserve(channels->size());
    for (std::size_t index = 0; index < channels->size(); ++index) {
        const auto& item = (*channels)[index];
        if (!item.is_object()) {
            if (error_message != nullptr) {
                *error_message = "params.channels[" + std::to_string(index) + "] 必须是对象";
            }
            return StatusCode::kInvalidArgument;
        }
        ChannelConfig channel;
        if (!is_ok(parse_channel_config_object(item, &channel, error_message))) {
            return StatusCode::kInvalidArgument;
        }
        parsed.bundle.channels.push_back(std::move(channel));
    }

    parsed.bundle.masters.reserve(masters->size());
    for (std::size_t index = 0; index < masters->size(); ++index) {
        const auto& item = (*masters)[index];
        if (!item.is_object()) {
            if (error_message != nullptr) {
                *error_message = "params.masters[" + std::to_string(index) + "] 必须是对象";
            }
            return StatusCode::kInvalidArgument;
        }
        MasterNodeConfig master;
        if (!is_ok(parse_master_config_object(item, &master, error_message))) {
            return StatusCode::kInvalidArgument;
        }
        parsed.bundle.masters.push_back(std::move(master));
    }

    // 解析报警规则清单。
    if (!is_ok(require_bool_field(*params, "alarm_rules_included", &parsed.bundle.alarm_rules_included, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(require_array_field(*params, "alarm_rules", &alarm_rules, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    parsed.bundle.alarm_rules.reserve(alarm_rules->size());
    for (std::size_t index = 0; index < alarm_rules->size(); ++index) {
        const auto& item = (*alarm_rules)[index];
        if (!item.is_object()) {
            if (error_message != nullptr) {
                *error_message = "params.alarm_rules[" + std::to_string(index) + "] 必须是对象";
            }
            return StatusCode::kInvalidArgument;
        }
        AlarmRule rule;
        if (!is_ok(parse_alarm_rule_object(item, &rule, error_message))) {
            return StatusCode::kInvalidArgument;
        }
        parsed.bundle.alarm_rules.push_back(std::move(rule));
    }

    // 解析 Modbus 服务端设置和寄存器映射，并执行整体映射校验。
    {
        const nlohmann::json* modbus_settings = nullptr;
        const nlohmann::json* modbus_mappings = nullptr;
        if (!is_ok(require_object_field(*params, "modbus_server_settings", &modbus_settings, error_message)) ||
            !is_ok(require_array_field(*params, "modbus_register_mappings", &modbus_mappings, error_message)) ||
            !is_ok(parse_modbus_server_settings_object(
                *modbus_settings, &parsed.bundle.modbus_server_settings, error_message))) {
            return StatusCode::kInvalidArgument;
        }
        parsed.bundle.modbus_register_mappings.reserve(modbus_mappings->size());
        for (std::size_t index = 0; index < modbus_mappings->size(); ++index) {
            const auto& item = (*modbus_mappings)[index];
            if (!item.is_object()) {
                if (error_message != nullptr) {
                    *error_message = "params.modbus_register_mappings[" + std::to_string(index) + "] 必须是对象";
                }
                return StatusCode::kInvalidArgument;
            }
            ModbusRegisterMapping mapping;
            if (!is_ok(parse_modbus_mapping_object(item, &mapping, error_message))) {
                return StatusCode::kInvalidArgument;
            }
            parsed.bundle.modbus_register_mappings.push_back(std::move(mapping));
        }
        if (!is_ok(validate_modbus_register_mappings(parsed.bundle.modbus_register_mappings, error_message))) {
            return StatusCode::kInvalidArgument;
        }
    }

    // 返回完成结构解析的配置包，跨对象关系由导入服务继续校验。
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取 Web 登录请求。
StatusCode extract_web_login_request(
    const nlohmann::json& request,
    WebLoginRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 登录请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    WebLoginRequest parsed;
    if (!is_ok(require_string_field(*params, "username", &parsed.username, error_message)) ||
        !is_ok(require_string_field(*params, "password", &parsed.password, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取 Web 密码修改请求。
StatusCode extract_web_password_change_request(
    const nlohmann::json& request,
    WebPasswordChangeRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Web 密码修改请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    WebPasswordChangeRequest parsed;
    if (!is_ok(require_string_field(*params, "username", &parsed.username, error_message)) ||
        !is_ok(require_string_field(*params, "current_password", &parsed.current_password, error_message)) ||
        !is_ok(require_string_field(*params, "new_password", &parsed.new_password, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取 Web 查看员密码设置请求。
StatusCode extract_web_viewer_password_set_request(
    const nlohmann::json& request,
    WebViewerPasswordSetRequest* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少只读用户密码设置请求输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto* params = request_params_object(request);
    if (params == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 params 对象";
        }
        return StatusCode::kInvalidArgument;
    }

    WebViewerPasswordSetRequest parsed;
    if (!is_ok(require_string_field(*params, "admin_password", &parsed.admin_password, error_message)) ||
        !is_ok(require_string_field(*params, "new_user_password", &parsed.new_user_password, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取Web用户创建。
StatusCode extract_web_user_create_request(
    const nlohmann::json& request,
    WebUserCreateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    const auto* params = request_params_object(request);
    if (params == nullptr) { if (error_message) *error_message = "缺少 params 对象"; return StatusCode::kInvalidArgument; }
    WebUserCreateRequest parsed;
    if (!is_ok(require_string_field(*params, "username", &parsed.username, error_message)) ||
        !is_ok(require_string_field(*params, "display_name", &parsed.display_name, error_message)) ||
        !is_ok(require_string_field(*params, "role", &parsed.role, error_message)) ||
        !is_ok(require_string_field(*params, "password", &parsed.password, error_message)) ||
        !is_ok(require_bool_field(*params, "enabled", &parsed.enabled, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取Web用户更新。
StatusCode extract_web_user_update_request(
    const nlohmann::json& request,
    WebUserUpdateRequest* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    const auto* params = request_params_object(request);
    if (params == nullptr) { if (error_message) *error_message = "缺少 params 对象"; return StatusCode::kInvalidArgument; }
    WebUserUpdateRequest parsed;
    if (!is_ok(require_string_field(*params, "username", &parsed.username, error_message)) ||
        !is_ok(require_string_field(*params, "display_name", &parsed.display_name, error_message)) ||
        !is_ok(require_string_field(*params, "role", &parsed.role, error_message)) ||
        !is_ok(require_bool_field(*params, "enabled", &parsed.enabled, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取Web用户密码重置。
StatusCode extract_web_user_password_reset_request(
    const nlohmann::json& request,
    WebUserPasswordResetRequest* result,
    std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    const auto* params = request_params_object(request);
    if (params == nullptr) { if (error_message) *error_message = "缺少 params 对象"; return StatusCode::kInvalidArgument; }
    WebUserPasswordResetRequest parsed;
    if (!is_ok(require_string_field(*params, "username", &parsed.username, error_message)) ||
        !is_ok(require_string_field(*params, "new_password", &parsed.new_password, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    *result = std::move(parsed);
    return StatusCode::kOk;
}

// 提取告警规则键请求。
StatusCode extract_alarm_rule_key_request(const nlohmann::json& request, DeviceId* device_id, std::string* point_key, std::string* error_message)
{
    if (device_id == nullptr || point_key == nullptr) return StatusCode::kInvalidArgument;
    const auto* params = request_params_object(request);
    if (params == nullptr) { if (error_message) *error_message = "缺少 params 对象"; return StatusCode::kInvalidArgument; }
    auto status = require_string_field(*params, "device_id", device_id, error_message);
    if (!is_ok(status)) return status;
    status = require_string_field(*params, "point_key", point_key, error_message);
    if (!is_ok(status)) return status;
    if (device_id->empty() || point_key->empty()) { if (error_message) *error_message = "params.device_id 和 params.point_key 不能为空"; return StatusCode::kInvalidArgument; }
    return StatusCode::kOk;
}

// 提取可选的告警设备标识。
StatusCode extract_optional_alarm_device_id(const nlohmann::json& request, std::optional<DeviceId>* device_id, std::string* error_message)
{
    if (device_id == nullptr) return StatusCode::kInvalidArgument;
    device_id->reset();
    const auto* params = request_params_object(request);
    if (params == nullptr) return StatusCode::kOk;
    const auto* value = find_field(*params, "device_id");
    if (value == nullptr) return StatusCode::kOk;
    if (!value->is_string() || value->get<std::string>().empty()) { if (error_message) *error_message = "params.device_id 必须是非空字符串"; return StatusCode::kInvalidArgument; }
    *device_id = value->get<std::string>();
    return StatusCode::kOk;
}

// 提取告警规则新增或更新请求。
StatusCode extract_alarm_rule_upsert_request(const nlohmann::json& request, AlarmRule* result, std::string* error_message)
{
    if (result == nullptr) return StatusCode::kInvalidArgument;
    const auto* params = request_params_object(request);
    if (params == nullptr) { if (error_message) *error_message = "缺少 params 对象"; return StatusCode::kInvalidArgument; }
    AlarmRule rule;
    auto status = require_string_field(*params, "device_id", &rule.device_id, error_message); if (!is_ok(status)) return status;
    status = require_string_field(*params, "point_key", &rule.point_key, error_message); if (!is_ok(status)) return status;
    status = require_bool_field(*params, "enabled", &rule.enabled, error_message); if (!is_ok(status)) return status;
    status = require_bool_field(*params, "high_enabled", &rule.high_enabled, error_message); if (!is_ok(status)) return status;
    status = require_bool_field(*params, "low_enabled", &rule.low_enabled, error_message); if (!is_ok(status)) return status;
    status = require_string_field(*params, "level", &rule.level, error_message); if (!is_ok(status)) return status;
    // IPC 只负责 JSON 类型和 uint32 边界；连续次数的 1-100 业务规则由领域
    // validator 统一处理，避免 Web、IPC 和 Store 各自维护一份阈值。
    status = require_uint_field(*params, "trigger_count", &rule.trigger_count, error_message); if (!is_ok(status)) return status;
    status = require_uint_field(*params, "recovery_count", &rule.recovery_count, error_message); if (!is_ok(status)) return status;
    const auto parse_double = [&](const char* name, double* output) {
        const auto* value = find_field(*params, name);
        if (value == nullptr || !number_as_double(*value, output)) { if (error_message) *error_message = std::string("缺少或非法的 params.") + name; return false; }
        return true;
    };
    if (!parse_double("high_threshold", &rule.high_threshold) || !parse_double("low_threshold", &rule.low_threshold) || !parse_double("hysteresis", &rule.hysteresis)) return StatusCode::kInvalidArgument;
    if (rule.device_id.empty() || rule.point_key.empty()) { if (error_message) *error_message = "device_id 和 point_key 不能为空"; return StatusCode::kInvalidArgument; }
    *result = std::move(rule);
    return StatusCode::kOk;
}

}  // namespace edge_controller::ipc_protocol
