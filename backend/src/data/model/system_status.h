// 汇总系统、通道、主站、设备和轮询周期的当前运行状态。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "shared/common/types.h"
#include "data/model/channel_status.h"
#include "data/model/diagnosis_status.h"
#include "data/model/device_status.h"
#include "data/model/master_node_status.h"

namespace edge_controller {

// 保存当前后端运行期状态，便于诊断和服务层读取。

struct SystemStatus {
    bool config_loaded{false};
    bool service_ready{false};
    bool running{false};
    bool polling_running{false};
    std::string polling_state{"not_started"};
    TimestampMs started_at_ms{0};
    TimestampMs stopped_at_ms{0};
    TimestampMs last_heartbeat_ms{0};
    TimestampMs last_poll_cycle_started_at_ms{0};
    TimestampMs last_poll_cycle_finished_at_ms{0};
    std::size_t online_channel_count{0};
    std::size_t online_master_count{0};
    std::size_t online_device_count{0};
    std::size_t last_poll_cycle_master_count{0};
    std::size_t last_poll_cycle_success_master_count{0};
    std::size_t last_poll_cycle_failed_master_count{0};
    std::size_t last_poll_cycle_success_device_count{0};
    std::size_t last_poll_cycle_failed_device_count{0};
    bool last_poll_cycle_has_error{false};
    DiagnosisStatus diagnosis;
    std::string last_status_message;
    std::string last_poll_cycle_error_message;

    std::vector<ChannelStatus> channel_status_list;
    std::vector<MasterNodeStatus> master_status_list;
    std::vector<DeviceStatus> device_status_list;
};

}  // namespace edge_controller
