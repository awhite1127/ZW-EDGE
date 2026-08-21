// 历史 IPC handler：校验分页和筛选条件后查询历史仓库，不暴露数据库文件。
#include "interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "interface/ipc_json.h"
#include "interface/ipc_protocol.h"
#include "service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理历史数据请求。
bool handle_history_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;
    (void)root;
    (void)id_json;
    (void)method;
    (void)backend_service_;

    // 设备历史视图和总览摘要查询。
    if (method == "get_device_history_view") {
        DeviceHistoryQuery query;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_device_history_query_request(root, &query, &request_error);
        if (!is_ok(request_status)) {
            *response_json = ipc_protocol::build_error_response(
                id_json,
                status_code_string(request_status),
                request_error.empty() ? "设备历史查询请求参数非法" : request_error);
            return true;
        }

        DeviceHistoryView view;
        std::string error_message;
        const auto status = backend_service_->get_device_history_view(query, &view, &error_message);
        if (!is_ok(status)) {
            *response_json = ipc_protocol::build_error_response(
                id_json,
                status_code_string(status),
                error_message.empty() ? "历史数据页面加载失败" : error_message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(view));
            return true;
    }

    if (method == "get_history_overview_summaries") {
        std::vector<HistoryOverviewSummary> summaries;
        std::string error_message;
        const auto status = backend_service_->get_history_overview_summaries(&summaries, &error_message);
        if (!is_ok(status)) {
            *response_json = ipc_protocol::build_error_response(
                id_json,
                status_code_string(status),
                error_message.empty() ? "历史总览摘要获取失败" : error_message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json_array(summaries));
        return true;
    }

    // 历史记录导出与过期数据维护。
    if (method == "export_history_records") {
        const auto query = extract_history_export_query(root);
        std::vector<HistoryRecord> records;
        std::string error_message;
        const auto status = backend_service_->export_history_records(query, &records, &error_message);
        if (!is_ok(status)) {
            *response_json = ipc_protocol::build_error_response(
                id_json,
                status_code_string(status),
                error_message.empty() ? "历史数据导出查询失败" : error_message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json_array(records));
            return true;
    }

    if (method == "get_data_maintenance_summary") {
        DataMaintenanceSummary summary;
        std::string error_message;
        const auto status = backend_service_->get_data_maintenance_summary(&summary, &error_message);
        if (!is_ok(status)) {
            *response_json = ipc_protocol::build_error_response(
                id_json, status_code_string(status),
                error_message.empty() ? "数据维护摘要获取失败" : error_message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(summary));
        return true;
    }

    if (method == "cleanup_expired_data") {
        DataMaintenanceSummary summary;
        std::string error_message;
        const auto status = backend_service_->cleanup_expired_data(&summary, &error_message);
        if (!is_ok(status)) {
            *response_json = ipc_protocol::build_error_response(
                id_json, status_code_string(status),
                error_message.empty() ? "超期数据清理失败" : error_message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(summary));
        return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
