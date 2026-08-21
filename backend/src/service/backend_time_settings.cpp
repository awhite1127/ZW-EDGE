// 时间设置入口只管理时区、NTP、系统时钟和 RTC；不得调用采集配置重载或串口重建路径。
#include "service/backend_service.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>

#include "common/time_utils.h"
#include "service/backend_service_internal.h"

namespace edge_controller {
namespace {

// 裁剪时间值。
std::string trim_time_value(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// 规范化输入并返回稳定结果。
TimeSettings normalized_settings(const TimeSettingsUpdateRequest& request)
{
    TimeSettings settings;
    settings.timezone = trim_time_value(request.timezone);
    settings.ntp_enabled = request.ntp_enabled;
    settings.ntp_primary = trim_time_value(request.ntp_primary);
    settings.ntp_secondary = trim_time_value(request.ntp_secondary);
    settings.updated_at = time_utils::system_now_ms();
    return settings;
}

// 判断两份时间设置是否等价。
bool equivalent_time_settings(const TimeSettings& left, const TimeSettings& right)
{
    return left.timezone == right.timezone &&
           left.ntp_enabled == right.ntp_enabled &&
           left.ntp_primary == right.ntp_primary &&
           left.ntp_secondary == right.ntp_secondary;
}

// 扣除稳态时钟流逝量后计算系统时间偏差。
std::int64_t time_delta_ms(
    TimestampMs after,
    TimestampMs before,
    TimestampMs steady_after,
    TimestampMs steady_before)
{
    const auto system_delta = after >= before
                                  ? static_cast<std::int64_t>(after - before)
                                  : -static_cast<std::int64_t>(before - after);
    const auto steady_delta = steady_after >= steady_before
                                  ? static_cast<std::int64_t>(steady_after - steady_before)
                                  : 0;
    return system_delta - steady_delta;
}

}  // namespace

// 读取时间设置。
TimeSettings BackendService::get_time_settings() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return system_config_.time_settings;
}

// 读取系统时间运行状态。
TimeRuntimeStatus BackendService::get_time_runtime_status() const
{
    TimeSettings persisted_settings;
    TimeSettings applied_settings;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        persisted_settings = system_config_.time_settings;
        applied_settings = applied_time_settings_;
    }
    auto status = time_runtime_.runtime_status(applied_settings);
    status.settings_pending_apply = !equivalent_time_settings(persisted_settings, applied_settings);
    return status;
}

// 保存并应用时间设置。
StatusCode BackendService::save_and_apply_time_settings(
    const TimeSettingsUpdateRequest& request,
    TimeApplyResult* result,
    std::string* error_message)
{
    std::lock_guard<std::mutex> time_operation_lock(time_service_mutex_);
    if (result == nullptr) {
        if (error_message != nullptr) *error_message = "缺少时间设置应用结果输出参数";
        return StatusCode::kInvalidArgument;
    }
    *result = {};
    const auto next = normalized_settings(request);
    const auto validation = time_runtime_.validate_settings(next, error_message);
    if (!is_ok(validation)) return validation;

    TimeSettings previous_persisted;
    TimeSettings previous_applied;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化";
            return StatusCode::kInvalidState;
        }
        previous_persisted = system_config_.time_settings;
        previous_applied = applied_time_settings_;
    }

    std::string write_error;
    const auto write_status = config_store_.save_time_settings(next, &write_error);
    if (!is_ok(write_status)) {
        if (error_message != nullptr) *error_message = write_error.empty() ? "保存时间设置到 SQLite 失败" : write_error;
        return write_status;
    }

    std::string apply_error;
    const auto apply_status = time_runtime_.apply_settings(next, previous_applied, &result->warning_message, &apply_error);
    if (!is_ok(apply_status)) {
        std::string rollback_error;
        const auto rollback_status = config_store_.save_time_settings(previous_persisted, &rollback_error);
        if (error_message != nullptr) {
            *error_message = apply_error.empty() ? "应用时间设置失败" : apply_error;
            if (!is_ok(rollback_status)) *error_message += "；SQLite 配置回退失败: " + rollback_error;
        }
        return apply_status;
    }

    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        system_config_.time_settings = next;
        applied_time_settings_ = next;
    }
    result->settings = next;
    result->message = next.ntp_enabled ? "日期与时间设置已保存并应用，后台 NTP 已启动" : "日期与时间设置已保存并应用，NTP 已停用";
    append_event("info", "system_time", "settings", "日期与时间设置已应用", result->message, 0);
    return StatusCode::kOk;
}

