// 历史采样缓冲与落库：时间明显跳变只清空尚未提交的 pending 分桶，不重启轮询链路。
#include "application/service/polling_service.h"
#include "application/service/alarm_evaluator.h"
#include "application/service/polling_service_internal.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "shared/common/enums.h"
#include "shared/common/logger.h"
#include "shared/common/readable_error.h"
#include "shared/common/time_utils.h"

namespace edge_controller {

using namespace polling_service_internal;

namespace {

// 移除已被当前配置停用的历史记录。
void remove_disabled_history_records(std::vector<HistoryRecord>* records)
{
    if (records == nullptr) return;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        enabled_fields_by_template;
    records->erase(
        std::remove_if(
            records->begin(),
            records->end(),
            [&](const auto& record) {
                auto iterator =
                    enabled_fields_by_template.find(record.template_id);
                if (iterator == enabled_fields_by_template.end()) {
                    std::unordered_set<std::string> enabled_fields;
                    const auto definition =
                        find_device_template(record.template_id);
                    if (definition != nullptr) {
                        enabled_fields.reserve(definition->fields.size());
                        for (const auto& field : definition->fields) {
                            if (device_template_field_history_enabled(field)) {
                                enabled_fields.insert(field.field_key);
                            }
                        }
                    }
                    iterator = enabled_fields_by_template
                                   .emplace(
                                       record.template_id,
                                       std::move(enabled_fields))
                                   .first;
                }
                return iterator->second.find(record.point_key) ==
                       iterator->second.end();
            }),
        records->end());
}

}  // namespace

// 比较历史写入节流键，用于有序容器排序。
bool PollingService::HistoryWriteThrottleKey::operator<(const HistoryWriteThrottleKey& other) const
{
    return std::tie(device_id, point_key, sample_period, bucket_start_ms) <
           std::tie(other.device_id, other.point_key, other.sample_period, other.bucket_start_ms);
}

// 从设备状态中提取并写入历史趋势记录。
void PollingService::write_history_records(
    const MasterNodeConfig& master_config,
    const std::vector<DeviceStatus>& statuses)
{
    if (history_store_ == nullptr) {
        return;
    }

    const auto now_ms = time_utils::system_now_ms();
    if (now_ms < kMinimumValidSystemTimeMs) {
        if (!history_time_invalid_.exchange(true)) {
            const std::string message = "系统时间早于 2020-01-01，历史采样写入已暂停，时间恢复后将自动继续";
            Logger::error(message);
        }
        return;
    }
    if (history_time_invalid_.exchange(false)) {
        Logger::info("系统时间已恢复到有效范围，历史采样写入自动恢复");
    }

    std::size_t point_capacity = 0;
    for (const auto& status : statuses) {
        point_capacity += status.points.size();
    }
    std::vector<HistoryRecord> candidate_records;
    candidate_records.reserve(point_capacity);
    const auto time_generation = history_time_generation_.load();
    for (const auto& status : statuses) {
        if (status.updated_at_ms < kMinimumValidSystemTimeMs) {
            continue;
        }

        const auto time_context =
            make_history_record_time_context(status.updated_at_ms);
        // 多区块设备允许本轮只成功一部分区块。设备聚合状态为 partial/失败时，
        // 仍按点位自身 valid/quality 写入成功区块的数据，坏点则独立跳过。
        for (const auto& point : status.points) {
            if (!history_point_is_persistable(point)) {
                continue;
            }

            candidate_records.push_back(make_history_record(
                master_config,
                status,
                point,
                time_context,
                "raw_10min"));
        }
    }

    std::vector<HistoryRecord> history_records;
    history_records.reserve(candidate_records.size());
    collect_history_records_for_write(
        candidate_records,
        now_ms,
        time_generation,
        &history_records);

    try {
        if (time_generation != history_time_generation_.load()) {
            return;
        }
        remove_disabled_history_records(&history_records);
        const auto write_status = history_store_->upsert_records(history_records);
        if (!is_ok(write_status)) {
            restore_pending_history_records(history_records, time_generation);
        }
    } catch (const std::exception& error) {
        restore_pending_history_records(history_records, time_generation);
        Logger::error("批量写入设备历史数据异常，原因=" + std::string(error.what()));
    } catch (...) {
        restore_pending_history_records(history_records, time_generation);
        Logger::error("批量写入设备历史数据发生未知异常");
    }
}

// 系统时间调整后修正历史聚合窗口。
std::size_t PollingService::on_system_time_adjusted(TimestampMs after_time_ms)
{
    std::lock_guard<std::mutex> lock(history_write_mutex_);
    ++history_time_generation_;
    const auto discarded = pending_history_records_.size();
    pending_history_records_.clear();
    history_write_times_.clear();
    history_time_invalid_.store(after_time_ms < kMinimumValidSystemTimeMs);
    return discarded;
}

// 判断历史写入是否处于保护状态。
bool PollingService::history_write_protected() const
{
    return history_time_invalid_.load();
}

// 批量收集待写历史记录，并按周期做写入节流。
void PollingService::collect_history_records_for_write(
    const std::vector<HistoryRecord>& records,
    TimestampMs now_ms,
    std::uint64_t time_generation,
    std::vector<HistoryRecord>* records_to_write)
{
    if (records_to_write == nullptr || records.empty()) {
        return;
    }

    const auto throttle_ms = kRaw10MinHistoryWriteThrottleMs;
    std::lock_guard<std::mutex> lock(history_write_mutex_);
    if (time_generation != history_time_generation_.load()) {
        return;
    }

    for (const auto& record : records) {
        if (record.device_id.empty() ||
            record.point_key.empty() ||
            record.bucket_start_ms == 0 ||
            record.sample_period != "raw_10min") {
            continue;
        }

        const HistoryWriteThrottleKey key{
            record.device_id,
            record.point_key,
            record.sample_period,
            record.bucket_start_ms,
        };
        // map 按 device/point/period/bucket 排序，只检查当前数据项所在的连续区间。
        const HistoryWriteThrottleKey range_start{
            key.device_id,
            key.point_key,
            key.sample_period,
            0,
        };
        for (auto iterator =
                 pending_history_records_.lower_bound(range_start);
             iterator != pending_history_records_.end();) {
            const auto& existing = iterator->first;
            if (existing.device_id != key.device_id ||
                existing.point_key != key.point_key ||
                existing.sample_period != key.sample_period) {
                break;
            }
            if (existing.bucket_start_ms != key.bucket_start_ms) {
                records_to_write->push_back(iterator->second);
                history_write_times_.erase(existing);
                iterator = pending_history_records_.erase(iterator);
                continue;
            }
            ++iterator;
        }

        const auto pending =
            pending_history_records_.insert_or_assign(key, record).first;
        const auto found = history_write_times_.find(key);
        if (found == history_write_times_.end() ||
            now_ms >= found->second + throttle_ms) {
            history_write_times_[key] = now_ms;
            records_to_write->push_back(pending->second);
        }
    }
}

// 恢复待写入历史数据记录。
void PollingService::restore_pending_history_records(
    const std::vector<HistoryRecord>& records,
    std::uint64_t time_generation)
{
    if (records.empty()) {
        return;
    }

    const auto now_ms = time_utils::system_now_ms();
    std::size_t discarded_count = 0;
    {
        std::lock_guard<std::mutex> lock(history_write_mutex_);
        if (time_generation != history_time_generation_.load()) {
            return;
        }
        for (const auto& record : records) {
            if (record.device_id.empty() ||
                record.point_key.empty() ||
                record.bucket_start_ms == 0 ||
                record.sample_period != "raw_10min") {
                continue;
            }
            const HistoryWriteThrottleKey key{
                record.device_id,
                record.point_key,
                record.sample_period,
                record.bucket_start_ms,
            };
            pending_history_records_[key] = record;
            if (history_write_times_.find(key) == history_write_times_.end()) {
                history_write_times_[key] = now_ms;
            }
        }
        discarded_count = prune_history_retry_backlog(
            &pending_history_records_,
            &history_write_times_,
            kMaxPendingHistoryBucketsPerPoint,
            [](const auto& left, const auto& right) {
                return left.device_id == right.device_id &&
                    left.point_key == right.point_key &&
                    left.sample_period == right.sample_period;
            });
    }

    if (discarded_count > 0) {
        // 日志文本保持稳定，交给 Logger 抑制持续磁盘故障下的重复告警。
        Logger::warn(
            "历史数据库持续写入失败，重试缓存已达上限，"
            "已丢弃每个点位最旧的历史桶以保护长期运行内存");
    }
}

// 将节流缓存中的历史记录立即写入存储。
void PollingService::flush_pending_history_records()
{
    if (history_store_ == nullptr) {
        return;
    }

    std::vector<HistoryRecord> records;
    const auto time_generation = history_time_generation_.load();
    {
        std::lock_guard<std::mutex> lock(history_write_mutex_);
        records.reserve(pending_history_records_.size());
        for (const auto& entry : pending_history_records_) {
            records.push_back(entry.second);
        }
        pending_history_records_.clear();
        history_write_times_.clear();
    }

    remove_disabled_history_records(&records);
    if (records.empty()) {
        return;
    }

    try {
        if (time_generation != history_time_generation_.load()) {
            return;
        }
        const auto write_status = history_store_->upsert_records(records);
        if (!is_ok(write_status)) {
            restore_pending_history_records(records, time_generation);
        }
    } catch (const std::exception& error) {
        restore_pending_history_records(records, time_generation);
        Logger::error("停止轮询前写入 pending 历史数据异常，原因=" + std::string(error.what()));
    } catch (...) {
        restore_pending_history_records(records, time_generation);
        Logger::error("停止轮询前写入 pending 历史数据发生未知异常");
    }
}

}  // namespace edge_controller
