// Modbus 报文记录器截取有限长度的请求/响应十六进制文本，用于现场诊断而非协议重放。
#include "communication/collect/modbus_trace_recorder.h"

#include <exception>
#include <utility>

#include "shared/common/enums.h"
#include "shared/common/format_utils.h"
#include "shared/common/logger.h"
#include "shared/common/time_utils.h"
#include "data/model/communication_trace.h"

namespace edge_controller {

// 将通讯结果状态码转换为稳定文本。
std::string communication_trace_result(StatusCode status)
{
    switch (status) {
    case StatusCode::kOk:
        return "success";
    case StatusCode::kTimeout:
        return "timeout";
    case StatusCode::kIoError:
        return "io_error";
    case StatusCode::kProtocolError:
        return "protocol_error";
    case StatusCode::kInvalidArgument:
    case StatusCode::kInvalidState:
    case StatusCode::kNotFound:
    case StatusCode::kInternalError:
    case StatusCode::kConflict:
        return "unknown_error";
    }
    return "unknown_error";
}

// 追加一条 Modbus 通讯报文记录。
void append_modbus_trace(
    CommunicationTraceStore* store,
    const MasterNodeConfig& master_config,
    std::uint32_t function_code,
    std::uint32_t start_register,
    std::uint32_t register_count,
    const std::vector<std::uint8_t>& request_frame,
    const std::vector<std::uint8_t>& response_frame,
    TimestampMs started_at_ms,
    StatusCode status,
    const std::string& error_message,
    std::int64_t device_index,
    const std::string& device_id,
    const std::string& block_key,
    const std::string& block_display_name)
{
    if (store == nullptr) {
        return;
    }

    try {
        const auto finished_at_ms = time_utils::system_now_ms();
        CommunicationTraceRecord record;
        record.timestamp_ms = finished_at_ms;
        record.channel_id = master_config.channel_id;
        record.master_id = master_config.master_id;
        record.device_index = device_index;
        record.device_id = device_id;
        record.block_key = block_key;
        record.block_display_name = block_display_name;
        record.protocol = to_string(master_config.protocol);
        record.slave_address = master_config.target_address;
        record.function_code = function_code;
        record.start_register = start_register;
        record.register_count = register_count;
        record.request_hex = format_utils::bytes_to_hex(request_frame);
        record.response_hex = format_utils::bytes_to_hex(response_frame);
        record.elapsed_ms = finished_at_ms > started_at_ms
                                ? static_cast<std::uint32_t>(finished_at_ms - started_at_ms)
                                : 0U;
        record.result = communication_trace_result(status);
        record.error_message = error_message;

        store->append(std::move(record));
    } catch (const std::exception& error) {
        Logger::warn("记录 Modbus 通讯报文失败，已跳过：" + std::string(error.what()));
    } catch (...) {
        Logger::warn("记录 Modbus 通讯报文发生未知异常，已跳过");
    }
}

}  // namespace edge_controller
