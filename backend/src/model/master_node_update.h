// 主站配置新增/更新及配置应用结果模型。
#pragma once

#include <cstdint>
#include <string>

#include "model/master_node_config.h"

namespace edge_controller {

struct MasterNodeConfigUpdateRequest {
    MasterNodeId master_id;
    std::string master_name;
    bool enabled{true};
    MasterProtocol protocol{MasterProtocol::kModbusRtu};
    ChannelId channel_id;
    std::uint16_t target_address{1};
    std::uint32_t poll_interval_ms{1000};
    std::uint32_t response_timeout_ms{1000};
    std::uint32_t retry_count{2};
    std::string remark;
    std::string device_template;
    RegisterAddress block_start_register{0};
    // 运行时设备生成和轮询次数的唯一权威来源。
    std::uint32_t device_count{1};
};

struct MasterNodeConfigUpdateResult {
    MasterNodeId master_id;
    MasterNodeConfig master_config;
    std::string message;
    std::string warning_message;
    bool polling_restarted{false};
};

struct MasterNodeConfigDeleteResult {
    MasterNodeId master_id;
    std::string message;
    bool polling_restarted{false};
};

}  // namespace edge_controller
