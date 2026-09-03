// 采集配置 IPC handler：处理通道、主站、设备、实时值和通讯报文查询/维护请求。
#include "application/interface/ipc_handlers.h"

#include <optional>
#include <string>
#include <vector>

#include "application/interface/ipc_json.h"
#include "application/interface/ipc_protocol.h"
#include "application/service/backend_service.h"

namespace edge_controller {
namespace ipc_handlers {

// 处理采集请求。
bool handle_collection_request(const IpcHandlerContext& context, std::string* response_json)
{
    const auto& root = context.root;
    const auto& id_json = context.id_json;
    const auto& method = context.method;
    auto* backend_service_ = context.backend_service;

    // 通道查询、保存和删除。
    if (method == "list_channels") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(backend_service_->get_channels()));
            return true;
    }

    if (method == "create_channel_config" || method == "update_channel_config") {
        ChannelConfigUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_channel_config_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "通道配置请求无效" : request_error, response_json)) return true;

        ChannelConfigUpdateResult result;
        std::string update_error;
        const auto update_status = method == "create_channel_config"
                                       ? backend_service_->create_channel_config(request, &result, &update_error)
                                       : backend_service_->update_channel_config(request, &result, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "更新通道配置失败" : update_error, response_json)) return true;

        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "delete_channel_config") {
        const auto channel_id = ipc_protocol::extract_channel_id(root);
        if (channel_id.empty()) {
            *response_json = ipc_protocol::build_error_response(id_json, "invalid_request", "缺少 params.channel_id");
            return true;
        }

        ChannelConfigDeleteResult result;
        std::string delete_error;
        const auto delete_status = backend_service_->delete_channel_config(channel_id, &result, &delete_error);
        if (respond_if_error(delete_status, id_json, delete_error.empty() ? "删除通道配置失败" : delete_error, response_json)) return true;

        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    // 主站保存和删除。
    if (method == "create_master_config" || method == "update_master_config") {
        MasterNodeConfigUpdateRequest request;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_master_config_update_request(
            root,
            &request,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "主控配置请求无效" : request_error, response_json)) return true;

        MasterNodeConfigUpdateResult result;
        std::string update_error;
        const auto update_status = method == "create_master_config"
                                       ? backend_service_->create_master_config(request, &result, &update_error)
                                       : backend_service_->update_master_config(request, &result, &update_error);
        if (respond_if_error(update_status, id_json, update_error.empty() ? "更新主控配置失败" : update_error, response_json)) return true;

        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    if (method == "delete_master_config") {
        const auto master_id = ipc_protocol::extract_master_id(root);
        if (master_id.empty()) {
            *response_json = ipc_protocol::build_error_response(id_json, "invalid_request", "缺少 params.master_id");
            return true;
        }

        MasterNodeConfigDeleteResult result;
        std::string delete_error;
        const auto delete_status = backend_service_->delete_master_config(master_id, &result, &delete_error);
        if (respond_if_error(delete_status, id_json, delete_error.empty() ? "删除主控配置失败" : delete_error, response_json)) return true;

        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
            return true;
    }

    // 串口、主站和设备配置查询。
    if (method == "list_serial_ports") {
        std::vector<SerialPortInfo> ports;
        std::string error_message;
        const auto status = backend_service_->list_serial_ports(&ports, &error_message);
        if (respond_if_error(status, id_json, error_message.empty() ? "系统串口扫描失败" : error_message, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(ports));
            return true;
    }

    if (method == "list_masters") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(backend_service_->get_masters()));
            return true;
    }

    if (method == "list_devices") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(backend_service_->get_devices()));
            return true;
    }

    // 单个或批量更新设备显示名称。
    if (method == "update_device_display_name") {
        const auto params = root.find("params");
        if (params == root.end() || !params->is_object() ||
            !params->contains("device_id") || !(*params)["device_id"].is_string() ||
            !params->contains("display_name") || !(*params)["display_name"].is_string()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, "invalid_argument", "设备名称更新参数格式不正确");
            return true;
        }
        DeviceConfig updated;
        std::string update_error;
        const auto status = backend_service_->update_device_display_name(
            (*params)["device_id"].get<std::string>(),
            (*params)["display_name"].get<std::string>(),
            &updated,
            &update_error);
        if (respond_if_error(status, id_json, update_error.empty() ? "设备名称保存失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(updated));
        return true;
    }

    if (method == "update_device_display_names_batch") {
        const auto params = root.find("params");
        if (params == root.end() || !params->is_object() ||
            !params->contains("items") || !(*params)["items"].is_array() ||
            (*params)["items"].empty()) {
            *response_json = ipc_protocol::build_error_response(
                id_json, "invalid_argument", "批量设备名称更新参数格式不正确");
            return true;
        }
        std::vector<DeviceDisplayNameBatchItem> items;
        items.reserve((*params)["items"].size());
        for (const auto& item : (*params)["items"]) {
            if (!item.is_object() || !item.contains("device_id") ||
                !item["device_id"].is_string() || !item.contains("display_name") ||
                !item["display_name"].is_string()) {
                *response_json = ipc_protocol::build_error_response(
                    id_json, "invalid_argument", "批量设备名称条目格式不正确");
                return true;
            }
            items.push_back({
                item["device_id"].get<std::string>(),
                item["display_name"].get<std::string>(),
            });
        }
        DeviceDisplayNameBatchResult result;
        std::string update_error;
        const auto status = backend_service_->update_device_display_names_batch(
            items, &result, &update_error);
        if (respond_if_error(status, id_json, update_error.empty() ? "批量设备名称保存失败" : update_error, response_json)) return true;
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(result));
        return true;
    }

    // 设备实时值与通道通讯报文查询。
    if (method == "list_device_realtime") {
        *response_json = ipc_protocol::build_success_response(
            id_json,
            ipc_json::to_json_array(backend_service_->get_all_device_realtime()));
            return true;
    }

    if (method == "get_channel_communication_traces") {
        ChannelId channel_id;
        std::uint32_t limit = ipc_protocol::kCommunicationTraceDefaultLimit;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_channel_communication_trace_query_request(
            root,
            &channel_id,
            &limit,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "通讯报文查询请求参数非法" : request_error, response_json)) return true;

        ChannelCommunicationTraces traces;
        traces.channel_id = channel_id;
        traces.records = backend_service_->get_channel_communication_traces(channel_id, limit);
        *response_json = ipc_protocol::build_success_response(id_json, ipc_json::to_json(traces));
            return true;
    }

    if (method == "clear_channel_communication_traces") {
        ChannelId channel_id;
        std::string request_error;
        const auto request_status = ipc_protocol::extract_channel_communication_trace_clear_request(
            root,
            &channel_id,
            &request_error);
        if (respond_if_error(request_status, id_json, request_error.empty() ? "通讯报文清空请求参数非法" : request_error, response_json)) return true;

        backend_service_->clear_channel_communication_traces(channel_id);
        *response_json = ipc_protocol::build_success_response(
            id_json,
            nlohmann::json{
                {"channel_id", channel_id},
                {"cleared", true},
            });
            return true;
    }

    return false;
}

}  // namespace ipc_handlers
}  // namespace edge_controller
