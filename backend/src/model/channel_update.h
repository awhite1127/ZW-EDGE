// 定义通道新增、更新、删除请求及配置应用结果。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstdint>
#include <string>

#include "common/enums.h"
#include "common/types.h"
#include "model/channel_config.h"

namespace edge_controller {

// ChannelConfigUpdateRequest 描述单个通道的可编辑配置输入。
struct ChannelConfigUpdateRequest {
    ChannelId channel_id;
    std::string channel_name;
    bool enabled{true};
    ChannelType channel_type{ChannelType::kModbusRtuSerial};
    std::string port_name;
    std::uint32_t baud_rate{9600};
    std::uint8_t data_bits{8};
    SerialParity parity{SerialParity::kNone};
    std::uint8_t stop_bits{1};
    std::uint32_t response_timeout_ms{1000};
    std::uint32_t retry_count{2};
    std::string tcp_host;
    std::uint16_t tcp_port{502};
    std::uint32_t connect_timeout_ms{3000};
};

// ChannelConfigUpdateResult 描述单个通道配置保存后的结果。
struct ChannelConfigUpdateResult {
    ChannelId channel_id;
    ChannelConfig channel_config;
    std::string message;
    std::string warning_message;
    bool polling_restarted{false};
};

struct ChannelConfigDeleteResult {
    ChannelId channel_id;
    std::string message;
    bool polling_restarted{false};
};

}  // namespace edge_controller
