// 使用系统时钟与稳态时钟差值检测明显跳变，避免正常运行耗时被误判为校时。
#include "application/service/time_jump_detector.h"

#include <cstdint>

namespace edge_controller {
namespace {

// 计算两个无符号时间戳之间带符号的安全差值。
std::int64_t signed_delta(TimestampMs current, TimestampMs previous)
{
    return current >= previous
               ? static_cast<std::int64_t>(current - previous)
               : -static_cast<std::int64_t>(previous - current);
}

// 判断时间偏差是否超过跳变检测阈值。
bool significant_delta(std::int64_t delta_ms)
{
    return delta_ms >= kSignificantTimeChangeThresholdMs ||
           delta_ms <= -kSignificantTimeChangeThresholdMs;
}

}  // namespace

// 对比系统时钟与稳态时钟，检测并返回显著时间跳变。
std::optional<TimeAdjustmentInfo> TimeJumpDetector::observe(
    TimestampMs system_time_ms,
    TimestampMs steady_time_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || steady_time_ms < last_steady_time_ms_) {
        initialized_ = true;
        last_system_time_ms_ = system_time_ms;
        last_steady_time_ms_ = steady_time_ms;
        return std::nullopt;
    }

    const auto steady_delta = steady_time_ms - last_steady_time_ms_;
    const auto system_delta = signed_delta(system_time_ms, last_system_time_ms_);
    const auto drift_ms = system_delta - static_cast<std::int64_t>(steady_delta);
    const auto expected_time_ms = last_system_time_ms_ + steady_delta;

    last_system_time_ms_ = system_time_ms;
    last_steady_time_ms_ = steady_time_ms;
    if (!significant_delta(drift_ms)) {
        return std::nullopt;
    }

    TimeAdjustmentInfo adjustment;
    adjustment.source = "monitor";
    adjustment.before_time_ms = expected_time_ms;
    adjustment.after_time_ms = system_time_ms;
    adjustment.delta_ms = drift_ms;
    adjustment.detected_at_ms = system_time_ms;
    adjustment.reason = "运行时检测到系统时间跳变";
    adjustment.significant = true;
    return adjustment;
}

// 以当前双时钟值重置检测基线。
void TimeJumpDetector::reset(TimestampMs system_time_ms, TimestampMs steady_time_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    last_system_time_ms_ = system_time_ms;
    last_steady_time_ms_ = steady_time_ms;
}

// 清除检测基线，等待下一次观测重新初始化。
void TimeJumpDetector::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    last_system_time_ms_ = 0;
    last_steady_time_ms_ = 0;
}

// 判断检测器是否已经建立时间基线。
bool TimeJumpDetector::initialized() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

}  // namespace edge_controller
