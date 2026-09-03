// 有界 Modbus 报文诊断记录器接口。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shared/common/status_code.h"
#include "shared/common/types.h"
#include "data/datastore/communication_store.h"
#include "data/model/master_node_config.h"

namespace edge_controller {

// 将通讯结果状态码转换为稳定文本。
std::string communication_trace_result(StatusCode status);

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
    std::int64_t device_index = -1,
    const std::string& device_id = {},
    const std::string& block_key = {},
    const std::string& block_display_name = {});

}  // namespace edge_controller
