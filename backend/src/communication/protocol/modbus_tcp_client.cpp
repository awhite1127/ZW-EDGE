#include "communication/collect/transport_diagnosis.h"
// 通过 TCP 通道执行带事务号的 Modbus 请求响应。
// 边界：严格校验帧边界与响应一致性，不猜测修复异常报文。

#include "communication/protocol/modbus_tcp_client.h"

#include <algorithm>
#include <atomic>
#include <string>

#include "shared/common/time_utils.h"
#include "communication/protocol/modbus_tcp_protocol.h"

namespace edge_controller {

namespace {

constexpr std::uint32_t kTransactionIdModulo = 0xFFFFU;

// 根据通道状态生成诊断错误码。
DiagnosisErrorCode diagnosis_from_channel_status(
    const ChannelStatus& channel_status,
    const std::string& error_message,
    StatusCode transport_status)
{
    (void)error_message;
    return transport_diagnosis(transport_status, channel_status, true);
}

}  // namespace

ModbusTcpClient::ModbusTcpClient(
    IChannel* channel,
    const std::atomic<bool>* cancel_requested)
    : channel_(channel),
      cancel_requested_(cancel_requested)
{
}

// 按指定起始地址、数量和 FC03/FC04 读取连续寄存器。
StatusCode ModbusTcpClient::read_registers(
    const MasterNodeConfig& master_config,
    std::uint8_t function_code,
    std::uint16_t start_register,
    std::uint16_t register_count,
    ModbusReadResult* result)
{
    if (result == nullptr) {
        return StatusCode::kInvalidArgument;
    }

    *result = {};
    result->timestamp_ms = time_utils::system_now_ms();

    if (channel_ == nullptr) {
        result->error_message = "TCP 通道未初始化";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidState;
        return StatusCode::kInvalidState;
    }

    if (function_code != ModbusTcpProtocol::kReadHoldingRegistersFunction &&
        function_code != ModbusTcpProtocol::kReadInputRegistersFunction) {
        result->error_message = "读取寄存器功能码只支持 FC03 或 FC04";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidArgument;
        return StatusCode::kInvalidArgument;
    }
    if (register_count == 0 ||
        register_count > ModbusTcpProtocol::kMaxReadRegisterCount) {
        result->error_message = "单次读取寄存器数量超出范围，允许范围为 1-125";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidArgument;
        return StatusCode::kInvalidArgument;
    }

    const auto transaction_id = next_transaction_id();
    const auto unit_id = static_cast<std::uint8_t>(master_config.target_address);
    result->request_frame = ModbusTcpProtocol::build_read_registers_request(
        transaction_id,
        unit_id,
        function_code,
        start_register,
        register_count);
    if (result->request_frame.empty()) {
        result->error_message =
            "构造 Modbus TCP FC" + std::to_string(function_code) + " 请求失败，参数无效";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidArgument;
        return StatusCode::kInvalidArgument;
    }

    const auto& channel_config = channel_->config();
    const auto timeout_ms = static_cast<int>(
        std::max<std::uint32_t>(
            master_config.response_timeout_ms > 0 ? master_config.response_timeout_ms : channel_config.response_timeout_ms,
            1));

    std::string parse_error;
    bool response_validated = false;
    result->transport_status = channel_->transceive(
        result->request_frame,
        timeout_ms,
        &result->response_frame,
        {},
        cancel_requested_,
        [&](const std::vector<std::uint8_t>& response, bool* connection_reusable) {
            response_validated = true;
            return ModbusTcpProtocol::parse_read_registers_response(
                transaction_id,
                unit_id,
                function_code,
                register_count,
                response,
                &result->registers,
                &parse_error,
                connection_reusable, &result->diagnosis_error_code);
        });
    result->timestamp_ms = time_utils::system_now_ms();
    if (!is_ok(result->transport_status)) {
        if (response_validated) {
            result->error_message = parse_error;

            return result->transport_status;
        }
        const auto channel_status = channel_->status();
        result->error_message = channel_status.last_error_message.empty()
                                    ? "TCP 响应超时"
                                    : channel_status.last_error_message;
        result->diagnosis_error_code = diagnosis_from_channel_status(
            channel_status,
            result->error_message,
            result->transport_status);
        return result->transport_status;
    }

    result->success = true;
    result->diagnosis_error_code = DiagnosisErrorCode::kNone;
    result->transport_status = StatusCode::kOk;
    return StatusCode::kOk;
}

// 按指定起始地址和数量读取保持寄存器。
StatusCode ModbusTcpClient::read_holding_registers(
    const MasterNodeConfig& master_config,
    std::uint16_t start_register,
    std::uint16_t register_count,
    ModbusReadResult* result)
{
    return read_registers(
        master_config,
        ModbusTcpProtocol::kReadHoldingRegistersFunction,
        start_register,
        register_count,
        result);
}

// 写入多个保持寄存器。
StatusCode ModbusTcpClient::write_multiple_holding_registers(
    const MasterNodeConfig& master_config,
    std::uint16_t start_register,
    const std::vector<std::uint16_t>& values,
    ModbusWriteMultipleRegistersResult* result)
{
    if (result == nullptr) {
        return StatusCode::kInvalidArgument;
    }

    *result = {};
    result->timestamp_ms = time_utils::system_now_ms();
    result->start_register = start_register;
    result->register_count = static_cast<std::uint16_t>(values.size());

    if (channel_ == nullptr) {
        result->error_message = "TCP 通道未初始化";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidState;
        return StatusCode::kInvalidState;
    }

    if (values.empty() || values.size() > ModbusTcpProtocol::kMaxWriteMultipleRegisterCount) {
        result->error_message = "单次写入保持寄存器数量超出范围，允许范围为 1-123";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidArgument;
        return StatusCode::kInvalidArgument;
    }

    const auto transaction_id = next_transaction_id();
    const auto unit_id = static_cast<std::uint8_t>(master_config.target_address);
    result->request_frame = ModbusTcpProtocol::build_write_multiple_holding_registers_request(
        transaction_id,
        unit_id,
        start_register,
        values);
    if (result->request_frame.empty()) {
        result->error_message = "构造 Modbus TCP 10 请求失败，寄存器数量无效";
        result->diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
        result->transport_status = StatusCode::kInvalidArgument;
        return StatusCode::kInvalidArgument;
    }

    const auto& channel_config = channel_->config();
    const auto timeout_ms = static_cast<int>(
        std::max<std::uint32_t>(
            master_config.response_timeout_ms > 0 ? master_config.response_timeout_ms : channel_config.response_timeout_ms,
            1));

    std::string parse_error;
    bool response_validated = false;
    result->transport_status = channel_->transceive(
        result->request_frame,
        timeout_ms,
        &result->response_frame,
        {},
        cancel_requested_,
        [&](const std::vector<std::uint8_t>& response, bool* connection_reusable) {
            response_validated = true;
            return ModbusTcpProtocol::parse_write_multiple_holding_registers_response(
                transaction_id,
                unit_id,
                start_register,
                result->register_count,
                response,
                &parse_error,
                connection_reusable, &result->diagnosis_error_code);
        });
    result->timestamp_ms = time_utils::system_now_ms();
    if (!is_ok(result->transport_status)) {
        if (response_validated) {
            result->error_message = parse_error;

            return result->transport_status;
        }
        const auto channel_status = channel_->status();
        result->error_message = channel_status.last_error_message.empty()
                                    ? "TCP 响应超时"
                                    : channel_status.last_error_message;
        result->diagnosis_error_code = diagnosis_from_channel_status(
            channel_status,
            result->error_message,
            result->transport_status);
        return result->transport_status;
    }

    result->success = true;
    result->diagnosis_error_code = DiagnosisErrorCode::kNone;
    result->transport_status = StatusCode::kOk;
    return StatusCode::kOk;
}

// 生成下一个 Modbus TCP 事务号。
std::uint16_t ModbusTcpClient::next_transaction_id()
{
    static std::atomic<std::uint32_t> counter{0};
    const auto value = counter.fetch_add(1U, std::memory_order_relaxed);
    return static_cast<std::uint16_t>((value % kTransactionIdModulo) + 1U);
}

}  // namespace edge_controller
