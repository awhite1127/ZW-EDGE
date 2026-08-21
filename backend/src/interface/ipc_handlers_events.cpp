// 事件 IPC handler：提供最近事件、分页导出和清理动作，事件聚合逻辑保留在仓库层。
#include "interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "interface/ipc_json.h"
#include "interface/ipc_protocol.h"
#include "service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理事件请求。
bool handle_events_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;
    (void)root;
    (void)id_json;
    (void)method;
    (void)backend_service_;

    if (method == "list_recent_events") {
        std::vector<ServiceEvent> events;
        std::string event_error;
        const auto event_status = backend_service_->get_recent_events(100, &events, &event_error);
        if (!is_ok(event_status)) {
            *response_json = ipc_protocol::build_error_response(id_json, status_code_string(event_status), event_error);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(events));
            return true;
    }

    if (method == "export_service_events") {
        const auto query = extract_event_export_query(root);
        std::vector<ServiceEvent> events;
        std::string event_error;
        const auto event_status = backend_service_->export_service_events(query, &events, &event_error);
        if (!is_ok(event_status)) {
            *response_json = ipc_protocol::build_error_response(id_json, status_code_string(event_status), event_error);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(events));
            return true;
    }

    if (method == "clear_recent_events") {
        std::string event_error;
        const auto event_status = backend_service_->clear_recent_events(&event_error);
        if (!is_ok(event_status)) {
            *response_json = ipc_protocol::build_error_response(id_json, status_code_string(event_status), event_error);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{{"message", "历史事件已清除"}});
            return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
