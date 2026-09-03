// 描述主站最近轮询、成功失败计数、寄存器块和诊断状态。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shared/common/enums.h"
#include "shared/common/types.h"
#include "data/model/diagnosis_status.h"

namespace edge_controller {

// 保存总控的运行状态；last_register_block 仅保留为旧接口字段，多区块采集不再填充它。
struct MasterNodeStatus {
    MasterNodeId master_id;
    bool online{false};
    // 表示本轮所有读取区块是否均通讯成功。
    bool last_collect_success{false};
    DataQuality communication_quality{DataQuality::kUnknown};
    TimestampMs last_poll_time_ms{0};
    TimestampMs last_success_time_ms{0};
    TimestampMs last_failure_time_ms{0};
    std::uint32_t last_cycle_duration_ms{0};
    std::uint32_t consecutive_failure_count{0};
    std::uint32_t consecutive_timeout_count{0};
    std::vector<std::uint16_t> last_register_block;
    DiagnosisStatus diagnosis;
    std::string last_error_message;
};

}  // namespace edge_controller
