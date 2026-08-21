// 描述单设备在线、质量、采集时间、数据项和诊断信息等运行状态。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <string>
#include <vector>

#include "common/enums.h"
#include "common/types.h"
#include "model/diagnosis_status.h"
#include "model/point_value.h"

namespace edge_controller {

// 保存一个逻辑设备最近一次解码后的状态和值。

struct DeviceStatus {
    DeviceId device_id;
    std::string device_name;
    MasterNodeId master_id;
    std::string template_id;
    std::string template_name;
    bool online{false};
    // 仅表示本轮所有读取区块是否均通讯成功，不包含字段值有效性。
    bool last_collect_success{false};
    // 仅表示本轮读取区块的 Modbus 通讯质量；字段值有效性由 PointValue 独立表达。
    DataQuality communication_quality{DataQuality::kUnknown};
    TimestampMs updated_at_ms{0};
    TimestampMs last_success_time_ms{0};
    TimestampMs last_failure_time_ms{0};

    bool has_resistance{false};
    double resistance_value{0.0};
    std::vector<PointValue> points;

    DiagnosisStatus diagnosis;
    std::string last_error_message;
};

}  // namespace edge_controller
