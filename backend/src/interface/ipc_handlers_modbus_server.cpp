// Modbus 北向配置管理 IPC：只负责宽类型解析和领域入口分派，Web 权限由 Go 层处理。
#include "interface/ipc_handlers.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "interface/ipc_json.h"
#include "service/backend_service.h"

namespace edge_controller::ipc_handlers {
namespace {

// 读取并要求请求参数为 JSON 对象。
StatusCode require_object_params(const nlohmann::json& root, const nlohmann::json** params, std::string* error)
{
    *params = request_params(root);
    if (*params == nullptr) {
        if (error != nullptr) *error = "缺少 params 对象";
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 读取并校验必填字符串参数。
StatusCode require_string(const nlohmann::json& value, const char* name, std::string* output, std::string* error)
{
    const auto* field = json_field(value, name);
    if (field == nullptr || !field->is_string()) {
        if (error != nullptr) *error = std::string("缺少或非法字符串字段：") + name;
        return StatusCode::kInvalidArgument;
    }
    *output = field->get<std::string>();
    return StatusCode::kOk;
}

// 读取并校验必填布尔参数。
StatusCode require_bool(const nlohmann::json& value, const char* name, bool* output, std::string* error)
{
    const auto* field = json_field(value, name);
    if (field == nullptr || !field->is_boolean()) {
        if (error != nullptr) *error = std::string("缺少或非法布尔字段：") + name;
        return StatusCode::kInvalidArgument;
    }
    *output = field->get<bool>();
    return StatusCode::kOk;
}

// 读取并校验必填无符号整数参数。
StatusCode require_uint(
    const nlohmann::json& value,
    const char* name,
    std::uint64_t maximum,
    std::uint64_t* output,
    std::string* error)
{
    const auto* field = json_field(value, name);
    if (field == nullptr || (!field->is_number_integer() && !field->is_number_unsigned())) {
        if (error != nullptr) *error = std::string("缺少或非法整数字段：") + name;
        return StatusCode::kInvalidArgument;
    }
    if (field->is_number_unsigned()) {
        const auto parsed = field->get<std::uint64_t>();
        if (parsed > maximum) {
            if (error != nullptr) *error = std::string(name) + " 超出允许范围 0～" + std::to_string(maximum);
            return StatusCode::kInvalidArgument;
        }
        *output = parsed;
    } else {
        const auto parsed = field->get<std::int64_t>();
        if (parsed < 0 || static_cast<std::uint64_t>(parsed) > maximum) {
            if (error != nullptr) *error = std::string(name) + " 超出允许范围 0～" + std::to_string(maximum);
            return StatusCode::kInvalidArgument;
        }
        *output = static_cast<std::uint64_t>(parsed);
    }
    return StatusCode::kOk;
}

// 读取并校验有限浮点数参数。
StatusCode require_finite_double(
    const nlohmann::json& value,
    const char* name,
    double* output,
    std::string* error)
{
    const auto* field = json_field(value, name);
    if (field == nullptr || !field->is_number()) {
        if (error != nullptr) *error = std::string("缺少或非法数值字段：") + name;
        return StatusCode::kInvalidArgument;
    }
    const auto parsed = field->get<double>();
    if (!std::isfinite(parsed)) {
        if (error != nullptr) *error = std::string(name) + " 必须是有限数";
        return StatusCode::kInvalidArgument;
    }
    *output = parsed;
    return StatusCode::kOk;
}

// 解析 Modbus 服务设置请求。
StatusCode parse_settings(const nlohmann::json& value, ModbusServerSettings* settings, std::string* error)
{
    std::uint64_t port = 0, unit = 0, clients = 0, timeout = 0, maximum_read = 0;
    auto status = require_bool(value, "enabled", &settings->enabled, error);
    if (is_ok(status)) status = require_string(value, "listen_address", &settings->listen_address, error);
    if (is_ok(status)) status = require_uint(value, "listen_port", 65535, &port, error);
    if (is_ok(status)) status = require_uint(value, "unit_id", 255, &unit, error);
    if (is_ok(status)) status = require_bool(value, "strict_unit_id", &settings->strict_unit_id, error);
    if (is_ok(status)) status = require_uint(value, "max_clients", 64, &clients, error);
    if (is_ok(status)) status = require_uint(value, "idle_timeout_seconds", 3600, &timeout, error);
    if (is_ok(status)) status = require_uint(value, "max_read_registers", 125, &maximum_read, error);
    if (!is_ok(status)) return status;
    settings->listen_port = static_cast<std::uint16_t>(port);
    settings->unit_id = static_cast<std::uint8_t>(unit);
    settings->max_clients = static_cast<std::uint32_t>(clients);
    settings->idle_timeout_seconds = static_cast<std::uint32_t>(timeout);
    settings->max_read_registers = static_cast<std::uint16_t>(maximum_read);
    normalize_modbus_server_settings(settings);
    return validate_modbus_server_settings(*settings, "modbus_server_settings", error);
}

// 解析 Modbus 寄存器映射请求。
StatusCode parse_mapping(const nlohmann::json& value, ModbusRegisterMapping* mapping, std::string* error)
{
    std::string data_type, byte_order, word_order;
    std::uint64_t start = 0, quality = 0;
    auto status = require_string(value, "device_id", &mapping->device_id, error);
    if (is_ok(status)) status = require_string(value, "point_key", &mapping->point_key, error);
    if (is_ok(status)) status = require_string(value, "device_name_snapshot", &mapping->device_name_snapshot, error);
    if (is_ok(status)) status = require_string(value, "point_name_snapshot", &mapping->point_name_snapshot, error);
    if (is_ok(status)) status = require_uint(value, "start_address", 65535, &start, error);
    if (is_ok(status)) status = require_string(value, "data_type", &data_type, error);
    if (is_ok(status)) status = require_finite_double(value, "value_multiplier", &mapping->value_multiplier, error);
    if (is_ok(status)) status = require_finite_double(value, "value_offset", &mapping->value_offset, error);
    if (is_ok(status)) status = require_string(value, "byte_order", &byte_order, error);
    if (is_ok(status)) status = require_string(value, "word_order", &word_order, error);
    if (is_ok(status)) status = require_uint(value, "quality_address", 65535, &quality, error);
    if (is_ok(status)) status = require_bool(value, "enabled", &mapping->enabled, error);
    if (!is_ok(status)) return status;
    if (!parse_modbus_register_data_type(data_type, &mapping->data_type)) {
        if (error != nullptr) *error = "data_type 不受支持：" + data_type;
        return StatusCode::kInvalidArgument;
    }
    if (!parse_modbus_byte_order(byte_order, &mapping->byte_order)) {
        if (error != nullptr) *error = "byte_order 不受支持：" + byte_order;
        return StatusCode::kInvalidArgument;
    }
    if (!parse_modbus_word_order(word_order, &mapping->word_order)) {
        if (error != nullptr) *error = "word_order 不受支持：" + word_order;
        return StatusCode::kInvalidArgument;
    }
    mapping->start_address = static_cast<RegisterAddress>(start);
    mapping->quality_address = static_cast<RegisterAddress>(quality);
    normalize_modbus_register_mapping(mapping);
    return StatusCode::kOk;
}

// 构造 Modbus 服务接口错误响应。
void error_response(
    const nlohmann::json& id,
    StatusCode status,
    const std::string& message,
    std::string* response)
{
    *response = ipc_protocol::build_error_response(id, status_code_string(status), message);
}

}  // namespace

// 分派并处理 Modbus 服务 IPC 请求。
bool handle_modbus_server_request(const IpcHandlerContext& context, std::string* response)
{
    if (context.method == "get_modbus_server_page_snapshot") {
        ModbusServerPageSnapshot snapshot;
        std::string error;
        const auto status = context.backend_service->get_modbus_server_page_snapshot(&snapshot, &error);
        if (!is_ok(status)) error_response(context.id_json, status, error, response);
        else *response = ipc_protocol::build_success_response(context.id_json, ipc_json::to_json(snapshot));
        return true;
    }
    if (context.method == "get_modbus_server_runtime_status") {
        *response = ipc_protocol::build_success_response(
            context.id_json, ipc_json::to_json(context.backend_service->get_modbus_server_runtime_status()));
        return true;
    }
    if (context.method == "list_modbus_exportable_points") {
        std::vector<ModbusExportablePoint> points;
        std::string error;
        const auto status = context.backend_service->list_modbus_exportable_points(&points, &error);
        if (!is_ok(status)) error_response(context.id_json, status, error, response);
        else *response = ipc_protocol::build_success_response(context.id_json, ipc_json::to_json_array(points));
        return true;
    }
    if (context.method == "list_modbus_register_mappings") {
        std::vector<ModbusRegisterMapping> mappings;
        std::string error;
        const auto status = context.backend_service->list_modbus_register_mappings(&mappings, &error);
        if (!is_ok(status)) error_response(context.id_json, status, error, response);
        else *response = ipc_protocol::build_success_response(context.id_json, ipc_json::to_json_array(mappings));
        return true;
    }
    if (context.method == "update_modbus_server_settings") {
        const nlohmann::json* params = nullptr;
        std::string error;
        auto status = require_object_params(context.root, &params, &error);
        ModbusServerSettings settings;
        if (is_ok(status)) status = parse_settings(*params, &settings, &error);
        if (is_ok(status)) status = context.backend_service->apply_modbus_server_settings(settings, &error);
        if (!is_ok(status)) error_response(context.id_json, status, error, response);
        else *response = ipc_protocol::build_success_response(context.id_json, ipc_json::to_json(settings));
        return true;
    }
    if (context.method == "create_modbus_register_mapping" ||
        context.method == "update_modbus_register_mapping") {
        const nlohmann::json* params = nullptr;
        std::string error;
        auto status = require_object_params(context.root, &params, &error);
        ModbusRegisterMapping request;
        if (is_ok(status)) status = parse_mapping(*params, &request, &error);
        ModbusRegisterMapping result;
        if (is_ok(status) && context.method == "create_modbus_register_mapping") {
            status = context.backend_service->create_modbus_register_mapping(request, &result, &error);
        } else if (is_ok(status)) {
            std::string mapping_id;
            status = require_string(*params, "mapping_id", &mapping_id, &error);
            if (is_ok(status)) status = context.backend_service->update_modbus_register_mapping(
                mapping_id, request, &result, &error);
        }
        if (!is_ok(status)) error_response(context.id_json, status, error, response);
        else *response = ipc_protocol::build_success_response(context.id_json, ipc_json::to_json(result));
        return true;
    }
    if (context.method == "delete_modbus_register_mapping") {
        const nlohmann::json* params = nullptr;
        std::string mapping_id, error;
        auto status = require_object_params(context.root, &params, &error);
        if (is_ok(status)) status = require_string(*params, "mapping_id", &mapping_id, &error);
        if (is_ok(status)) status = context.backend_service->delete_modbus_register_mapping(mapping_id, &error);
        if (!is_ok(status)) error_response(context.id_json, status, error, response);
        else *response = ipc_protocol::build_success_response(context.id_json, nlohmann::json{{"deleted", true}});
        return true;
    }
    return false;
}

}  // namespace edge_controller::ipc_handlers
