// 告警规则领域校验的唯一实现。

#include "data/model/alarm_validation.h"

#include <cmath>

namespace edge_controller {
namespace {

StatusCode invalid_rule(std::string* error_message, const char* message)
{
    if (error_message != nullptr) {
        *error_message = message;
    }
    return StatusCode::kInvalidArgument;
}

}  // namespace

StatusCode validate_alarm_rule(const AlarmRule& rule, std::string* error_message)
{
    if (rule.device_id.empty() || rule.point_key.empty()) {
        return invalid_rule(error_message, "告警规则的 device_id 和 point_key 不能为空");
    }
    if (rule.level != "warning" && rule.level != "error") {
        return invalid_rule(error_message, "告警级别只能是 warning 或 error");
    }
    if (rule.trigger_count < 1 || rule.recovery_count < 1) {
        return invalid_rule(error_message, "告警触发和恢复连续次数必须大于 0");
    }
    if (!std::isfinite(rule.hysteresis) || rule.hysteresis < 0.0) {
        return invalid_rule(error_message, "告警回差必须为有限非负数");
    }
    if (rule.enabled && !rule.high_enabled && !rule.low_enabled) {
        return invalid_rule(error_message, "启用告警规则时至少需要启用上限或下限");
    }
    if (rule.high_enabled && !std::isfinite(rule.high_threshold)) {
        return invalid_rule(error_message, "启用上限时上限阈值必须为有限数");
    }
    if (rule.low_enabled && !std::isfinite(rule.low_threshold)) {
        return invalid_rule(error_message, "启用下限时下限阈值必须为有限数");
    }
    if (rule.high_enabled && rule.low_enabled) {
        const auto threshold_span = rule.high_threshold - rule.low_threshold;
        if (threshold_span <= 0.0) {
            return invalid_rule(error_message, "同时启用上下限时，下限必须小于上限");
        }
        if (rule.hysteresis >= threshold_span) {
            return invalid_rule(error_message, "同时启用上下限时，回差必须小于上下限差值");
        }
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
