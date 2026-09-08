// 持久化十分钟、小时和天级历史数据并执行分层聚合与保留清理。
// 边界：一致性由类内锁或 SQLite 事务保证，错误通过 StatusCode 返回。

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "shared/common/status_code.h"
#include "shared/common/types.h"

struct sqlite3;

namespace edge_controller {

struct HistoryRecord {
    DeviceId device_id;
    MasterNodeId master_id;
    ChannelId channel_id;
    std::string template_id;
    std::string sample_period{"day"};
    TimestampMs bucket_start_ms{0};
    std::string bucket_text;
    TimestampMs timestamp_ms{0};
    std::string date;
    std::string point_key;
    std::string point_name;
    std::string unit;
    std::uint32_t precision{0};
    double value{0.0};
    double raw_value{0.0};
    std::string quality{"good"};
    bool valid{true};
    std::string message;
};

struct HistoryPointSummary {
    std::string point_key;
    std::string point_name;
    std::string unit;
    std::uint32_t precision{0};
};

// 历史总览使用的设备/点位/周期轻量聚合结果，不包含完整曲线记录。
struct HistoryOverviewSummary {
    DeviceId device_id;
    MasterNodeId master_id;
    ChannelId channel_id;
    std::string point_key;
    std::string point_name;
    std::string unit;
    std::uint32_t precision{0};
    std::string sample_period;
    std::uint64_t record_count{0};
    double latest_value{0.0};
    TimestampMs latest_timestamp_ms{0};
    TimestampMs latest_bucket_start_ms{0};
};

struct HistoryQueryOptions {
    std::uint32_t days{30};
    std::string start_date;
    std::string end_date;
    std::string point_key;
    std::string sample_period{"day"};
};

struct HistoryExportQuery {
    ChannelId channel_id;
    MasterNodeId master_id;
    DeviceId device_id;
    std::string point_key;
    std::string sample_period;
    std::uint32_t limit{500};
    std::uint32_t offset{0};
};

// 历史导出内部稳定游标。公开 IPC 仍使用 limit/offset；服务内跨批扫描使用该游标，
// 避免清理或并发写入导致后续批次因 OFFSET 位移而重复/遗漏。
struct HistoryExportCursor {
    TimestampMs bucket_start_ms{0};
    ChannelId channel_id;
    MasterNodeId master_id;
    DeviceId device_id;
    std::string point_key;
    std::string sample_period;
    bool valid{false};
};

struct HistoryCleanupResult {
    std::uint64_t deleted_raw_10min_count{0};
    std::uint64_t deleted_hour_count{0};
    std::uint64_t deleted_day_count{0};
};

class HistoryStore {
public:

    ~HistoryStore();

    // 初始化历史数据 SQLite 存储并创建必要表结构。
    StatusCode initialize(const std::string& database_path = {});
    // 批量写入或更新设备历史趋势记录。
    StatusCode upsert_records(const std::vector<HistoryRecord>& records);
    // 按设备和查询条件读取历史趋势记录。
    StatusCode get_device_history(
        const DeviceId& device_id,
        const HistoryQueryOptions& options,
        std::vector<HistoryRecord>* records,
        std::string* error_message = nullptr) const;
    // 读取指定设备默认范围内的历史趋势记录。
    StatusCode get_device_history(
        const DeviceId& device_id,
        std::vector<HistoryRecord>* records,
        std::string* error_message = nullptr) const;
    // 查询指定设备已产生历史数据的数据项列表。
    StatusCode get_device_history_points(
        const DeviceId& device_id,
        const std::string& sample_period,
        std::vector<HistoryPointSummary>* points,
        std::string* error_message = nullptr) const;
    // 批量查询历史总览所需的最近 30 天日摘要和最近 24 小时小时摘要。
    StatusCode get_history_overview_summaries(
        std::vector<HistoryOverviewSummary>* summaries,
        std::string* error_message = nullptr) const;
    // 按导出筛选条件分页读取 history_samples 原始业务记录。
    StatusCode export_records(
        const HistoryExportQuery& query,
        std::vector<HistoryRecord>* records,
        std::string* error_message = nullptr) const;
    // 从稳定游标之后读取下一批；仅供服务内流式扫描，query.offset 在此接口中忽略。
    StatusCode export_records_after(
        const HistoryExportQuery& query,
        const HistoryExportCursor* cursor,
        std::vector<HistoryRecord>* records,
        HistoryExportCursor* next_cursor,
        std::string* error_message = nullptr) const;
    // 清理过期。
    StatusCode cleanup_expired(HistoryCleanupResult* result, std::string* error_message = nullptr);
    // 统计按周期。
    StatusCode count_by_period(const std::string& sample_period, std::uint64_t* count, std::string* error_message = nullptr) const;
    // 立即刷新历史数据库 WAL；超期清理由统一数据维护任务负责。
    void save_now();
    // 清空全部历史趋势数据。
    StatusCode clear_all(std::string* error_message = nullptr);
    // 返回历史数据库文件路径。
    std::string database_path() const;

private:
    friend struct HistoryStoreTestPeer;

