// 设备命令 IPC handler：只接受模板声明的命令并执行严格参数校验，禁止任意寄存器写入。
#include "application/interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "application/interface/ipc_json.h"
#include "application/interface/ipc_protocol.h"
#include "application/service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理设备命令请求。
bool handle_device_command_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;

    // 设备控制只接受模板声明的命令；底层 FC10 不作为通用 IPC method 暴露。
    if (method == "execute_device_command") {
        DeviceCommandExecuteRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_device_command_execute_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "设备命令执行请求参数非法" : request_error, response_json)) return true;

        DeviceCommandExecuteResponse response;
        std::string error_message;
        (void)backend_service_->execute_device_command(request, &response, &error_message);
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(response));
            return true;
    }

    // EM100 事件和测试记录主动读取。
    if (method == "read_em100_event_record" || method == "read_em100_test_record") {
        const auto device_id = ipc_protocol::extract_device_id(root);
        if (device_id.empty()) {
            *response_json = ipc_protocol::build_error_response(id_json, "invalid_request", "缺少 params.device_id");
            return true;
        }

        EM100RecordReadResponse response;
        std::string error_message;
        const auto status = method == "read_em100_event_record"
                                ? backend_service_->read_em100_event_record(device_id, &response, &error_message)
                                : backend_service_->read_em100_test_record(device_id, &response, &error_message);
        if (!is_ok(status) && response.read_result.error_message.empty()) {
            *response_json = ipc_protocol::build_error_response(
                id_json,
                status_code_string(status),
                error_message.empty() ? "EM100 记录读取失败" : error_message);
            return true;
        }
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(response));
            return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
