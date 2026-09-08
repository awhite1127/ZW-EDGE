// 集中定义轮询实现共享的节流、历史记录和诊断辅助。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#pragma once

#include <chrono>
#include <cstddef>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "shared/common/enums.h"
#include "shared/common/readable_error.h"
#include "data/model/system_config.h"
#include "communication/protocol/modbus_rtu_protocol.h"
#include "application/service/polling_service.h"

namespace edge_controller {
namespace polling_service_internal {

inline constexpr TimestampMs kRaw10MinHistoryWriteThrottleMs = 2ULL * 60ULL * 1000ULL;
// SQLite 持续不可写时每个点位只保留最新 1 小时的 10 分钟桶，防止重试队列无上限增长。
inline constexpr std::size_t kMaxPendingHistoryBucketsPerPoint = 6U;

// 按设备/点位/周期分组丢弃最旧重试桶；调用方必须已持有历史缓存锁。
template <typename PendingMap, typename WriteTimeMap, typename SameSeries>
std::size_t prune_history_retry_backlog(
    PendingMap* pending_records,
    WriteTimeMap* write_times,
    std::size_t max_buckets_per_point,
    SameSeries same_series)
{
    if (pending_records == nullptr || write_times == nullptr || pending_records->empty()) {
        return 0;
    }

    std::size_t discarded_count = 0;
    auto series_begin = pending_records->begin();
    while (series_begin != pending_records->end()) {
        auto series_end = std::next(series_begin);
        while (series_end != pending_records->end() &&
               same_series(series_begin->first, series_end->first)) {
            ++series_end;
        }

        auto series_size = static_cast<std::size_t>(std::distance(series_begin, series_end));
        while (series_size > max_buckets_per_point) {
            write_times->erase(series_begin->first);
            series_begin = pending_records->erase(series_begin);
            --series_size;
            ++discarded_count;
        }
        series_begin = series_end;
    }
    return discarded_count;
}

// 历史趋势只接收可展示的当前好值；设备级 partial 不参与该字段级判断。
inline bool history_point_is_persistable(const PointValue& point)
{
    return point.history_enabled &&
           point.valid &&
           point.quality == DataQuality::kGood &&
           !point.key.empty() &&
           std::isfinite(point.value);
}

// 将毫秒时间戳转换为本地时间结构。
inline std::tm local_time_from_ms(TimestampMs timestamp_ms)
{
    const auto time_point = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
    const auto time_value = std::chrono::system_clock::to_time_t(time_point);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &time_value);
#else
    localtime_r(&time_value, &local_tm);
#endif
    return local_tm;
}

// 将本地时间结构转换为毫秒时间戳。
inline TimestampMs timestamp_ms_from_local_time(std::tm local_tm)
{
    const auto time_value = std::mktime(&local_tm);
    if (time_value < 0) {
        return 0;
    }
    return static_cast<TimestampMs>(time_value) * 1000ULL;
}

// 格式化时间桶文本。
inline std::string format_bucket_text(const std::tm& local_tm, const char* format)
{
    std::ostringstream stream;
    stream << std::put_time(&local_tm, format);
    return stream.str();
}

struct HistoryRecordTimeContext {
    TimestampMs bucket_start_ms{0};
    std::string bucket_text;
    std::string date;
};

// 同一设备状态的全部点位共享时间桶，避免逐字段重复 localtime/mktime/格式化。
inline HistoryRecordTimeContext make_history_record_time_context(
    TimestampMs timestamp_ms)
{
    auto bucket_tm = local_time_from_ms(timestamp_ms);
    bucket_tm.tm_sec = 0;
    bucket_tm.tm_min = (bucket_tm.tm_min / 10) * 10;

    HistoryRecordTimeContext context;
    context.bucket_start_ms = timestamp_ms_from_local_time(bucket_tm);
    context.bucket_text = format_bucket_text(bucket_tm, "%Y-%m-%d %H:%M");
    context.date = format_bucket_text(bucket_tm, "%Y-%m-%d");
    return context;
}

inline HistoryRecord make_history_record(
    const MasterNodeConfig& master_config,
    const DeviceStatus& status,
    const PointValue& point,
    const HistoryRecordTimeContext& time_context,
    const std::string& sample_period)
{
    HistoryRecord record;
    record.device_id = status.device_id;
    record.master_id = status.master_id.empty() ? master_config.master_id : status.master_id;
    record.channel_id = master_config.channel_id;
    record.template_id = status.template_id;
    record.sample_period = sample_period;
    record.bucket_start_ms = time_context.bucket_start_ms;
    record.bucket_text = time_context.bucket_text;
    record.timestamp_ms = status.updated_at_ms;
    record.date = time_context.date;
    record.point_key = point.key;
    record.point_name = point.name;
    record.unit = point.unit;
    record.precision = point.precision;
    record.value = point.value;
    record.raw_value = std::isfinite(point.raw_value) ? point.raw_value : point.value;
    record.quality = to_string(point.quality);
    record.valid = point.valid;
    record.message = point.message;
    return record;
}

// 合并采集失败状态并保留最有诊断价值的信息。
inline DeviceStatus merge_failed_status(const DeviceStatus& cached_status, const DeviceStatus& failed_status)
{
    // 全失败且没有新点位时才保留旧业务值；部分成功状态携带本轮完整点位集合，
    // 其中成功区块字段保留当前时间，失败区块字段明确 invalid/bad。
    DeviceStatus merged = cached_status;
    merged.device_id = failed_status.device_id;
    merged.device_name = failed_status.device_name;
    merged.master_id = failed_status.master_id;
    merged.online = failed_status.online;
    merged.last_collect_success = failed_status.last_collect_success;
    merged.communication_quality = failed_status.communication_quality;
    merged.updated_at_ms = failed_status.updated_at_ms;
    if (failed_status.communication_quality == DataQuality::kPartial &&
        failed_status.last_success_time_ms != 0) {
        merged.last_success_time_ms = failed_status.last_success_time_ms;
    }
    merged.last_failure_time_ms = failed_status.last_failure_time_ms;
    merged.diagnosis = make_diagnosis(
        DiagnosisLevel::kDevice,
        failed_status.device_id,
        failed_status.device_name,
        failed_status.communication_quality == DataQuality::kPartial
            ? DiagnosisRunStatus::kWarning
            : (failed_status.diagnosis.status == "offline"
                   ? DiagnosisRunStatus::kOffline
                   : DiagnosisRunStatus::kError),
        diagnosis_code_from_string(failed_status.diagnosis.error_code),
        merged.last_success_time_ms,
        failed_status.last_failure_time_ms,
        cached_status.diagnosis.consecutive_failures + 1);
    if (!failed_status.last_error_message.empty()) {
        merged.diagnosis.message = failed_status.last_error_message;
    }
    merged.last_error_message = merged.diagnosis.message;
    if (!failed_status.points.empty()) {
        merged.template_id = failed_status.template_id;
        merged.template_name = failed_status.template_name;
        merged.has_resistance = failed_status.has_resistance;
        merged.resistance_value = failed_status.resistance_value;
        merged.points = failed_status.points;
    }
    return merged;
}

// 查找通道配置。
inline const ChannelConfig* find_channel_config(const SystemConfig* system_config, const ChannelId& channel_id)
{
    if (system_config == nullptr) {
        return nullptr;
    }
    for (const auto& channel : system_config->channels) {
        if (channel.channel_id == channel_id) {
            return &channel;
        }
    }
    return nullptr;
}

}  // namespace polling_service_internal
}  // namespace edge_controller
