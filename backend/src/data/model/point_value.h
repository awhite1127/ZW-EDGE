// 描述模板数据项的数值、原始值、单位、精度、质量和有效性。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstdint>
#include <string>

#include "shared/common/enums.h"
#include "shared/common/types.h"

namespace edge_controller {

// 用于存储、诊断或对外输出的简单点位快照。
struct PointValue {
    std::string key;
    std::string name;
    double value{0.0};
    std::string unit;
    std::uint32_t precision{0};
    bool summary{true};
    std::uint32_t display_order{0};
    DataQuality quality{DataQuality::kUnknown};
    bool valid{false};
    double raw_value{0.0};
    // 可选展示标签；value 仍是历史、告警、MQTT 与北向计算的唯一标准数据。
    std::string display_text;
    std::string message;
    TimestampMs sample_time_ms{0};
    // 仅控制历史采样；不改变实时展示和告警能力。
    bool history_enabled{true};
};

}  // namespace edge_controller
