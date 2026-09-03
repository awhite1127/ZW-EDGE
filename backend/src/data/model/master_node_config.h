// 主站协议、地址、轮询块和设备模板引用配置模型。
#pragma once

#include <cstdint>
#include <string>

#include "shared/common/enums.h"
#include "shared/common/types.h"

namespace edge_controller {

inline constexpr std::uint32_t kMaxDevicesPerMaster = 256;

struct MasterNodeConfig {
    MasterNodeId master_id;
    std::string master_name;
    bool enabled{true};
    MasterProtocol protocol{MasterProtocol::kModbusRtu};
    ChannelId channel_id;
    std::uint16_t target_address{1};
    std::uint32_t poll_interval_ms{1000};
    std::uint32_t retry_count{2};
    std::string remark;
    std::string device_template;
    RegisterAddress block_start_register{0};
    // 运行时设备生成和轮询次数的唯一权威来源。
    std::uint32_t device_count{1};
    std::uint32_t response_timeout_ms{0};
};

}  // namespace edge_controller
