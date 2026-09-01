// 告警 IPC handler：维护设备数据项级规则并返回当前活动告警快照。
#include "interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "interface/ipc_json.h"
#include "interface/ipc_protocol.h"
#include "service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理告警请求。
bool handle_alarms_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;

    // 报警规则查询与维护。
    if (method == "list_alarm_rules") {
        std::optional<DeviceId> device_id;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_optional_alarm_device_id(root, &device_id, &request_error);
        if (respond_if_error(request_status, id_json, request_error, response_json)) return true;
        std::vector<AlarmRule> rules;
        std::string query_error;
        const auto status = device_id.has_value()
                                ? backend_service_->list_device_alarm_rules(*device_id, &rules, &query_error)
                                : backend_service_->list_alarm_rules(&rules, &query_error);
        if (respond_if_error(status, id_json, query_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json_array(rules));
        return true;
    }

    if (method == "upsert_alarm_rule") {
        AlarmRule request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_alarm_rule_upsert_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error, response_json)) return true;
        AlarmRule saved;
        std::string message;
        std::string save_error;
        const auto status = backend_service_->upsert_alarm_rule(request, &saved, &message, &save_error);
        if (respond_if_error(status, id_json, save_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, nlohmann::json{{"message", message}, {"rule", ipc_json::to_json(saved)}});
        return true;
    }

    if (method == "delete_alarm_rule") {
        DeviceId device_id;
        std::string point_key;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_alarm_rule_key_request(root, &device_id, &point_key, &request_error);
        if (respond_if_error(request_status, id_json, request_error, response_json)) return true;
        std::string message;
        std::string delete_error;
        const auto status = backend_service_->delete_alarm_rule(device_id, point_key, &message, &delete_error);
        if (respond_if_error(status, id_json, delete_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, nlohmann::json{{"message", message}});
        return true;
    }

    // 活动报警查询与确认。
    if (method == "list_active_alarms") {
        std::vector<ActiveAlarmView> alarms;
        std::string query_error;
        const auto status = backend_service_->list_active_alarms(&alarms, &query_error);
        if (respond_if_error(status, id_json, query_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json_array(alarms));
        return true;
    }

    if (method == "acknowledge_active_alarm") {
        const auto* params = request_params(root);
        const auto* device_id_json = params == nullptr ? nullptr : json_field(*params, "device_id");
        const auto* point_key_json = params == nullptr ? nullptr : json_field(*params, "point_key");
        const auto* acknowledged_by_json = params == nullptr ? nullptr : json_field(*params, "acknowledged_by");
        if (device_id_json == nullptr || !device_id_json->is_string() ||
            point_key_json == nullptr || !point_key_json->is_string() ||
            acknowledged_by_json == nullptr || !acknowledged_by_json->is_string() ||
            device_id_json->get<std::string>().empty() || point_key_json->get<std::string>().empty() ||
            acknowledged_by_json->get<std::string>().empty()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, status_code_string(StatusCode::kInvalidArgument),
                "device_id、point_key 和 acknowledged_by 不能为空");
            return true;
        }
        std::optional<TimestampMs> active_since_ms;
        const auto* active_since_json = json_field(*params, "active_since_ms");
        if (active_since_json != nullptr && !active_since_json->is_null()) {
            if ((!active_since_json->is_number_unsigned() && !active_since_json->is_number_integer()) ||
                (active_since_json->is_number_integer() && active_since_json->get<std::int64_t>() < 0)) {
                *response_json = ipc_protocol::build_error_response(
                    id_json, status_code_string(StatusCode::kInvalidArgument), "active_since_ms 必须是非负整数");
                return true;
            }
            active_since_ms = active_since_json->get<std::uint64_t>();
        }
        ActiveAlarmView alarm;
        std::string message;
        std::string acknowledge_error;
        const auto status = backend_service_->acknowledge_active_alarm(
            device_id_json->get<std::string>(), point_key_json->get<std::string>(),
            acknowledged_by_json->get<std::string>(), active_since_ms, &alarm, &message, &acknowledge_error);
        if (respond_if_error(status, id_json, acknowledge_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json, nlohmann::json{{"message", message}, {"alarm", ipc_json::to_json(alarm)}});
        return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
