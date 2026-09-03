// 设备模板 IPC handler：只读查询模板定义、引用关系和管理视图。
#include "application/interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "application/interface/ipc_json.h"
#include "application/interface/ipc_protocol.h"
#include "application/service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理模板请求。
bool handle_templates_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;

    // 查询设备类型管理视图。
    if (method == "get_device_template_management") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_device_template_management()));
            return true;
    }

    // 创建设备类型并返回规范化后的定义。
    if (method == "create_device_template") {
        DeviceTemplateDefinition request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_device_template_create_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "设备模板创建请求无效" : request_error, response_json)) return true;

        DeviceTemplateManagementView view;
        std::string create_error;
        const auto create_status = backend_service_->create_device_template(request, &view, &create_error);
        if (respond_if_error(create_status, id_json, create_error.empty() ? "新增设备模板失败" : create_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{
                {"message", "设备模板已创建"},
                {"template_management", ipc_json::to_json(view)},
            });
            return true;
    }

    // 更新设备类型完整定义。
    if (method == "update_device_template") {
        DeviceTemplateDefinition request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_device_template_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "设备模板更新请求无效" : request_error, response_json)) return true;

        DeviceTemplateManagementView view;
        std::string update_error;
        const auto update_status = backend_service_->update_device_template(request, &view, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "编辑设备模板失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{
                {"message", "设备模板已更新"},
                {"template_management", ipc_json::to_json(view)},
            });
            return true;
    }

    // 更新字段的实时显示与历史记录偏好。
    if (method == "update_device_template_realtime_display") {
        const auto params_it = root.find("params");
        if (params_it == root.end() || !params_it->is_object() ||
            !params_it->contains("template_id") || !(*params_it)["template_id"].is_string() ||
            !params_it->contains("field_key") || !(*params_it)["field_key"].is_string() ||
            !params_it->contains("show_in_realtime") || !(*params_it)["show_in_realtime"].is_boolean()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, status_code_string(StatusCode::kInvalidArgument), "实时展示配置请求无效");
            return true;
        }
        DeviceTemplateManagementView view;
        std::string update_error;
        const auto update_status = backend_service_->update_device_template_realtime_display(
            (*params_it)["template_id"].get<std::string>(),
            (*params_it)["field_key"].get<std::string>(),
            (*params_it)["show_in_realtime"].get<bool>(),
            &view,
            &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "保存实时展示配置失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, nlohmann::json{
            {"message", "实时展示配置已保存"},
            {"template_management", ipc_json::to_json(view)},
        });
        return true;
    }

    if (method == "update_device_template_history_enabled") {
        const auto params_it = root.find("params");
        if (params_it == root.end() || !params_it->is_object() ||
            !params_it->contains("template_id") || !(*params_it)["template_id"].is_string() ||
            !params_it->contains("field_key") || !(*params_it)["field_key"].is_string() ||
            !params_it->contains("history_enabled") || !(*params_it)["history_enabled"].is_boolean()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, status_code_string(StatusCode::kInvalidArgument), "历史记录配置请求无效");
            return true;
        }
        DeviceTemplateManagementView view;
        std::string update_error;
        const auto update_status = backend_service_->update_device_template_history_enabled(
            (*params_it)["template_id"].get<std::string>(),
            (*params_it)["field_key"].get<std::string>(),
            (*params_it)["history_enabled"].get<bool>(),
            &view,
            &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "保存历史记录配置失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, nlohmann::json{
            {"message", "历史记录配置已保存"},
            {"template_management", ipc_json::to_json(view)},
        });
        return true;
    }

    // 删除自定义设备类型。
    if (method == "delete_device_template") {
        std::string template_id;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_device_template_delete_request(
            root,
            &template_id,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "设备模板删除请求无效" : request_error, response_json)) return true;

        DeviceTemplateManagementView view;
        std::string delete_error;
        const auto delete_status = backend_service_->delete_device_template(template_id, &view, &delete_error);
        if (respond_if_error(delete_status, id_json, delete_error.empty() ? "删除设备模板失败" : delete_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{
                {"message", "设备模板已删除"},
                {"template_management", ipc_json::to_json(view)},
            });
            return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
