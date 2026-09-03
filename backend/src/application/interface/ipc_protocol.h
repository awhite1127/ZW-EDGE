// IPC 长度前缀协议和 JSON-RPC 风格包络定义。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "shared/common/status_code.h"
#include "shared/common/types.h"
#include "data/model/channel_update.h"
#include "data/model/alarm.h"
#include "data/model/device_history_view.h"
#include "data/model/device_template.h"
#include "data/model/master_node_update.h"
#include "data/model/modbus_write_request.h"
#include "data/model/mqtt_settings.h"
#include "data/model/network_settings.h"
#include "data/model/service_summary.h"
#include "data/model/system_settings.h"
#include "data/model/web_auth.h"

namespace edge_controller::ipc_protocol {

constexpr std::uint32_t kFrameHeaderBytes = 4;
// 实时聚合快照在最大合法设备规模下可超过 1 MiB。仍保留固定上限，避免
// 本地客户端用无界载荷耗尽内存；Go Web 端必须与此值保持一致。
constexpr std::uint32_t kMaxPayloadBytes = 8 * 1024 * 1024;
constexpr std::uint32_t kCommunicationTraceDefaultLimit = 10;
constexpr std::uint32_t kCommunicationTraceMaxLimit = 50;

// 校验载荷长度。
StatusCode validate_payload_length(std::uint32_t length, std::string* error_message);
// 解析请求JSON。
StatusCode parse_request_json(
    const std::string& request_json,
    nlohmann::json* request,
    std::string* error_message);
// 提取 IPC 请求方法名。
StatusCode extract_request_method(
    const nlohmann::json& request,
    std::string* method,
    std::string* error_message);

// 提取请求标识，缺失或无效时返回 JSON null。
nlohmann::json request_id_json_or_null(const nlohmann::json& request);
// 构造成功响应。
std::string build_success_response(const nlohmann::json& id, const nlohmann::json& result);
// 构造错误信息响应。
std::string build_error_response(
    const nlohmann::json& id,
    const std::string& code,
    const std::string& message);

// 提取通道标识。
std::string extract_channel_id(const nlohmann::json& request);
// 提取主站标识。
std::string extract_master_id(const nlohmann::json& request);
// 提取设备标识。
std::string extract_device_id(const nlohmann::json& request);
// 提取设备历史数据查询参数。
StatusCode extract_device_history_query_request(
    const nlohmann::json& request,
    DeviceHistoryQuery* result,
    std::string* error_message);
// 提取通道通讯报文查询参数。
StatusCode extract_channel_communication_trace_query_request(
    const nlohmann::json& request,
    ChannelId* channel_id,
    std::uint32_t* limit,
    std::string* error_message);
// 提取通道通讯报文清理参数。
StatusCode extract_channel_communication_trace_clear_request(
    const nlohmann::json& request,
    ChannelId* channel_id,
    std::string* error_message);
// 提取设备命令执行请求。
StatusCode extract_device_command_execute_request(
    const nlohmann::json& request,
    DeviceCommandExecuteRequest* result,
    std::string* error_message);
// 提取通道配置更新请求。
StatusCode extract_channel_config_update_request(
    const nlohmann::json& request,
    ChannelConfigUpdateRequest* result,
    std::string* error_message);
// 提取主站配置更新请求。
StatusCode extract_master_config_update_request(
    const nlohmann::json& request,
    MasterNodeConfigUpdateRequest* result,
    std::string* error_message);
// 提取设备类型创建请求。
StatusCode extract_device_template_create_request(
    const nlohmann::json& request,
    DeviceTemplateDefinition* result,
    std::string* error_message);
// 提取设备类型更新请求。
StatusCode extract_device_template_update_request(
    const nlohmann::json& request,
    DeviceTemplateDefinition* result,
    std::string* error_message);
// 提取设备类型删除请求。
StatusCode extract_device_template_delete_request(
    const nlohmann::json& request,
    std::string* template_id,
    std::string* error_message);
// 解析输入并写入结构化结果。
StatusCode extract_system_settings_update_request(
    const nlohmann::json& request,
    SystemSettingsUpdateRequest* result,
    std::string* error_message);
// 解析输入并写入结构化结果。
StatusCode extract_time_settings_update_request(
    const nlohmann::json& request,
    TimeSettingsUpdateRequest* result,
    std::string* error_message);
// 提取手动设置系统时间的请求。
StatusCode extract_manual_time_set_request(
    const nlohmann::json& request,
    ManualTimeSetRequest* result,
    std::string* error_message);
// 解析输入并写入结构化结果。
StatusCode extract_network_settings_update_request(
    const nlohmann::json& request,
    NetworkSettingsUpdateRequest* result,
    std::string* error_message);
// 解析输入并写入结构化结果。
StatusCode extract_mqtt_settings_update_request(
    const nlohmann::json& request,
    MqttSettingsUpdateRequest* result,
    std::string* error_message);
// 提取配置导入请求。
StatusCode extract_config_import_request(
    const nlohmann::json& request,
    ConfigImportRequest* result,
    std::string* error_message);
// 提取 Web 登录请求。
StatusCode extract_web_login_request(
    const nlohmann::json& request,
    WebLoginRequest* result,
    std::string* error_message);
// 提取 Web 密码修改请求。
StatusCode extract_web_password_change_request(
    const nlohmann::json& request,
    WebPasswordChangeRequest* result,
    std::string* error_message);
// 提取 Web 查看员密码设置请求。
StatusCode extract_web_viewer_password_set_request(
    const nlohmann::json& request,
    WebViewerPasswordSetRequest* result,
    std::string* error_message);
// 提取 Web 用户创建请求。
StatusCode extract_web_user_create_request(
    const nlohmann::json& request,
    WebUserCreateRequest* result,
    std::string* error_message);
// 提取 Web 用户更新请求。
StatusCode extract_web_user_update_request(
    const nlohmann::json& request,
    WebUserUpdateRequest* result,
    std::string* error_message);
// 提取 Web 用户密码重置请求。
StatusCode extract_web_user_password_reset_request(
    const nlohmann::json& request,
    WebUserPasswordResetRequest* result,
    std::string* error_message);
// 提取告警规则新增或更新请求。
StatusCode extract_alarm_rule_upsert_request(const nlohmann::json& request, AlarmRule* result, std::string* error_message);
// 提取告警规则键请求。
StatusCode extract_alarm_rule_key_request(const nlohmann::json& request, DeviceId* device_id, std::string* point_key, std::string* error_message);
// 提取可选的告警设备标识。
StatusCode extract_optional_alarm_device_id(const nlohmann::json& request, std::optional<DeviceId>* device_id, std::string* error_message);

}  // namespace edge_controller::ipc_protocol
