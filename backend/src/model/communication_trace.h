// Modbus 请求/响应报文追踪及分页查询模型。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

namespace edge_controller {

struct CommunicationTraceRecord {
    std::uint64_t sequence{0};
    TimestampMs timestamp_ms{0};
    std::string channel_id;
    std::string master_id;
    // 周期采集报文使用零基设备序号；手动命令等非设备区块请求为 -1。
    std::int64_t device_index{-1};
    std::string device_id;
    std::string block_key;
    std::string block_display_name;
    std::string protocol{"modbus_rtu"};

    std::uint32_t slave_address{0};
    std::uint32_t function_code{0};
    std::uint32_t start_register{0};
    std::uint32_t register_count{0};

    std::string request_hex;
    std::string response_hex;
    std::uint32_t elapsed_ms{0};

    std::string result;
    std::string error_message;
};

struct ChannelCommunicationTraces {
    ChannelId channel_id;
    std::vector<CommunicationTraceRecord> records;
};

}  // namespace edge_controller
