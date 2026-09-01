#include "model/alarm_validation.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

using edge_controller::AlarmRule;
using edge_controller::StatusCode;
using edge_controller::is_ok;
using edge_controller::validate_alarm_rule;

bool expect(bool condition, const std::string& message)
{
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

AlarmRule valid_rule()
{
    AlarmRule rule;
    rule.device_id = "device-1";
    rule.point_key = "temperature";
    rule.enabled = true;
    rule.high_enabled = true;
    rule.high_threshold = 80.0;
    rule.low_enabled = true;
    rule.low_threshold = 10.0;
    rule.level = "warning";
    rule.hysteresis = 2.0;
    rule.trigger_count = 2;
    rule.recovery_count = 3;
    return rule;
}

bool rejects(AlarmRule rule, const std::string& expected_fragment)
{
    std::string error;
    const auto status = validate_alarm_rule(rule, &error);
    return expect(status == StatusCode::kInvalidArgument, "invalid alarm rule was accepted") &&
           expect(error.find(expected_fragment) != std::string::npos,
                  "unexpected alarm validation error: " + error);
}

}  // namespace

int main()
{
    bool passed = true;
    std::string error;
    passed &= expect(is_ok(validate_alarm_rule(valid_rule(), &error)),
                     "valid alarm rule was rejected: " + error);

    auto rule = valid_rule();
    rule.trigger_count = 0;
    passed &= rejects(rule, "连续次数");
    rule = valid_rule();
    rule.hysteresis = -1.0;
    passed &= rejects(rule, "回差");
    rule = valid_rule();
    rule.enabled = true;
    rule.high_enabled = false;
    rule.low_enabled = false;
    passed &= rejects(rule, "至少需要启用");
    rule = valid_rule();
    rule.low_threshold = 80.0;
    passed &= rejects(rule, "下限必须小于上限");
    rule = valid_rule();
    rule.hysteresis = 70.0;
    passed &= rejects(rule, "回差必须小于");
    rule = valid_rule();
    rule.high_threshold = std::numeric_limits<double>::infinity();
    passed &= rejects(rule, "有限数");
    rule = valid_rule();
    rule.level = "critical";
    passed &= rejects(rule, "告警级别");
    return passed ? 0 : 1;
}