    static constexpr std::size_t kReadConnectionCount = 2;

    struct ReadConnection {
        mutable std::mutex mutex;
        ::sqlite3* database{nullptr};
        ~ReadConnection();
    };

    struct ReadLease {
        std::unique_lock<std::mutex> lifecycle_lock;
        std::shared_ptr<ReadConnection> connection;
        std::unique_lock<std::mutex> connection_lock;
        ::sqlite3* database{nullptr};
    };

    // 打开历史 SQLite 数据库。
    StatusCode open_database_locked(const std::string& database_path, std::string* error_message);
    // 为 WAL 文件打开小型 query_only 只读连接组；内存库保持单连接语义。
    StatusCode open_read_connections_locked(std::string* error_message);
    // 关闭全部只读连接。调用方必须持有主连接生命周期锁。
    void close_read_connections_locked();
    // 短暂持有主锁选择只读连接，随后仅持有该连接自己的锁。
    StatusCode acquire_read_lease(ReadLease* lease, std::string* error_message) const;
    // 初始化历史表和索引结构。
    StatusCode initialize_schema_locked(std::string* error_message);
    // 判断持久化聚合状态是否要求启动重建（缺失、版本变化或 dirty 都要求重建）。
    StatusCode aggregate_rebuild_required_locked(bool* required, std::string* error_message) const;
    // 在当前事务中原子写入聚合 dirty/clean 状态。
    StatusCode set_aggregate_state_locked(bool dirty, TimestampMs now_ms, std::string* error_message);
    // 只在 marker 要求时执行全窗口重建，成功提交时同步清理 dirty。
    StatusCode rebuild_aggregates_if_needed_locked(TimestampMs now_ms, bool* rebuilt, std::string* error_message);
    // 根据当前基础样本刷新对应小时点和天点；聚合失败由调用方记录并在后续样本写入时重试。
    StatusCode refresh_aggregates_locked(
        const std::vector<HistoryRecord>& raw_records,
        TimestampMs now_ms,
        std::string* error_message);
    // 将一批受影响时间桶暂存后，通过单次 GROUP BY/UPSERT 刷新一层聚合。
    StatusCode refresh_aggregate_buckets_locked(
        const std::vector<HistoryRecord>& sources,
        const std::string& source_period,
        const std::string& input_period,
        const std::string& output_period,
        TimestampMs now_ms,
        std::string* error_message);
    // 在持锁状态下重建近期聚合记录。
    StatusCode rebuild_recent_aggregates_locked(TimestampMs now_ms, std::string* error_message);
    // 导出查询公共实现；cursor 非空且有效时采用 keyset，否则保留公开 OFFSET 语义。
    StatusCode export_records_page(
        const HistoryExportQuery& query,
        const HistoryExportCursor* cursor,
        bool use_offset,
        std::vector<HistoryRecord>* records,
        HistoryExportCursor* next_cursor,
        std::string* error_message) const;
    // 清理超过保留周期的历史数据。
    StatusCode cleanup_expired_locked(TimestampMs now_ms, std::string* error_message, HistoryCleanupResult* result = nullptr);
    // 在当前数据库连接上执行 SQL。
    StatusCode execute_sql_locked(const char* sql, std::string* error_message) const;
    // 检查历史数据库是否可用。
    bool database_available_locked(std::string* error_message) const;
    // 批量写入前在连接失效时复用完整打开流程尝试恢复。
    StatusCode ensure_database_for_write_locked(std::string* error_message);
    // 关闭历史数据库连接。
    void close_database_locked();
    // 事务结束失败后恢复 autocommit；无法可信恢复时重建当前连接。
    void recover_failed_transaction_locked(const std::string& original_error);

    // 主连接只负责写事务和生命周期；查询只短暂持本锁后转入独立只读连接。
    mutable std::mutex mutex_;
    ::sqlite3* database_{nullptr};
    mutable std::array<std::shared_ptr<ReadConnection>, kReadConnectionCount> read_connections_{};
    mutable std::size_t next_read_connection_{0};
    std::string database_path_;
    std::string last_error_message_;
    std::chrono::steady_clock::time_point last_cleanup_time_{};
    std::chrono::steady_clock::time_point last_reopen_failure_log_time_{};
    bool initialized_{false};
};

// 返回当前历史数据库日期。
std::string current_history_date();
// 根据时间戳计算历史数据库日期。
std::string history_date_from_timestamp(TimestampMs timestamp_ms);

}  // namespace edge_controller