// 恢复时间设置。
StatusCode BackendService::restore_time_settings_on_startup(std::string* error_message)
{
    std::lock_guard<std::mutex> time_operation_lock(time_service_mutex_);
    TimeSettings settings;
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) *error_message = "后端服务尚未初始化";
            return StatusCode::kInvalidState;
        }
        settings = system_config_.time_settings;
    }
    const auto status = time_runtime_.restore_on_startup(settings, error_message);
    if (is_ok(status)) {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        applied_time_settings_ = settings;
    } else {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        applied_time_settings_ = time_runtime_.snapshot_applied_settings(applied_time_settings_);
    }
    // 启动恢复属于基线建立，不产生跳变事件。
    time_jump_detector_.reset(time_utils::system_now_ms(), time_utils::steady_now_ms());
    return status;
}

// 立即执行一次系统时间同步。
StatusCode BackendService::sync_time_now(TimeSyncResult* result, std::string* error_message)
{
    // 立即校时是最多 45 秒的管理操作。重复请求若阻塞等待同一互斥，会把
    // 固定 IPC worker 全部占满；采用 single-flight，后续请求快速返回。
    std::unique_lock<std::mutex> time_operation_lock(time_service_mutex_, std::try_to_lock);
    if (!time_operation_lock.owns_lock()) {
        if (error_message != nullptr) *error_message = "时间同步正在执行，请稍后重试";
        return StatusCode::kInvalidState;
    }
    TimeSettings settings;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!equivalent_time_settings(system_config_.time_settings, applied_time_settings_)) {
            if (error_message != nullptr) *error_message = "时间设置待应用，请先保存并应用后再立即同步";
            return StatusCode::kInvalidState;
        }
        settings = applied_time_settings_;
    }
    const auto before_time_ms = time_utils::system_now_ms();
    const auto before_steady_ms = time_utils::steady_now_ms();
    const auto status = time_runtime_.sync_now(settings, result, std::chrono::seconds(45), error_message);
    if (is_ok(status)) {
        const auto after_time_ms = time_utils::system_now_ms();
        const auto after_steady_ms = time_utils::steady_now_ms();
        TimeAdjustmentInfo adjustment;
        adjustment.source = "ntp_now";
        adjustment.before_time_ms = before_time_ms;
        adjustment.after_time_ms = after_time_ms;
        adjustment.delta_ms = time_delta_ms(after_time_ms, before_time_ms, after_steady_ms, before_steady_ms);
        adjustment.detected_at_ms = after_time_ms;
        adjustment.reason = "用户执行立即 NTP 同步";
        handle_system_time_changed(
            std::move(adjustment),
            result == nullptr ? std::string{} : result->warning_message);
    } else {
        append_event("error", "system_time", "ntp_sync", "NTP 立即同步失败", error_message == nullptr ? std::string{} : *error_message, 0);
    }
    return status;
}

// 设置手动系统时间。
StatusCode BackendService::set_manual_system_time(
    const ManualTimeSetRequest& request,
    ManualTimeSetResult* result,
    std::string* error_message)
{
    std::lock_guard<std::mutex> time_operation_lock(time_service_mutex_);
    const auto source = request.source.empty() ? std::string("manual") : request.source;
    if (source != "manual" && source != "browser") {
        if (error_message != nullptr) *error_message = "手动校时来源只能是 manual 或 browser";
        return StatusCode::kInvalidArgument;
    }
    TimeSettings settings;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!equivalent_time_settings(system_config_.time_settings, applied_time_settings_)) {
            if (error_message != nullptr) *error_message = "时间设置待应用，请先保存并应用后再手动校时";
            return StatusCode::kInvalidState;
        }
        settings = applied_time_settings_;
    }
    const auto before_steady_ms = time_utils::steady_now_ms();
    const auto status = time_runtime_.set_manual_time(settings, request.epoch_ms, result, error_message);
    if (is_ok(status) && result != nullptr) {
        const auto after_steady_ms = time_utils::steady_now_ms();
        TimeAdjustmentInfo adjustment;
        adjustment.source = source;
        adjustment.before_time_ms = result->previous_time_ms;
        adjustment.after_time_ms = result->current_time_ms;
        adjustment.delta_ms = time_delta_ms(
            result->current_time_ms,
            result->previous_time_ms,
            after_steady_ms,
            before_steady_ms);
        adjustment.detected_at_ms = result->current_time_ms;
        adjustment.reason = source == "browser" ? "用户同步浏览器时间" : "用户手动设置系统时间";
        handle_system_time_changed(std::move(adjustment), result->warning_message);
        result->message = source == "browser" ? "浏览器时间已同步，系统时间已调整" : "系统时间已手动调整";
    } else {
        append_event("error", "system_time", "manual_set", "手动设置系统时间失败", error_message == nullptr ? std::string{} : *error_message, 0);
    }
    return status;
}

}  // namespace edge_controller
