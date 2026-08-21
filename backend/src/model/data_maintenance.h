// 描述历史分层保留期限、最近清理结果和各类记录数量。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"

namespace edge_controller {

struct DataMaintenanceSummary {
    std::uint32_t raw_10min_retention_hours{72};
    std::uint32_t hour_retention_days{35};
    std::uint32_t day_retention_days{30};
    std::uint32_t event_retention_days{30};
    // 日志按大小与备份数轮转，不承诺固定天数；0 表示“不适用天数口径”。
    std::uint32_t log_retention_days{0};
    std::string log_rotation_policy{"按文件大小轮转，由 edge-log-rotator systemd 服务管理"};
    TimestampMs last_cleanup_time_ms{0};
    bool last_cleanup_success{false};
    std::string last_cleanup_result{"尚未执行"};
    std::uint64_t deleted_raw_10min_count{0};
    std::uint64_t deleted_hour_count{0};
    std::uint64_t deleted_day_count{0};
    std::uint64_t deleted_event_count{0};
    std::uint64_t current_raw_10min_count{0};
    std::uint64_t current_hour_count{0};
    std::uint64_t current_day_count{0};
    std::uint64_t current_event_count{0};
};

}  // namespace edge_controller
