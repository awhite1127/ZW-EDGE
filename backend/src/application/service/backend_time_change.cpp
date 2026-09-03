// 时间跳变联动重置历史 pending 与事件去重状态，并通知 MQTT 异步重连，保持采集链路连续运行。
#include "application/service/backend_service.h"

#include <chrono>
#include <sstream>
#include <system_error>

#include "shared/common/logger.h"
#include "shared/common/time_utils.h"

namespace edge_controller {
namespace {

constexpr auto kTimeJumpMonitorInterval = std::chrono::seconds(5);
constexpr TimestampMs kTimeJumpStartupGraceMs = 15ULL * 1000ULL;

// 判断时间偏差是否达到跳变阈值。
bool is_significant(std::int64_t delta_ms)
{
    return delta_ms >= kSignificantTimeChangeThresholdMs ||
           delta_ms <= -kSignificantTimeChangeThresholdMs;
}

// 生成来源显示文本。
std::string source_text(const std::string& source)
{
    if (source == "manual") return "手动校时";
    if (source == "browser") return "同步浏览器时间";
    if (source == "ntp_now") return "立即 NTP 同步";
    if (source == "startup_restore") return "启动时间恢复";
    return "运行时监测";
}

}  // namespace

// 处理系统时间变更。
void BackendService::handle_system_time_changed(
    TimeAdjustmentInfo adjustment,
    const std::string& warning_message)
{
    std::lock_guard<std::mutex> adjustment_lock(time_adjustment_mutex_);
    if (adjustment.after_time_ms == 0) {
        adjustment.after_time_ms = time_utils::system_now_ms();
    }
    if (adjustment.detected_at_ms == 0) {
        adjustment.detected_at_ms = adjustment.after_time_ms;
    }
    adjustment.significant = is_significant(adjustment.delta_ms);

    if (adjustment.source == last_time_adjustment_source_ &&
        adjustment.after_time_ms == last_time_adjustment_after_ms_ &&
        adjustment.delta_ms == last_time_adjustment_delta_ms_) {
        return;
    }
    last_time_adjustment_source_ = adjustment.source;
    last_time_adjustment_after_ms_ = adjustment.after_time_ms;
    last_time_adjustment_delta_ms_ = adjustment.delta_ms;

    // 显式校时和监测路径都在处理后重建基线，避免同一个跳变被监测线程重复上报。
    time_jump_detector_.reset(adjustment.after_time_ms, time_utils::steady_now_ms());
    if (!adjustment.significant) {
        Logger::info(
            "系统时间变化未达到联动阈值，source=" + adjustment.source +
            "，delta_ms=" + std::to_string(adjustment.delta_ms));
        return;
    }

    TimeAdjustmentHandlingResult handling;
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (history_sampling_service_ != nullptr) {
            handling.discarded_pending_history_records =
                history_sampling_service_->on_system_time_adjusted(adjustment.after_time_ms);
            handling.history_state_reset = true;
        }
    }
    handling.mqtt = mqtt_publisher_service_.on_system_time_adjusted();
    handling.event_deduplication_reset =
        is_ok(event_store_.reset_deduplication(&handling.event_deduplication_error));

    std::ostringstream detail;
    detail << "来源=" << source_text(adjustment.source)
           << "，调整前 epoch_ms=" << adjustment.before_time_ms
           << "，调整后 epoch_ms=" << adjustment.after_time_ms
           << "，变化量=" << adjustment.delta_ms << "ms"
           << "，检测时间 epoch_ms=" << adjustment.detected_at_ms
           << "，明显跳变=是"
           << "，历史临时状态="
            << (handling.history_state_reset
                    ? "已重置（丢弃 pending " + std::to_string(handling.discarded_pending_history_records) + " 条）"
                    : "轮询未启动，无需重置")
           << "，事件去重="
           << (handling.event_deduplication_reset
                   ? "已重置"
                   : "重置失败（" + handling.event_deduplication_error + "）")
           << "，MQTT联动=" << handling.mqtt.detail;
    if (!warning_message.empty()) {
        detail << "，校时警告=" << warning_message;
    }
    if (!adjustment.reason.empty()) {
        detail << "，原因=" << adjustment.reason;
    }

    append_event(
        "info",
        "system_time",
        "time_adjustment-" + std::to_string(adjustment.after_time_ms),
        "系统时间已调整",
        detail.str(),
        adjustment.after_time_ms);
    Logger::warn("检测到明显系统时间调整：" + detail.str());
}

// 启动时间跳变监控。
StatusCode BackendService::start_time_jump_monitor(std::string* error_message)
{
    time_jump_detector_.reset(time_utils::system_now_ms(), time_utils::steady_now_ms());
    {
        std::lock_guard<std::mutex> lock(time_monitor_mutex_);
        if (time_monitor_thread_.joinable() && !time_monitor_stop_requested_) {
            return StatusCode::kOk;
        }
        time_monitor_stop_requested_ = false;
    }
    try {
        time_monitor_thread_ = std::thread([this]() { time_jump_monitor_loop(); });
    } catch (const std::system_error& error) {
        {
            std::lock_guard<std::mutex> lock(time_monitor_mutex_);
            time_monitor_stop_requested_ = true;
        }
        if (error_message != nullptr) {
            *error_message = "创建时间跳变监控线程失败：" + std::string(error.what());
        }
        return StatusCode::kInternalError;
    }
    return StatusCode::kOk;
}

// 停止时间跳变监控。
void BackendService::stop_time_jump_monitor()
{
    {
        std::lock_guard<std::mutex> lock(time_monitor_mutex_);
        time_monitor_stop_requested_ = true;
    }
    time_monitor_wakeup_.notify_all();
    if (time_monitor_thread_.joinable()) {
        time_monitor_thread_.join();
    }
    time_jump_detector_.clear();
}

// 周期检测系统时间跳变并通知相关服务。
void BackendService::time_jump_monitor_loop()
{
    const auto startup_grace_deadline = time_utils::steady_now_ms() + kTimeJumpStartupGraceMs;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(time_monitor_mutex_);
            if (time_monitor_wakeup_.wait_for(
                    lock,
                    kTimeJumpMonitorInterval,
                    [this]() { return time_monitor_stop_requested_; })) {
                return;
            }
        }
        // 与显式校时串行，避免同一次手动/NTP调整同时被 monitor 重复上报。
        std::lock_guard<std::mutex> time_operation_lock(time_service_mutex_);
        if (time_utils::steady_now_ms() < startup_grace_deadline) {
            time_jump_detector_.reset(time_utils::system_now_ms(), time_utils::steady_now_ms());
            continue;
        }
        const auto adjustment = time_jump_detector_.observe(
            time_utils::system_now_ms(),
            time_utils::steady_now_ms());
        if (adjustment.has_value()) {
            handle_system_time_changed(*adjustment);
        }
    }
}

}  // namespace edge_controller
