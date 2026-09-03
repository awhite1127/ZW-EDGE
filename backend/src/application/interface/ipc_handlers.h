// IPC handler 分派接口：按领域拆分方法实现，共享统一请求 ID 和错误响应格式。
#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "shared/common/status_code.h"
#include "application/interface/ipc_protocol.h"
#include "data/datastore/event_store.h"
#include "data/datastore/history_store.h"

namespace edge_controller {

class BackendService;

namespace ipc_handlers {

struct IpcHandlerContext {
    BackendService* backend_service{nullptr};
    const nlohmann::json& root;
    const nlohmann::json& id_json;
    const std::string& method;
};

// 将状态码转换为字符串。
inline std::string status_code_string(StatusCode code)
{
    return to_string(code);
}

// 失败时写入统一错误响应；成功时不修改输出。
inline bool respond_if_error(
    StatusCode status,
    const nlohmann::json& id,
    const std::string& message,
    std::string* response)
{
    if (is_ok(status)) return false;
    *response = ipc_protocol::build_error_response(id, status_code_string(status), message);
    return true;
}

// 按名称查找 JSON 对象字段。
inline const nlohmann::json* json_field(const nlohmann::json& object, const std::string& field)
{
    if (!object.is_object()) {
        return nullptr;
    }
    const auto iterator = object.find(field);
    return iterator == object.end() ? nullptr : &*iterator;
}

// 返回 IPC 请求的参数对象。
inline const nlohmann::json* request_params(const nlohmann::json& root)
{
    const auto* params = json_field(root, "params");
    return params != nullptr && params->is_object() ? params : nullptr;
}

// 读取可选字符串请求参数，缺失时返回空值。
inline std::string optional_string_param(const nlohmann::json& root, const std::string& field)
{
    const auto* params = request_params(root);
    if (params == nullptr) {
        return {};
    }
    const auto* value = json_field(*params, field);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return value->get<std::string>();
}

// 读取可选无符号整数请求参数，缺失时使用默认值。
inline std::uint32_t optional_uint_param(const nlohmann::json& root, const std::string& field, std::uint32_t fallback)
{
    const auto* params = request_params(root);
    if (params == nullptr) {
        return fallback;
    }
    const auto* value = json_field(*params, field);
    if (value == nullptr || (!value->is_number_unsigned() && !value->is_number_integer())) {
        return fallback;
    }
    if (value->is_number_integer() && value->get<std::int64_t>() < 0) {
        return fallback;
    }
    const auto parsed = value->get<std::uint64_t>();
    constexpr auto max_value = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    return parsed > max_value ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(parsed);
}

// 提取历史数据导出查询参数。
inline HistoryExportQuery extract_history_export_query(const nlohmann::json& root)
{
    HistoryExportQuery query;
    query.channel_id = optional_string_param(root, "channel_id");
    query.master_id = optional_string_param(root, "master_id");
    query.device_id = optional_string_param(root, "device_id");
    query.point_key = optional_string_param(root, "point_key");
    query.sample_period = optional_string_param(root, "sample_period");
    if (query.sample_period.empty()) {
        query.sample_period = optional_string_param(root, "period");
    }
    query.limit = optional_uint_param(root, "limit", query.limit);
    query.offset = optional_uint_param(root, "offset", query.offset);
    return query;
}

// 提取事件导出查询参数。
inline EventExportQuery extract_event_export_query(const nlohmann::json& root)
{
    EventExportQuery query;
    query.level = optional_string_param(root, "level");
    query.source = optional_string_param(root, "source");
    query.time_range = optional_string_param(root, "time_range");
    if (query.time_range.empty()) {
        query.time_range = optional_string_param(root, "range");
    }
    query.search = optional_string_param(root, "search");
    if (query.search.empty()) {
        query.search = optional_string_param(root, "q");
    }
    query.limit = optional_uint_param(root, "limit", query.limit);
    query.offset = optional_uint_param(root, "offset", query.offset);
    return query;
}

// 提取历史事件页筛选与分页参数。
inline EventHistoryQuery extract_event_history_query(const nlohmann::json& root)
{
    EventHistoryQuery query;
    query.level = optional_string_param(root, "level");
    query.source = optional_string_param(root, "source");
    query.time_range = optional_string_param(root, "time_range");
    if (query.time_range.empty()) {
        query.time_range = optional_string_param(root, "range");
    }
    query.search = optional_string_param(root, "search");
    if (query.search.empty()) {
        query.search = optional_string_param(root, "q");
    }
    query.page = optional_uint_param(root, "page", query.page);
    query.page_size = optional_uint_param(root, "page_size", query.page_size);
    return query;
}

// 处理系统请求。
bool handle_system_request(const IpcHandlerContext& context, std::string* response);
// 处理事件请求。
bool handle_events_request(const IpcHandlerContext& context, std::string* response);
// 处理告警请求。
bool handle_alarms_request(const IpcHandlerContext& context, std::string* response);
// 处理设置请求。
bool handle_settings_request(const IpcHandlerContext& context, std::string* response);
// 处理 Modbus 北向配置管理请求。
bool handle_modbus_server_request(const IpcHandlerContext& context, std::string* response);
// 处理采集请求。
bool handle_collection_request(const IpcHandlerContext& context, std::string* response);
// 处理历史数据请求。
bool handle_history_request(const IpcHandlerContext& context, std::string* response);
// 处理模板请求。
bool handle_templates_request(const IpcHandlerContext& context, std::string* response);
// 处理设备命令请求。
bool handle_device_command_request(const IpcHandlerContext& context, std::string* response);

}  // namespace ipc_handlers
}  // namespace edge_controller
