// 系统 IPC handler：提供状态、概览、轮询控制和最近错误等服务级接口。
#include "interface/ipc_handlers.h"

#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "interface/ipc_json.h"
#include "interface/ipc_protocol.h"
#include "service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {
namespace {

bool build_update_response(
    const nlohmann::json& id_json,
    StatusCode status,
    const std::string& result_json,
    const std::string& error_message,
    const std::string& fallback_error,
    std::string* response_json)
{
    if (respond_if_error(status, id_json, error_message.empty() ? fallback_error : error_message, response_json)) return true;
    try {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json::parse(result_json));
    } catch (const std::exception& error) {
        *response_json = ipc_protocol::build_error_response(
            id_json,
            "protocol_error",
            std::string("升级引擎结果解析失败：") + error.what());
    }
    return true;
}

}  // namespace

// 处理系统请求。
bool handle_system_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;

    // init/status 使用的轻量 RPC；只有完成 UDS 帧解析、worker 调度和响应写回才算就绪。
    if (method == "health_check") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{{"ready", true}});
        return true;
    }

    // 系统状态和页面原子快照查询。
    if (method == "get_system_status") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_system_status()));
            return true;
    }

    if (method == "get_realtime_view_snapshot") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_realtime_view_snapshot()));
            return true;
    }

    if (method == "get_system_overview_snapshot") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_system_overview_snapshot()));
            return true;
    }

    // 应用级离线升级只暴露安全包标识和 job_id，不接受任意路径或命令。
    if (method == "get_update_current_version") {
        std::string result_json;
        std::string error_message;
        const auto status = backend_service_->get_update_current_version(&result_json, &error_message);
        return build_update_response(
            id_json, status, result_json, error_message, "查询当前安装版本失败", response_json);
    }

    if (method == "get_update_status" || method == "get_recent_update_result") {
        std::string result_json;
        std::string error_message;
        const auto status = backend_service_->get_update_status(&result_json, &error_message);
        return build_update_response(
            id_json, status, result_json, error_message, "查询升级状态失败", response_json);
    }

    if (method == "validate_update_package") {
        const auto package_identifier = optional_string_param(root, "package");
        if (package_identifier.empty()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, "invalid_argument", "缺少 incoming 升级包标识 package");
            return true;
        }
        std::string result_json;
        std::string error_message;
        const auto status = backend_service_->validate_update_package(
            package_identifier, &result_json, &error_message);
        return build_update_response(
            id_json, status, result_json, error_message, "升级包校验失败", response_json);
    }

    if (method == "import_application_upgrade_package") {
        const auto upload_id = optional_string_param(root, "upload_id");
        const auto package_identifier = optional_string_param(root, "package");
        if (upload_id.empty() || package_identifier.empty()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, "invalid_argument", "缺少上传标识 upload_id 或升级包标识 package");
            return true;
        }
        std::string result_json;
        std::string error_message;
        const auto status = backend_service_->import_application_upgrade_package(
            upload_id, package_identifier, &result_json, &error_message);
        return build_update_response(
            id_json, status, result_json, error_message, "升级包安全接管或校验失败", response_json);
    }

    if (method == "start_update_job") {
        const auto job_id = optional_string_param(root, "job_id");
        if (job_id.empty()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, "invalid_argument", "缺少升级任务 job_id");
            return true;
        }
        std::string result_json;
        std::string error_message;
        const auto status = backend_service_->start_update_job(job_id, &result_json, &error_message);
        return build_update_response(
            id_json, status, result_json, error_message, "启动独立升级任务失败", response_json);
    }

    if (method == "get_overview_page_snapshot") {
        OverviewPageSnapshot snapshot;
        std::string error_message;
        const auto status = backend_service_->get_overview_page_snapshot(&snapshot, &error_message);
        if (respond_if_error(status, id_json, error_message.empty() ? "系统概览首屏快照获取失败" : error_message, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(snapshot));
        return true;
    }

    // 最近轮询与错误摘要查询。
    if (method == "get_recent_polling_summary") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_recent_polling_summary()));
            return true;
    }

    if (method == "get_recent_error_summary") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_recent_error_summary()));
            return true;
    }

    // 轮询服务启动和停止控制。
    if (method == "start_polling") {
        const auto status = backend_service_->start_polling();
        if (!is_ok(status)) {
            const auto error = backend_service_->get_recent_error_summary();
            *response_json = ipc_protocol::build_error_response(
                id_json,
                status_code_string(status),
                error.message.empty() ? "启动轮询失败" : error.message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_recent_polling_summary()));
            return true;
    }

    if (method == "stop_polling") {
        backend_service_->stop_polling();
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_recent_polling_summary()));
            return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
