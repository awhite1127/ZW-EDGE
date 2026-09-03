// 描述通道打开、收发时间、连续错误和诊断信息等当前运行状态。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstdint>
#include <string>

#include "shared/common/types.h"
#include "data/model/diagnosis_status.h"

namespace edge_controller {

// 描述一个通信通道的运行时连接状态。
struct ChannelStatus {
    ChannelId channel_id;
    bool configured{false};
    bool enabled{false};
    bool opened{false};
    std::string status;
    std::string device_path;
    TimestampMs last_open_time_ms{0};
    TimestampMs last_close_time_ms{0};
    TimestampMs last_send_time_ms{0};
    TimestampMs last_receive_time_ms{0};
    TimestampMs last_change_time_ms{0};
    std::uint32_t consecutive_error_count{0};
    DiagnosisStatus diagnosis;
    std::string last_error_message;
};

}  // namespace edge_controller
