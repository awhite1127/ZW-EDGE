// 设置 IPC handler：解析系统、网络、时间、MQTT 和账户请求并调用对应领域入口。
#include "interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "interface/ipc_json.h"
#include "interface/ipc_protocol.h"
#include "service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理设置请求。
bool handle_settings_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;

    // 首次启动管理员入口与恢复出厂设置。
    if (method == "get_first_boot_admin_entry_status") {
        bool available = false;
        std::string status_error;
        const auto status = backend_service_->get_first_boot_admin_entry_available(&available, &status_error);
        if (respond_if_error(status, id_json, status_error.empty() ? "读取首次部署入口状态失败" : status_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{{"available", available}});
        return true;
    }

    if (method == "consume_first_boot_admin_entry") {
        bool consumed = false;
        std::string consume_error;
        const auto status = backend_service_->consume_first_boot_admin_entry(&consumed, &consume_error);
        if (respond_if_error(status, id_json, consume_error.empty() ? "关闭首次部署入口失败" : consume_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{{"consumed", consumed}});
        return true;
    }

    if (method == "request_factory_reset") {
        FactoryResetResult result;
        std::string reset_error;
        const auto reset_status = backend_service_->request_factory_reset(&result, &reset_error);
        if (respond_if_error(reset_status, id_json, reset_error.empty() ? "恢复出厂数据失败" : reset_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    // 系统配置导入与导出。
    if (method == "export_system_config") {
        ConfigExportBundle bundle;
        std::string export_error;
        const auto export_status = backend_service_->export_system_config(&bundle, &export_error);
        if (respond_if_error(export_status, id_json, export_error.empty() ? "导出系统配置失败" : export_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(bundle));
            return true;
    }

    if (method == "import_system_config") {
        ConfigImportRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_config_import_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "配置导入请求无效" : request_error, response_json)) return true;

        ConfigImportResult result;
        std::string import_error;
        const auto import_status = backend_service_->import_system_config(request, &result, &import_error);
        if (respond_if_error(import_status, id_json, import_error.empty() ? "导入系统配置失败" : import_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    // 系统、时间、网络和 MQTT 配置及运行状态查询。
    if (method == "get_config_summary") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_config_summary()));
            return true;
    }

    if (method == "get_system_settings") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_system_settings()));
            return true;
    }

    if (method == "get_time_settings") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_time_settings()));
        return true;
    }

    if (method == "get_time_runtime_status") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_time_runtime_status()));
        return true;
    }

    if (method == "save_and_apply_time_settings") {
        TimeSettingsUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_time_settings_update_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "时间设置请求无效" : request_error, response_json)) return true;
        TimeApplyResult result;
        std::string apply_error;
        const auto apply_status = backend_service_->save_and_apply_time_settings(request, &result, &apply_error);
        if (respond_if_error(apply_status, id_json, apply_error.empty() ? "保存并应用时间设置失败" : apply_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    if (method == "sync_time_now") {
        TimeSyncResult result;
        std::string sync_error;
        const auto sync_status = backend_service_->sync_time_now(&result, &sync_error);
        if (respond_if_error(sync_status, id_json, sync_error.empty() ? "立即同步系统时间失败" : sync_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    if (method == "set_manual_system_time") {
        ManualTimeSetRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_manual_time_set_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "手动时间请求无效" : request_error, response_json)) return true;
        ManualTimeSetResult result;
        std::string set_error;
        const auto set_status = backend_service_->set_manual_system_time(request, &result, &set_error);
        if (respond_if_error(set_status, id_json, set_error.empty() ? "设置系统时间失败" : set_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    if (method == "get_network_settings") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_network_settings()));
            return true;
    }

    if (method == "get_network_runtime_status") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_network_runtime_status()));
            return true;
    }

    if (method == "get_mqtt_settings") {
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(backend_service_->get_mqtt_settings()));
            return true;
    }

    if (method == "get_mqtt_runtime_status") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json(backend_service_->get_mqtt_runtime_status()));
            return true;
    }

    // Web 登录状态、密码和用户管理。
    if (method == "get_web_auth_status") {
        WebAuthStatus status;
        std::string auth_error;
        const auto auth_status = backend_service_->get_web_auth_status(&status, &auth_error);
        if (respond_if_error(auth_status, id_json, auth_error.empty() ? "获取 Web 登录状态失败" : auth_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(status));
            return true;
    }

    if (method == "verify_web_login") {
        WebLoginRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_web_login_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "Web 登录请求无效" : request_error, response_json)) return true;

        WebLoginResult result;
        std::string auth_error;
        const auto auth_status = backend_service_->verify_web_login(request, &result, &auth_error);
        if (respond_if_error(auth_status, id_json, auth_error.empty() ? "Web 登录验证失败" : auth_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "change_web_password") {
        WebPasswordChangeRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_web_password_change_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "Web 账户密码修改请求无效" : request_error, response_json)) return true;

        WebPasswordChangeResult result;
        std::string change_error;
        const auto change_status = backend_service_->change_web_password(request, &result, &change_error);
        if (respond_if_error(change_status, id_json, change_error.empty() ? "修改 Web 账户密码失败" : change_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "set_web_viewer_password") {
        WebViewerPasswordSetRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_web_viewer_password_set_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "只读用户密码设置请求无效" : request_error, response_json)) return true;

        WebPasswordChangeResult result;
        std::string change_error;
        const auto change_status = backend_service_->set_web_viewer_password(request, &result, &change_error);
        if (respond_if_error(change_status, id_json, change_error.empty() ? "设置只读用户密码失败" : change_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "list_web_users") {
        std::vector<WebUserView> users;
        std::string list_error;
        const auto list_status = backend_service_->list_web_users(&users, &list_error);
        if (respond_if_error(list_status, id_json, list_error.empty() ? "读取 Web 用户列表失败" : list_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json_array(users));
        return true;
    }

    if (method == "create_web_user") {
        WebUserCreateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_web_user_create_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "创建用户请求无效" : request_error, response_json)) return true;
        WebUserMutationResult result;
        std::string create_error;
        const auto create_status = backend_service_->create_web_user(request, &result, &create_error);
        if (respond_if_error(create_status, id_json, create_error.empty() ? result.message : create_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    if (method == "update_web_user") {
        WebUserUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_web_user_update_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "更新用户请求无效" : request_error, response_json)) return true;
        WebUserMutationResult result;
        std::string update_error;
        const auto update_status = backend_service_->update_web_user(request, &result, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? result.message : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    if (method == "reset_web_user_password") {
        WebUserPasswordResetRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_web_user_password_reset_request(root, &request, &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "重置用户密码请求无效" : request_error, response_json)) return true;
        WebUserMutationResult result;
        std::string reset_error;
        const auto reset_status = backend_service_->reset_web_user_password(request, &result, &reset_error);
        if (respond_if_error(reset_status, id_json, reset_error.empty() ? result.message : reset_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    // 各类设置保存及立即应用操作。
    if (method == "update_system_settings") {
        SystemSettingsUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_system_settings_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "系统设置请求无效" : request_error, response_json)) return true;

        SystemSettingsUpdateResult result;
        std::string update_error;
        const auto update_status = backend_service_->update_system_settings(request, &result, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "保存系统设置失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "update_network_settings") {
        NetworkSettingsUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_network_settings_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "网络配置请求无效" : request_error, response_json)) return true;

        NetworkSettingsUpdateResult result;
        std::string update_error;
        const auto update_status = backend_service_->update_network_settings(request, &result, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "保存网络配置失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "save_and_apply_network_settings") {
        NetworkSettingsUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_network_settings_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "网络配置请求无效" : request_error, response_json)) return true;

        NetworkApplyResult result;
        std::string apply_error;
        const auto apply_status = backend_service_->save_and_apply_network_settings(request, &result, &apply_error);
        if (respond_if_error(apply_status, id_json, apply_error.empty() ? "保存并应用网络配置失败" : apply_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    if (method == "update_mqtt_settings") {
        MqttSettingsUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_mqtt_settings_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "MQTT 配置请求无效" : request_error, response_json)) return true;

        MqttSettingsUpdateResult result;
        std::string update_error;
        const auto update_status = backend_service_->update_mqtt_settings(request, &result, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "保存 MQTT 配置失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "apply_network_settings") {
        NetworkApplyResult result;
        std::string apply_error;
        const auto apply_status = backend_service_->apply_network_settings(&result, &apply_error);
        if (respond_if_error(apply_status, id_json, apply_error.empty() ? "应用网络配置到系统网口失败" : apply_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
