// 比较系统时钟与单调时钟，识别异常时间跳变。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#pragma once

#include <mutex>
#include <optional>

#include "data/model/time_change.h"

namespace edge_controller {

// 以 steady_clock 的流逝量为参照检测 system_clock 的明显跳变。
class TimeJumpDetector {
public:
    // 对比系统时钟与稳态时钟，检测并返回显著时间跳变。
    std::optional<TimeAdjustmentInfo> observe(
        TimestampMs system_time_ms,
        TimestampMs steady_time_ms);
    // 以当前双时钟值重置检测基线。
    void reset(TimestampMs system_time_ms, TimestampMs steady_time_ms);
    // 清除检测基线，等待下一次观测重新初始化。
    void clear();
    // 判断当前组件是否已初始化。
    bool initialized() const;

private:
    mutable std::mutex mutex_;
    bool initialized_{false};
    TimestampMs last_system_time_ms_{0};
    TimestampMs last_steady_time_ms_{0};
};

}  // namespace edge_controller
