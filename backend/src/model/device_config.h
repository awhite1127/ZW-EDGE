// 描述自动推导设备的稳定 ID、系统名称、显示名称、所属主站和寄存器偏移。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/types.h"

namespace edge_controller {

// 设备由所属主控的显式 device_count 和设备类型地址跨度生成，只保留实例级可维护字段。
struct DeviceConfig {
    DeviceId device_id;
    // 系统推导名称始终保留，device_name 为合并别名后的现场显示名称。
    std::string generated_name;
    std::string device_name;
    MasterNodeId master_id;
    bool enabled{true};
    // 设备基地址相对于 MasterNode 第一台设备基地址的偏移，等于 index × stride。
    RegisterAddress register_offset{0};
};

struct DeviceAlias {
    DeviceId device_id;
    std::string display_name;
    TimestampMs updated_at_ms{0};
};

struct DeviceDisplayNameBatchItem {
    DeviceId device_id;
    std::string display_name;
};

struct DeviceDisplayNameBatchFailure {
    DeviceId device_id;
    std::string message;
};

struct DeviceDisplayNameBatchResult {
    std::size_t success_count{0};
    std::size_t failure_count{0};
    std::vector<DeviceDisplayNameBatchFailure> failures;
};

}  // namespace edge_controller
