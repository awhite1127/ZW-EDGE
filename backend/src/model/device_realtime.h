// 单设备实时数据项快照及采样时间模型。
#pragma once

#include <string>
#include <vector>

#include "common/enums.h"
#include "common/types.h"
#include "model/point_value.h"

namespace edge_controller {

struct DeviceRealtimeSnapshot {
    DeviceId device_id;
    std::string device_name;
    MasterNodeId master_id;
    std::string template_id;
    std::string template_name;
    TimestampMs sample_time_ms{0};
    // 设备级快照只承载通讯质量；每个点位的数据质量保存在 PointValue::quality 中。
    DataQuality communication_quality{DataQuality::kUnknown};
    std::vector<PointValue> points;

    bool has_resistance{false};
    PointValue resistance;
};

}  // namespace edge_controller
