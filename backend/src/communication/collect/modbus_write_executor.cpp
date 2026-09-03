// Modbus 写执行器负责受控 FC10 写入、请求校验和回读结果组织，避免页面层拼装协议帧。
#include "communication/collect/modbus_write_executor.h"

#include <algorithm>
#include <string>
#include <utility>

#include "communication/channel/i_channel.h"
#include "communication/collect/modbus_trace_recorder.h"
#include "shared/common/enums.h"
#include "shared/common/logger.h"
#include "shared/common/status_code.h"
#include "shared/common/time_utils.h"
#include "data/model/channel_config.h"
#include "communication/protocol/modbus_rtu_protocol.h"
#include "communication/protocol/modbus_tcp_client.h"
#include "communication/protocol/modbus_tcp_protocol.h"

namespace edge_controller {

namespace {

// 根据 RTU 通道状态生成诊断错误码。
DiagnosisErrorCode diagnosis_from_rtu_channel_status(
    const ChannelStatus& channel_status,
    const std::string& error_message,
    StatusCode transport_status)
{
    if (transport_status == StatusCode::kTimeout) {
        return DiagnosisErrorCode::kModbusTimeout;
    }
    if (channel_status.diagnosis.error_code == to_string(DiagnosisErrorCode::kChannelOpenFailed)) {
        return DiagnosisErrorCode::kChannelOpenFailed;
    }
    if (channel_status.diagnosis.error_code == to_string(DiagnosisErrorCode::kChannelConfigFailed)) {
        return DiagnosisErrorCode::kChannelConfigFailed;
    }
    if (channel_status.diagnosis.error_code == to_string(DiagnosisErrorCode::kChannelIoError)) {
        return DiagnosisErrorCode::kChannelIoError;
    }

    const auto classified = classify_channel_error(error_message);
    return classified == DiagnosisErrorCode::kUnknownError ? DiagnosisErrorCode::kChannelIoError : classified;
}

// 记录 Modbus 寄存器写入结果。
void log_write_result(
    const MasterNodeConfig& master_config,
    const ChannelConfig& channel_config,
    std::uint16_t start_register,
    std::uint16_t register_count,
    bool success,
    const std::string& error_message)
{
    if (!Logger::debug_enabled()) {
        return;
    }
    std::string message =
        "主控 " + master_config.master_id +
        (success ? " Modbus FC10 写入成功，通道=" : " Modbus FC10 写入失败，通道=") +
        master_config.channel_id +
        "，Unit ID / 从站地址=" + std::to_string(master_config.target_address) +
        "，起始寄存器=" + std::to_string(start_register) +
        "，寄存器数量=" + std::to_string(register_count);
    if (!error_message.empty()) {
        message += "，错误=" + error_message;
    }
    message += "，目标=" + channel_target_description(channel_config, "未配置串口设备");
    Logger::debug(message);
}

}  // namespace

// 绑定通道管理器和通讯追踪存储。
ModbusWriteExecutor::ModbusWriteExecutor(
    ChannelManager* channel_manager,
    CommunicationTraceStore* communication_trace_store)
    : channel_manager_(channel_manager),
      communication_trace_store_(communication_trace_store)
{
}

// 对指定主站执行一次写多个保持寄存器。
ModbusWriteMultipleRegistersResult ModbusWriteExecutor::write_multiple_holding_registers(
    const MasterNodeConfig& master_config,
    std::uint16_t start_register,
    const std::vector<std::uint16_t>& values)
{
    // 校验运行依赖、写入数量和目标通道。
    ModbusWriteMultipleRegistersResult result;
    const auto started_at_ms = time_utils::system_now_ms();
    result.timestamp_ms = started_at_ms;
    result.start_register = start_register;
    result.register_count = static_cast<std::uint16_t>(values.size());

    if (channel_manager_ == nullptr) {
        result.transport_status = StatusCode::kInvalidState;
        result.diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result.error_message = "通道管理器未初始化";
        return result;
    }

    if (values.empty() || values.size() > ModbusRtuProtocol::kMaxWriteMultipleRegisterCount) {
        result.transport_status = StatusCode::kInvalidArgument;
        result.diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result.error_message = "单次写入保持寄存器数量超出范围，允许范围为 1-123";
        return result;
    }

    auto* channel = channel_manager_->get_channel(master_config.channel_id);
    if (channel == nullptr) {
        result.transport_status = StatusCode::kNotFound;
        result.diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result.error_message = "未找到通道: " + master_config.channel_id;
        return result;
    }

    const auto& channel_config = channel->config();
    const auto timeout_ms = static_cast<int>(
        std::max<std::uint32_t>(
            master_config.response_timeout_ms > 0 ? master_config.response_timeout_ms : channel_config.response_timeout_ms,
            1));

    // TCP 主站委托 TCP 客户端执行，并统一转换通讯追踪信息。
    if (master_config.protocol == MasterProtocol::kModbusTcp) {
        ModbusTcpClient tcp_client(channel);
        ModbusWriteMultipleRegistersResult tcp_result;
        (void)tcp_client.write_multiple_holding_registers(master_config, start_register, values, &tcp_result);
        result = std::move(tcp_result);
        append_modbus_trace(
            communication_trace_store_,
            master_config,
            ModbusTcpProtocol::kWriteMultipleHoldingRegistersFunction,
            start_register,
            result.register_count,
            result.request_frame,
            result.response_frame,
            started_at_ms,
            result.transport_status,
            result.error_message);
        log_write_result(
            master_config,
            channel_config,
            start_register,
            result.register_count,
            result.success,
            result.error_message);
        return result;
    }

    if (master_config.protocol != MasterProtocol::kModbusRtu) {
        result.transport_status = StatusCode::kInvalidArgument;
        result.diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result.error_message = "不支持的 Modbus 协议类型";
        return result;
    }

    // RTU 主站先构造请求帧，再通过通道完成收发。
    const auto slave_address = static_cast<std::uint8_t>(master_config.target_address);
    result.request_frame = ModbusRtuProtocol::build_write_multiple_holding_registers_request(
        slave_address,
        start_register,
        values);
    if (result.request_frame.empty()) {
        result.transport_status = StatusCode::kInvalidArgument;
        result.diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result.error_message = "构造 Modbus 10 请求失败，寄存器数量无效";
        return result;
    }

    ChannelTraceContext trace_context;
    trace_context.channel_id = master_config.channel_id;
    trace_context.channel_name = channel_config.channel_name;
    trace_context.master_id = master_config.master_id;
    trace_context.master_name = master_config.master_name;
    trace_context.target = channel_target_description(channel_config, "未配置串口设备");

    result.transport_status = channel->transceive(
        result.request_frame,
        timeout_ms,
        &result.response_frame,
        trace_context);
    result.timestamp_ms = time_utils::system_now_ms();
    if (!is_ok(result.transport_status)) {
        const auto channel_status = channel->status();
        result.error_message = channel_status.last_error_message.empty()
                                   ? "通道响应超时"
                                   : channel_status.last_error_message;
        result.diagnosis_error_code = diagnosis_from_rtu_channel_status(
            channel_status,
            result.error_message,
            result.transport_status);
        append_modbus_trace(
            communication_trace_store_,
            master_config,
            ModbusRtuProtocol::kWriteMultipleHoldingRegistersFunction,
            start_register,
            result.register_count,
            result.request_frame,
            result.response_frame,
            started_at_ms,
            result.transport_status,
            result.error_message);
        log_write_result(
            master_config,
            channel_config,
            start_register,
            result.register_count,
            false,
            result.error_message);
        return result;
    }

    // 校验响应帧，并生成最终写入结果和诊断信息。
    std::string parse_error;
    const auto parse_status = ModbusRtuProtocol::parse_write_multiple_holding_registers_response(
        slave_address,
        start_register,
        result.register_count,
        result.response_frame,
        &parse_error);
    if (!is_ok(parse_status)) {
        result.transport_status = parse_status;
        result.error_message = parse_error;
        result.diagnosis_error_code = classify_modbus_error(parse_error);
        append_modbus_trace(
            communication_trace_store_,
            master_config,
            ModbusRtuProtocol::kWriteMultipleHoldingRegistersFunction,
            start_register,
            result.register_count,
            result.request_frame,
            result.response_frame,
            started_at_ms,
            result.transport_status,
            result.error_message);
        log_write_result(
            master_config,
            channel_config,
            start_register,
            result.register_count,
            false,
            result.error_message);
        return result;
    }

    result.success = true;
    result.transport_status = StatusCode::kOk;
    result.diagnosis_error_code = DiagnosisErrorCode::kNone;
    result.timestamp_ms = time_utils::system_now_ms();
    append_modbus_trace(
        communication_trace_store_,
        master_config,
        ModbusRtuProtocol::kWriteMultipleHoldingRegistersFunction,
        start_register,
        result.register_count,
        result.request_frame,
        result.response_frame,
        started_at_ms,
        result.transport_status,
        "");
    log_write_result(
        master_config,
        channel_config,
        start_register,
        result.register_count,
        true,
        "");
    return result;
}

}  // namespace edge_controller
