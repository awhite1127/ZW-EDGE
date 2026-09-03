// 设备原始数值状态码识别：把协议约定的哨兵值映射为统一诊断码。
#pragma once

#include "data/model/diagnosis_status.h"

namespace edge_controller {

// 数值状态规则族明确限定数据来源；尚未取得规则的数据宽度不能复用已有状态码。
enum class DeviceValueStatusScope {
    kNone,
    kBuiltinUint16,
};

// 数值状态自身使用带数据宽度的稳定标识，诊断层仍复用通用设备故障语义。
enum class DeviceValueStatusCode {
    kNone,
    kBuiltinUint16DeviceFault,
    kBuiltinUint16DeviceOpenCircuit,
};

struct DeviceValueStatusMatch {
    DeviceValueStatusCode status_code{DeviceValueStatusCode::kNone};
    DiagnosisErrorCode error_code{DiagnosisErrorCode::kNone};
    DiagnosisRunStatus run_status{DiagnosisRunStatus::kNormal};

    bool matched() const
    {
        return status_code != DeviceValueStatusCode::kNone;
    }
};

// 返回数值状态的稳定标识。
const char* to_string(DeviceValueStatusCode status_code);

// 对已启用规则族的原始值直接判定；命中表示该值无效并带有明确状态语义，
// 规则族不匹配或未命中表示本模块不否定该值的有效性。
DeviceValueStatusMatch classify_device_value_status(
    DeviceValueStatusScope scope,
    double raw_value);

// 判断诊断码是否来自设备原始数值状态识别模块。
bool is_device_value_status_error_code(DiagnosisErrorCode error_code);

// 判断稳定字符串诊断码是否来自设备原始数值状态识别模块。
bool is_device_value_status_error_code(const std::string& error_code);

// 设备同时出现多个数值状态时，返回用于选择设备级主诊断的优先级。
int device_value_status_priority(DeviceValueStatusCode status_code);

}  // namespace edge_controller
