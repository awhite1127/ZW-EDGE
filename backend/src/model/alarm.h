// 告警规则、活动告警和规则维护结果模型。
#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"

namespace edge_controller {

struct AlarmRule {
    DeviceId device_id;
    std::string point_key;
    bool enabled{false};
    bool high_enabled{false};
    double high_threshold{0.0};
    bool low_enabled{false};
    double low_threshold{0.0};
    std::string level{"warning"};
    double hysteresis{0.0};
    std::uint32_t trigger_count{1};
    std::uint32_t recovery_count{1};
    TimestampMs updated_at_ms{0};
};

struct AlarmRuntimeState {
    DeviceId device_id;
    std::string point_key;
    std::string state{"normal"};
    std::string direction{"none"};
    double current_value{0.0};
    double threshold_value{0.0};
    std::uint32_t consecutive_trigger_count{0};
    std::uint32_t consecutive_recovery_count{0};
    TimestampMs active_since_ms{0};
    TimestampMs last_evaluated_at_ms{0};
    bool acknowledged{false};
    TimestampMs acknowledged_at_ms{0};
    std::string acknowledged_by;
    TimestampMs updated_at_ms{0};
};

struct ActiveAlarmView {
    DeviceId device_id;
    std::string device_name;
    MasterNodeId master_id;
    std::string template_id;
    std::string point_key;
    std::string point_name;
    std::string unit;
    std::uint32_t precision{0};
    std::string direction{"none"};
    std::string level{"warning"};
    double current_value{0.0};
    double threshold_value{0.0};
    TimestampMs active_since_ms{0};
    TimestampMs last_evaluated_at_ms{0};
    bool acknowledged{false};
    TimestampMs acknowledged_at_ms{0};
    std::string acknowledged_by;
};

}  // namespace edge_controller
