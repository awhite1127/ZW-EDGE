// 服务事件持久化、聚合、查询和清理接口。
#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/status_code.h"
#include "model/service_summary.h"

struct sqlite3;

namespace edge_controller {

struct EventExportQuery {
    std::string level;
    std::string source;
    std::string time_range;
    std::string search;
    std::uint32_t limit{500};
    std::uint32_t offset{0};
};

// 历史事件页的后端筛选与分页参数。
struct EventHistoryQuery {
    std::string level;
    std::string source;
    std::string time_range;
    std::string search;
    std::uint32_t page{1};
    std::uint32_t page_size{10};
};

struct EventLevelStats {
    std::uint64_t error{0};
    std::uint64_t warning{0};
    std::uint64_t info{0};
};

struct EventSourceStat {
    std::string source;
    std::uint64_t count{0};
};

struct EventHistoryResult {
    std::vector<ServiceEvent> rows;
    // total 只统计当前筛选条件的匹配数，用于准确分页。
    std::uint64_t total{0};
    // 级别与来源统计覆盖全部保留事件，保持页面统计卡与筛选项的现有语义。
    EventLevelStats level_stats;
    std::vector<EventSourceStat> source_stats;
};

class EventStore {
public:
    // 销毁 EventStore 实例并释放相关资源。
    ~EventStore();

    // 初始化。
    StatusCode initialize(const std::string& database_path, std::string* error_message = nullptr);
    // 追加。
    StatusCode append(ServiceEvent event, std::string* error_message = nullptr, ServiceEvent* stored_event = nullptr);
    // 列出最近。
    StatusCode list_recent(std::size_t limit, std::vector<ServiceEvent>* events, std::string* error_message = nullptr) const;
    // 按页面条件在数据库中筛选、分页并返回统计。
    StatusCode query_history(
        const EventHistoryQuery& query,
        EventHistoryResult* result,
        std::string* error_message = nullptr) const;
    // 导出事件。
    StatusCode export_events(
        const EventExportQuery& query,
        std::vector<ServiceEvent>* events,
        std::string* error_message = nullptr) const;
    // 清空全部。
    StatusCode clear_all(std::string* error_message = nullptr);
    // 立即将待写事件刷新到数据库。
    StatusCode save_now(std::string* error_message = nullptr);
    // 清理过期。
    StatusCode cleanup_expired(std::uint64_t* deleted_count, std::string* error_message = nullptr);
    // 系统时间明显跳变后刷新并清空进程内去重窗口。
    StatusCode reset_deduplication(std::string* error_message = nullptr);
    // 记录数量。
    StatusCode record_count(std::uint64_t* count, std::string* error_message = nullptr) const;

private:
    struct DedupState {
        ServiceEvent event;
        TimestampMs last_counted_at_ms{0};
        bool dirty{false};
    };

    // 在持锁状态下初始化数据库结构。
    StatusCode initialize_schema_locked(std::string* error_message);
    // 在持锁状态下写入。
    StatusCode insert_locked(ServiceEvent* event, std::string* error_message);
    // 在持锁状态下更新事件。
    StatusCode update_event_locked(const ServiceEvent& event, std::string* error_message) const;
    // 在持锁状态下刷新尚未持久化的事件。
    StatusCode flush_dirty_locked(std::string* error_message) const;
    // 在持锁状态下清理。
    StatusCode cleanup_locked(TimestampMs now_ms, std::string* error_message, std::uint64_t* deleted_count = nullptr);
    // 在持锁状态下执行。
    StatusCode execute_locked(const char* sql, std::string* error_message) const;
    // 在持锁状态下检查数据库是否可用。
    bool available_locked(std::string* error_message) const;
    // 在持锁状态下关闭。
    void close_locked();

    // 事件落库与进程内去重状态必须在同一临界区更新，确保计数和最近发生时间一致。
    mutable std::mutex mutex_;
    ::sqlite3* database_{nullptr};
    std::string database_path_;
    mutable std::unordered_map<std::string, DedupState> dedup_states_{};
    // 正常写入会立即落库；单独计数可让查询路径在无失败补写时 O(1) 跳过整张去重表扫描。
    mutable std::size_t dirty_state_count_{0};
    std::chrono::steady_clock::time_point last_cleanup_time_{};
    std::size_t record_count_{0};
};

}  // namespace edge_controller
