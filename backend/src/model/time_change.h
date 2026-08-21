// 系统时间调整信息及其历史/MQTT 联动结果模型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/types.h"

namespace edge_controller {

inline constexpr std::int64_t kSignificantTimeChangeThresholdMs = 60LL * 1000LL;
inline constexpr TimestampMs kMinimumValidSystemTimeMs = 1577836800000ULL;  // 2020-01-01T00:00:00Z

struct TimeAdjustmentInfo {
    std::string source;
    TimestampMs before_time_ms{0};
    TimestampMs after_time_ms{0};
    std::int64_t delta_ms{0};
    TimestampMs detected_at_ms{0};
    std::string reason;
    bool significant{false};
};

struct MqttTimeAdjustmentResult {
    bool enabled{false};
    bool tls_enabled{false};
    bool reconnect_requested{false};
    std::string detail;
};

struct TimeAdjustmentHandlingResult {
    bool history_state_reset{false};
    std::size_t discarded_pending_history_records{0};
    bool event_deduplication_reset{false};
    std::string event_deduplication_error;
    MqttTimeAdjustmentResult mqtt;
};

}  // namespace edge_controller
