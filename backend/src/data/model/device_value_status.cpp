// 设备原始数值状态码注册表：新增协议状态值时集中扩展此处。
#include "data/model/device_value_status.h"

#include <array>
#include <cmath>

namespace edge_controller {

namespace {

struct DeviceValueStatusRule {
    DeviceValueStatusScope scope{DeviceValueStatusScope::kNone};
    DeviceValueStatusCode status_code{DeviceValueStatusCode::kNone};
    double raw_value{0.0};
    DiagnosisErrorCode error_code{DiagnosisErrorCode::kNone};
    DiagnosisRunStatus run_status{DiagnosisRunStatus::kNormal};
    int priority{0};
};

// 状态规则在 scale/value_offset 之前匹配原始值。当前只登记内置 uint16 双字节规则：
// 0xFFFF=设备故障，0xFFFE=开路。后续新增状态值只需追加规则并补充测试。
constexpr std::array<DeviceValueStatusRule, 2> kDeviceValueStatusRules{{
    {DeviceValueStatusScope::kBuiltinUint16,
     DeviceValueStatusCode::kBuiltinUint16DeviceFault,
     65535.0,
     DiagnosisErrorCode::kDeviceFault,
     DiagnosisRunStatus::kError,
     20},
    {DeviceValueStatusScope::kBuiltinUint16,
     DeviceValueStatusCode::kBuiltinUint16DeviceOpenCircuit,
     65534.0,
     DiagnosisErrorCode::kDeviceOpenCircuit,
     DiagnosisRunStatus::kError,
     10},
}};

}  // namespace

const char* to_string(DeviceValueStatusCode status_code)
{
    switch (status_code) {
    case DeviceValueStatusCode::kBuiltinUint16DeviceFault:
        return "BUILTIN_UINT16_DEVICE_FAULT";
    case DeviceValueStatusCode::kBuiltinUint16DeviceOpenCircuit:
        return "BUILTIN_UINT16_DEVICE_OPEN_CIRCUIT";
    case DeviceValueStatusCode::kNone:
    default:
        return "NONE";
    }
}

DeviceValueStatusMatch classify_device_value_status(
    DeviceValueStatusScope scope,
    double raw_value)
{
    if (scope == DeviceValueStatusScope::kNone || !std::isfinite(raw_value)) {
        return {};
    }
    for (const auto& rule : kDeviceValueStatusRules) {
        if (scope == rule.scope && raw_value == rule.raw_value) {
            return {rule.status_code, rule.error_code, rule.run_status};
        }
    }
    return {};
}

bool is_device_value_status_error_code(DiagnosisErrorCode error_code)
{
    for (const auto& rule : kDeviceValueStatusRules) {
        if (rule.error_code == error_code) {
            return true;
        }
    }
    return false;
}

bool is_device_value_status_error_code(const std::string& error_code)
{
    for (const auto& rule : kDeviceValueStatusRules) {
        if (error_code == diagnosis_definition(rule.error_code).code) {
            return true;
        }
    }
    return false;
}

int device_value_status_priority(DeviceValueStatusCode status_code)
{
    for (const auto& rule : kDeviceValueStatusRules) {
        if (rule.status_code == status_code) {
            return rule.priority;
        }
    }
    return 0;
}

}  // namespace edge_controller
