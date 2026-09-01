// 历史数据仓库：按采样粒度写入和分页读取 SQLite，所有 SQL 参数均使用绑定值。
#include "datastore/history_store.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/filesystem_compat.h"
#include "common/logger.h"
#include "common/sqlite_compat.h"
#include "common/time_utils.h"
#include "model/device_template.h"
#include "datastore/database_paths.h"
#include "datastore/sqlite_helpers.h"
#include "model/data_item_keys.h"

namespace edge_controller {

namespace {

using sqlite_helpers::Statement;
using sqlite_helpers::bind_double;
using sqlite_helpers::bind_int;
using sqlite_helpers::bind_int64;
using sqlite_helpers::bind_text;
using sqlite_helpers::column_text;
using sqlite_helpers::schema_migration_required_message;
using sqlite_helpers::sqlite_error;

using HistoryEnabledPointCache =
    std::unordered_map<std::string, std::unordered_set<std::string>>;

// 在单次启动补算内按模板缓存历史启用点位，避免每条记录重复获取模板并线性扫描字段。
bool history_record_enabled_by_current_template(
    const HistoryRecord& record,
    HistoryEnabledPointCache* cache)
{
    auto [points, inserted] = cache->try_emplace(record.template_id);
    if (inserted) {
        const auto definition = find_device_template(record.template_id);
        if (definition != nullptr) {
            points->second.reserve(definition->fields.size());
            for (const auto& field : definition->fields) {
                if (device_template_field_history_enabled(field)) {
                    points->second.insert(field.field_key);
                }
            }
        }
    }
    return points->second.find(record.point_key) != points->second.end();
}

constexpr int kRawHistoryRetentionHours = 72;
constexpr int kDayHistoryRetentionDays = 30;
constexpr int kHourHistoryRetentionDays = 35;
constexpr TimestampMs kRawHistoryRetentionMs =
    static_cast<TimestampMs>(kRawHistoryRetentionHours) * 60ULL * 60ULL * 1000ULL;
constexpr TimestampMs kDayHistoryRetentionMs =
    static_cast<TimestampMs>(kDayHistoryRetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr TimestampMs kHourHistoryRetentionMs =
    static_cast<TimestampMs>(kHourHistoryRetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr TimestampMs kDefaultDayQueryRangeMs =
    static_cast<TimestampMs>(kDayHistoryRetentionDays) * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr TimestampMs kDefaultHourQueryRangeMs = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr std::uint32_t kDefaultExportLimit = 500;
constexpr std::uint32_t kMaxExportLimit = 1000;
constexpr std::uint32_t kStartupAggregateBatchSize = 4096;
constexpr int kCurrentHistoryDatabaseVersion = 1;
constexpr int kCurrentAggregateStateVersion = 1;
constexpr int kHistoryReadBusyTimeoutMs = 5000;
constexpr auto kHistoryReopenFailureLogInterval = std::chrono::seconds(60);

// 拼接错误信息。
std::string join_error(const std::string& prefix, const std::string& detail)
{
    return detail.empty() ? prefix : prefix + ": " + detail;
}

// 根据当前时间和范围计算截止时间戳。
TimestampMs cutoff_ms(TimestampMs now_ms, TimestampMs range_ms)
{
    return now_ms > range_ms ? now_ms - range_ms : 0;
}

// 根据历史记录生成可读上下文。
std::string record_context(const HistoryRecord& record)
{
    return "channel_id=" + record.channel_id +
           ", master_id=" + record.master_id +
           ", device_id=" + record.device_id +
           ", point_key=" + record.point_key +
           ", sample_period=" + record.sample_period;
}

std::tm local_tm_from_time_t(std::time_t value)
{
    std::tm result{};
#if defined(_WIN32)
    localtime_s(&result, &value);
#else
    localtime_r(&value, &result);
#endif
    return result;
}

std::string format_local_date(std::chrono::system_clock::time_point time_point)
{
    const auto time_value = std::chrono::system_clock::to_time_t(time_point);
    const auto local_tm = local_tm_from_time_t(time_value);
    std::ostringstream stream;
    stream << std::put_time(&local_tm, "%Y-%m-%d");
    return stream.str();
}

std::string date_from_timestamp_ms(TimestampMs timestamp_ms)
{
    return format_local_date(std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms)));
}

bool is_valid_date_text(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

std::string normalize_sample_period(const std::string& value)
{
    return value == "raw_10min" || value == "hour" ? value : "day";
}

bool is_valid_sample_period(const std::string& value)
{
    return value == "raw_10min" || value == "hour" || value == "day";
}

TimestampMs bucket_start_for_period(TimestampMs timestamp_ms, const std::string& period)
{
    const auto time_point = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp_ms));
    auto local_tm = local_tm_from_time_t(std::chrono::system_clock::to_time_t(time_point));
    local_tm.tm_sec = 0;
    if (period == "raw_10min") local_tm.tm_min = (local_tm.tm_min / 10) * 10;
    else {
        local_tm.tm_min = 0;
        if (period == "day") local_tm.tm_hour = 0;
    }
    const auto bucket_time = std::mktime(&local_tm);
    return bucket_time < 0 ? 0 : static_cast<TimestampMs>(bucket_time) * 1000ULL;
}

std::string bucket_text_for_period(TimestampMs bucket_start_ms, const std::string& period)
{
    const auto time_point = std::chrono::system_clock::time_point(std::chrono::milliseconds(bucket_start_ms));
    const auto local_tm = local_tm_from_time_t(std::chrono::system_clock::to_time_t(time_point));
    std::ostringstream stream;
    stream << std::put_time(
        &local_tm,
        period == "day" ? "%Y-%m-%d" : (period == "hour" ? "%Y-%m-%d %H:00" : "%Y-%m-%d %H:%M"));
    return stream.str();
}

std::uint32_t normalize_export_limit(std::uint32_t value)
{
    return value == 0 ? kDefaultExportLimit : std::min(value, kMaxExportLimit);
}

// 确保数据库文件的父目录存在。
StatusCode ensure_parent_directory(const edge::fs::path& path, std::string* error_message)
{
    if (!path.has_parent_path()) {
        return StatusCode::kOk;
    }

    std::error_code create_error;
    edge::fs::create_directories(path.parent_path(), create_error);
    if (create_error) {
        if (error_message != nullptr) {
            *error_message = "创建历史 SQLite 数据库目录失败：" + path.parent_path().string() +
                             "，原因=" + create_error.message();
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 读取历史数据记录。
HistoryRecord read_history_record(sqlite3_stmt* statement)
{
    HistoryRecord record;
    record.device_id = column_text(statement, 0);
    record.master_id = column_text(statement, 1);
    record.channel_id = column_text(statement, 2);
    record.template_id = column_text(statement, 3);
    record.sample_period = column_text(statement, 4);
    record.bucket_start_ms = static_cast<TimestampMs>(sqlite3_column_int64(statement, 5));
    record.bucket_text = column_text(statement, 6);
    record.timestamp_ms = static_cast<TimestampMs>(sqlite3_column_int64(statement, 7));
    record.date = column_text(statement, 8);
    record.point_key = column_text(statement, 9);
    record.point_name = column_text(statement, 10);
    record.unit = column_text(statement, 11);
    record.precision = static_cast<std::uint32_t>(sqlite3_column_int(statement, 12));
    record.value = sqlite3_column_double(statement, 13);
    record.raw_value = sqlite3_column_double(statement, 14);
    record.quality = column_text(statement, 15);
    record.valid = sqlite3_column_int(statement, 16) != 0;
    record.message = column_text(statement, 17);
    return record;
}

}  // namespace

// 返回当前历史数据库日期。
std::string current_history_date()
{
    return format_local_date(std::chrono::system_clock::now());
}

// 根据时间戳计算历史数据库日期。
std::string history_date_from_timestamp(TimestampMs timestamp_ms)
{
    if (timestamp_ms == 0) {
        return current_history_date();
    }
    return date_from_timestamp_ms(timestamp_ms);
}

// 关闭数据库连接并释放存储资源；内部加锁保证析构安全。
HistoryStore::ReadConnection::~ReadConnection()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (database != nullptr) {
        sqlite3_close(database);
        database = nullptr;
    }
}

HistoryStore::~HistoryStore()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_database_locked();
}

// 初始化历史数据库连接和表结构。
StatusCode HistoryStore::initialize(const std::string& database_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_database_locked();
    database_path_.clear();
    last_error_message_.clear();
    last_cleanup_time_ = {};
    last_reopen_failure_log_time_ = {};
    initialized_ = false;

    const auto resolved_path = database_path.empty()
                                   ? DatabasePaths(std::string{}).history_database()
                                   : database_path;
    std::string error_message;
    const auto status = open_database_locked(resolved_path, &error_message);
    if (is_ok(status)) {
        initialized_ = true;
        last_error_message_.clear();
        Logger::info("历史 SQLite 存储初始化完成，路径=" + database_path_);
        return StatusCode::kOk;
    }

    last_error_message_ = error_message.empty()
                              ? "历史 SQLite 数据库初始化失败"
                              : error_message;
    Logger::warn(last_error_message_);
    close_database_locked();
    return status;
}

// 批量写入或更新历史趋势记录。
StatusCode HistoryStore::upsert_records(const std::vector<HistoryRecord>& records)
{
    if (records.empty()) {
        return StatusCode::kOk;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::string error_message;
    const auto database_status = ensure_database_for_write_locked(&error_message);
    if (!is_ok(database_status)) {
        return database_status;
    }

    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", &error_message);
    if (!is_ok(begin_status)) {
        Logger::error("历史采集数据写入失败，开始事务失败：" + error_message);
        return begin_status;
    }

    const char* upsert_sql =
        "INSERT OR REPLACE INTO history_samples "
        "(sample_period, bucket_start_ms, bucket_text, timestamp_ms, date_text, "
        "channel_id, master_id, device_id, template_id, point_key, point_name, unit, "
        "precision, value, raw_value, quality, valid, message, created_at_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    bool write_failed = false;
    std::vector<HistoryRecord> accepted_records;
    accepted_records.reserve(records.size());
    const auto now_ms = time_utils::system_now_ms();
    {
        // 连接可能在事务结束失败后被重建，因此写入语句必须先在本作用域结束并 finalize。
        Statement statement(database_, upsert_sql);
        if (!statement.ok()) {
            error_message = sqlite_error(database_);
            execute_sql_locked("ROLLBACK;", nullptr);
            Logger::error("历史采集数据写入失败，准备写入语句失败：" + error_message);
            return StatusCode::kIoError;
        }

        for (const auto& source_record : records) {
        // 写入前只补齐时间和日期；质量必须由采样链路明确给出，空质量不能提升为 good。
        HistoryRecord record = source_record;
        if (record.timestamp_ms == 0) {
            record.timestamp_ms = now_ms;
        }
        if (record.date.empty()) {
            record.date = history_date_from_timestamp(record.timestamp_ms);
        }
        if (record.device_id.empty() ||
            record.master_id.empty() ||
            record.channel_id.empty() ||
            record.point_key.empty() ||
            record.bucket_start_ms == 0 ||
            record.bucket_text.empty() ||
            !is_valid_sample_period(record.sample_period) ||
            !is_valid_date_text(record.date) ||
            !std::isfinite(record.value) ||
            !std::isfinite(record.raw_value) ||
            !record.valid || record.quality != "good") {
            Logger::warn("跳过无效历史趋势记录：" + record_context(record) + "，桶、点位、日期或数值非法");
            continue;
        }

        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());

        const bool bind_ok =
            bind_text(statement.get(), 1, record.sample_period) &&
            bind_int64(statement.get(), 2, record.bucket_start_ms) &&
            bind_text(statement.get(), 3, record.bucket_text) &&
            bind_int64(statement.get(), 4, record.timestamp_ms) &&
            bind_text(statement.get(), 5, record.date) &&
            bind_text(statement.get(), 6, record.channel_id) &&
            bind_text(statement.get(), 7, record.master_id) &&
            bind_text(statement.get(), 8, record.device_id) &&
            bind_text(statement.get(), 9, record.template_id) &&
            bind_text(statement.get(), 10, record.point_key) &&
            bind_text(statement.get(), 11, record.point_name) &&
            bind_text(statement.get(), 12, record.unit) &&
            bind_int(statement.get(), 13, static_cast<int>(record.precision)) &&
            bind_double(statement.get(), 14, record.value) &&
            bind_double(statement.get(), 15, record.raw_value) &&
            bind_text(statement.get(), 16, record.quality) &&
            bind_int(statement.get(), 17, record.valid ? 1 : 0) &&
            bind_text(statement.get(), 18, record.message) &&
            bind_int64(statement.get(), 19, now_ms);
        if (!bind_ok) {
            write_failed = true;
            error_message = sqlite_error(database_);
            Logger::error("历史采集数据写入失败，绑定参数失败：" + record_context(record) + "，原因=" + error_message);
            break;
        }

        const auto step_status = sqlite3_step(statement.get());
        if (step_status != SQLITE_DONE) {
            write_failed = true;
            error_message = sqlite_error(database_);
            Logger::error("历史采集数据写入失败：" + record_context(record) + "，原因=" + error_message);
            break;
        }
            accepted_records.push_back(std::move(record));
        }
    }

    if (!write_failed) {
        const auto aggregate_status = refresh_aggregates_locked(
            accepted_records, now_ms, &error_message);
        if (!is_ok(aggregate_status)) {
            // 基础样本仍可提交，但必须在同一事务中持久化 dirty；若 marker 也写失败，
            // 回滚整批，不能留下普通重启会误判为 clean 的基础样本。
            const auto aggregate_error = error_message;
            std::string marker_error;
            const auto marker_status = set_aggregate_state_locked(true, now_ms, &marker_error);
            if (!is_ok(marker_status)) {
                write_failed = true;
                error_message = join_error(
                    "历史聚合失败后写入重建标记失败",
                    join_error(aggregate_error, marker_error));
            } else {
                Logger::error("历史趋势聚合刷新失败，已标记为启动重建：" + aggregate_error);
            }
        }
    }

    const auto finish_status = execute_sql_locked(write_failed ? "ROLLBACK;" : "COMMIT;", &error_message);
    if (!is_ok(finish_status)) {
        const auto original_error = error_message;
        Logger::error(std::string("历史采集数据写入失败，") + (write_failed ? "回滚" : "提交") + "事务失败：" + original_error);
        recover_failed_transaction_locked(original_error);
        return finish_status;
    }
    return write_failed ? StatusCode::kIoError : StatusCode::kOk;
}

// 按导出筛选条件分页读取原始历史样本。
StatusCode HistoryStore::export_records(
    const HistoryExportQuery& query,
    std::vector<HistoryRecord>* records,
    std::string* error_message) const
{
    return export_records_page(query, nullptr, true, records, nullptr, error_message);
}

// 使用稳定排序键读取下一批导出记录；公开 offset 参数保持不变，仅服务内扫描改走本接口。
StatusCode HistoryStore::export_records_after(
    const HistoryExportQuery& query,
    const HistoryExportCursor* cursor,
    std::vector<HistoryRecord>* records,
    HistoryExportCursor* next_cursor,
    std::string* error_message) const
{
    if (next_cursor == nullptr) {
        if (error_message != nullptr) {
            *error_message = "历史数据导出游标输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    return export_records_page(query, cursor, false, records, next_cursor, error_message);
}

StatusCode HistoryStore::export_records_page(
    const HistoryExportQuery& query,
    const HistoryExportCursor* cursor,
    bool use_offset,
    std::vector<HistoryRecord>* records,
    HistoryExportCursor* next_cursor,
    std::string* error_message) const
{
    if (records == nullptr) {
        if (error_message != nullptr) {
            *error_message = "历史数据导出输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    records->clear();
    if (!query.sample_period.empty() && !is_valid_sample_period(query.sample_period)) {
        if (error_message != nullptr) {
            *error_message = "历史采样粒度非法，应为 raw_10min、hour 或 day";
        }
        return StatusCode::kInvalidArgument;
    }
    if (next_cursor != nullptr) {
        *next_cursor = {};
    }

    ReadLease lease;
    const auto lease_status = acquire_read_lease(&lease, error_message);
    if (!is_ok(lease_status)) return lease_status;
    auto* query_database = lease.database;

    std::string query_sql =
        "SELECT device_id, master_id, channel_id, template_id, sample_period, "
        "bucket_start_ms, bucket_text, timestamp_ms, date_text, point_key, point_name, unit, "
        "precision, value, raw_value, quality, valid, message "
        "FROM history_samples WHERE 1=1";
    if (!query.channel_id.empty()) {
        query_sql += " AND channel_id = ?";
    }
    if (!query.master_id.empty()) {
        query_sql += " AND master_id = ?";
    }
    if (!query.device_id.empty()) {
        query_sql += " AND device_id = ?";
    }
    if (!query.point_key.empty()) {
        query_sql += " AND point_key = ?";
    }
    if (!query.sample_period.empty()) {
        query_sql += " AND sample_period = ?";
    }
    const bool has_cursor = cursor != nullptr && cursor->valid;
    if (has_cursor) {
        query_sql +=
            " AND (bucket_start_ms, channel_id, master_id, device_id, point_key, sample_period) "
            "> (?, ?, ?, ?, ?, ?)";
    }
    query_sql +=
        " ORDER BY bucket_start_ms ASC, channel_id ASC, master_id ASC, device_id ASC, point_key ASC, sample_period ASC, id ASC "
        "LIMIT ?";
    if (use_offset) {
        query_sql += " OFFSET ?";
    }
    query_sql += ";";

    Statement statement(query_database, query_sql.c_str());
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = join_error("准备历史数据导出查询语句失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    int bind_index = 1;
    bool bind_ok = true;
    if (!query.channel_id.empty()) {
        bind_ok = bind_ok && bind_text(statement.get(), bind_index++, query.channel_id);
    }
    if (!query.master_id.empty()) {
        bind_ok = bind_ok && bind_text(statement.get(), bind_index++, query.master_id);
    }
    if (!query.device_id.empty()) {
        bind_ok = bind_ok && bind_text(statement.get(), bind_index++, query.device_id);
    }
    if (!query.point_key.empty()) {
        bind_ok = bind_ok && bind_text(statement.get(), bind_index++, query.point_key);
    }
    if (!query.sample_period.empty()) {
        bind_ok = bind_ok && bind_text(statement.get(), bind_index++, query.sample_period);
    }
    if (has_cursor) {
        bind_ok = bind_ok &&
                  bind_int64(statement.get(), bind_index++, cursor->bucket_start_ms) &&
                  bind_text(statement.get(), bind_index++, cursor->channel_id) &&
                  bind_text(statement.get(), bind_index++, cursor->master_id) &&
                  bind_text(statement.get(), bind_index++, cursor->device_id) &&
                  bind_text(statement.get(), bind_index++, cursor->point_key) &&
                  bind_text(statement.get(), bind_index++, cursor->sample_period);
    }
    bind_ok = bind_ok && bind_int64(
        statement.get(), bind_index++, normalize_export_limit(query.limit));
    if (use_offset) {
        bind_ok = bind_ok && bind_int64(statement.get(), bind_index++, query.offset);
    }
    if (!bind_ok) {
        if (error_message != nullptr) {
            *error_message = join_error("绑定历史数据导出查询参数失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    while (true) {
        const auto step_status = sqlite3_step(statement.get());
        if (step_status == SQLITE_ROW) {
            auto record = read_history_record(statement.get());
            if (next_cursor != nullptr) {
                next_cursor->bucket_start_ms = record.bucket_start_ms;
                next_cursor->channel_id = record.channel_id;
                next_cursor->master_id = record.master_id;
                next_cursor->device_id = record.device_id;
                next_cursor->point_key = record.point_key;
                next_cursor->sample_period = record.sample_period;
                // unique(device_id, point_key, sample_period, bucket_start_ms) 保证该排序前缀唯一；
                // 不把 INSERT OR REPLACE 会改变的 rowid 放入游标，避免同一桶被更新后重复导出。
                next_cursor->valid = true;
            }
            records->push_back(std::move(record));
            continue;
        }
        if (step_status == SQLITE_DONE) {
            break;
        }
        if (error_message != nullptr) {
            *error_message = join_error("读取历史数据导出记录失败", sqlite_error(query_database));
        }
        records->clear();
        return StatusCode::kIoError;
    }

    return StatusCode::kOk;
}

// 按设备和查询条件读取历史趋势记录。
StatusCode HistoryStore::get_device_history(
    const DeviceId& device_id,
    const HistoryQueryOptions& options,
    std::vector<HistoryRecord>* records,
    std::string* error_message) const
{
    if (records == nullptr) {
        if (error_message != nullptr) {
            *error_message = "历史数据输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    records->clear();
    if (device_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "设备ID不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    const auto requested_sample_period = options.sample_period.empty() ? std::string("day") : options.sample_period;
    if (!is_valid_sample_period(requested_sample_period)) {
        if (error_message != nullptr) {
            *error_message = "历史采样粒度非法，应为 raw_10min、hour 或 day";
        }
        return StatusCode::kInvalidArgument;
    }
    const auto sample_period = normalize_sample_period(requested_sample_period);

    if (sample_period == "day" && !options.start_date.empty() && !is_valid_date_text(options.start_date)) {
        if (error_message != nullptr) {
            *error_message = "起始日期格式非法，应为 YYYY-MM-DD";
        }
        return StatusCode::kInvalidArgument;
    }
    if (sample_period == "day" && !options.end_date.empty() && !is_valid_date_text(options.end_date)) {
        if (error_message != nullptr) {
            *error_message = "结束日期格式非法，应为 YYYY-MM-DD";
        }
        return StatusCode::kInvalidArgument;
    }
    if (sample_period == "day" &&
        !options.start_date.empty() &&
        !options.end_date.empty() &&
        options.start_date > options.end_date) {
        if (error_message != nullptr) {
            *error_message = "起始日期不能晚于结束日期";
        }
        return StatusCode::kInvalidArgument;
    }

    ReadLease lease;
    const auto lease_status = acquire_read_lease(&lease, error_message);
    if (!is_ok(lease_status)) return lease_status;
    auto* query_database = lease.database;

    std::string point_key = options.point_key;
    if (point_key.empty()) {
        // 默认优先展示接地电阻，符合当前设备历史页的主要观察对象；没有电阻则回退到首个点位。
        const char* resistance_sql =
            "SELECT 1 FROM history_samples "
            "WHERE device_id = ? AND sample_period = ? AND point_key = ? "
            "LIMIT 1;";
        Statement resistance_statement(query_database, resistance_sql);
        if (!resistance_statement.ok()) {
            if (error_message != nullptr) {
                *error_message = join_error("准备默认历史点位查询语句失败", sqlite_error(query_database));
            }
            return StatusCode::kIoError;
        }
        if (!bind_text(resistance_statement.get(), 1, device_id) ||
            !bind_text(resistance_statement.get(), 2, sample_period) ||
            !bind_text(resistance_statement.get(), 3, kResistanceFieldKey)) {
            if (error_message != nullptr) {
                *error_message = join_error("绑定默认历史点位查询参数失败", sqlite_error(query_database));
            }
            return StatusCode::kIoError;
        }
        const auto resistance_status = sqlite3_step(resistance_statement.get());
        if (resistance_status == SQLITE_ROW) {
            point_key = kResistanceFieldKey;
        } else if (resistance_status != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = join_error("读取默认历史点位失败", sqlite_error(query_database));
            }
            return StatusCode::kIoError;
        }
    }

    if (point_key.empty()) {
        const char* first_point_sql =
            "SELECT point_key FROM history_samples "
            "WHERE device_id = ? AND sample_period = ? "
            "ORDER BY bucket_start_ms ASC, point_key ASC "
            "LIMIT 1;";
        Statement point_statement(query_database, first_point_sql);
        if (!point_statement.ok()) {
            if (error_message != nullptr) {
                *error_message = join_error("准备首个历史点位查询语句失败", sqlite_error(query_database));
            }
            return StatusCode::kIoError;
        }
        if (!bind_text(point_statement.get(), 1, device_id) ||
            !bind_text(point_statement.get(), 2, sample_period)) {
            if (error_message != nullptr) {
                *error_message = join_error("绑定首个历史点位查询参数失败", sqlite_error(query_database));
            }
            return StatusCode::kIoError;
        }
        const auto point_status = sqlite3_step(point_statement.get());
        if (point_status == SQLITE_ROW) {
            point_key = column_text(point_statement.get(), 0);
        } else if (point_status != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = join_error("读取首个历史点位失败", sqlite_error(query_database));
            }
            return StatusCode::kIoError;
        }
    }

    if (point_key.empty()) {
        return StatusCode::kOk;
    }

    std::string query_sql =
        "SELECT device_id, master_id, channel_id, template_id, sample_period, "
        "bucket_start_ms, bucket_text, timestamp_ms, date_text, point_key, point_name, unit, "
        "precision, value, raw_value, quality, valid, message "
        "FROM history_samples "
        "WHERE device_id = ? AND sample_period = ? AND point_key = ?";
    const bool has_date_range =
        sample_period == "day" && (!options.start_date.empty() || !options.end_date.empty());
    if (has_date_range) {
        // 日粒度支持明确日期范围；小时粒度默认只看最近窗口，控制查询量。
        if (!options.start_date.empty()) {
            query_sql += " AND date_text >= ?";
        }
        if (!options.end_date.empty()) {
            query_sql += " AND date_text <= ?";
        }
    } else {
        query_sql += " AND bucket_start_ms >= ?";
    }
    query_sql += " ORDER BY bucket_start_ms ASC;";

    Statement statement(query_database, query_sql.c_str());
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = join_error("准备历史数据查询语句失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    int bind_index = 1;
    bool bind_ok =
        bind_text(statement.get(), bind_index++, device_id) &&
        bind_text(statement.get(), bind_index++, sample_period) &&
        bind_text(statement.get(), bind_index++, point_key);
    if (has_date_range) {
        if (!options.start_date.empty()) {
            bind_ok = bind_ok && bind_text(statement.get(), bind_index++, options.start_date);
        }
        if (!options.end_date.empty()) {
            bind_ok = bind_ok && bind_text(statement.get(), bind_index++, options.end_date);
        }
    } else {
        const TimestampMs range_ms =
            sample_period == "hour"
                ? kDefaultHourQueryRangeMs
                : (options.days == 0
                       ? kDefaultDayQueryRangeMs
                       : static_cast<TimestampMs>(options.days) * 24ULL * 60ULL * 60ULL * 1000ULL);
        const auto now_ms = time_utils::system_now_ms();
        bind_ok = bind_ok && bind_int64(statement.get(), bind_index++, cutoff_ms(now_ms, range_ms));
    }
    if (!bind_ok) {
        if (error_message != nullptr) {
            *error_message = join_error("绑定历史数据查询参数失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    while (true) {
        const auto step_status = sqlite3_step(statement.get());
        if (step_status == SQLITE_ROW) {
            HistoryRecord record;
            record.device_id = column_text(statement.get(), 0);
            record.master_id = column_text(statement.get(), 1);
            record.channel_id = column_text(statement.get(), 2);
            record.template_id = column_text(statement.get(), 3);
            record.sample_period = column_text(statement.get(), 4);
            record.bucket_start_ms = static_cast<TimestampMs>(sqlite3_column_int64(statement.get(), 5));
            record.bucket_text = column_text(statement.get(), 6);
            record.timestamp_ms = static_cast<TimestampMs>(sqlite3_column_int64(statement.get(), 7));
            record.date = column_text(statement.get(), 8);
            record.point_key = column_text(statement.get(), 9);
            record.point_name = column_text(statement.get(), 10);
            record.unit = column_text(statement.get(), 11);
            record.precision = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 12));
            record.value = sqlite3_column_double(statement.get(), 13);
            record.raw_value = sqlite3_column_double(statement.get(), 14);
            record.quality = column_text(statement.get(), 15);
            record.valid = sqlite3_column_int(statement.get(), 16) != 0;
            record.message = column_text(statement.get(), 17);
            records->push_back(std::move(record));
            continue;
        }
        if (step_status == SQLITE_DONE) {
            break;
        }
        if (error_message != nullptr) {
            *error_message = join_error("读取历史数据失败", sqlite_error(query_database));
        }
        records->clear();
        return StatusCode::kIoError;
    }

    return StatusCode::kOk;
}

StatusCode HistoryStore::get_history_overview_summaries(
    std::vector<HistoryOverviewSummary>* summaries,
    std::string* error_message) const
{
    if (summaries == nullptr) {
        if (error_message != nullptr) {
            *error_message = "历史总览摘要输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    summaries->clear();

    ReadLease lease;
    const auto lease_status = acquire_read_lease(&lease, error_message);
    if (!is_ok(lease_status)) return lease_status;
    auto* query_database = lease.database;

    const char* sql =
        "WITH aggregate_rows AS ("
        " SELECT device_id, point_key, sample_period, COUNT(*) AS record_count,"
        "        MAX(bucket_start_ms) AS latest_bucket_start_ms"
        " FROM history_samples"
        " WHERE (sample_period = 'day' AND bucket_start_ms >= ?)"
        "    OR (sample_period = 'hour' AND bucket_start_ms >= ?)"
        " GROUP BY device_id, point_key, sample_period"
        ")"
        " SELECT latest.device_id, latest.master_id, latest.channel_id,"
        "        latest.point_key, latest.point_name, latest.unit, latest.precision,"
        "        latest.sample_period, aggregate_rows.record_count, latest.value,"
        "        latest.timestamp_ms, latest.bucket_start_ms"
        " FROM aggregate_rows"
        " JOIN history_samples AS latest"
        "   ON latest.device_id = aggregate_rows.device_id"
        "  AND latest.point_key = aggregate_rows.point_key"
        "  AND latest.sample_period = aggregate_rows.sample_period"
        "  AND latest.bucket_start_ms = aggregate_rows.latest_bucket_start_ms"
        " ORDER BY latest.device_id, latest.point_key, latest.sample_period;";
    Statement statement(query_database, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = join_error("准备历史总览摘要查询失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    const auto now_ms = time_utils::system_now_ms();
    if (!bind_int64(statement.get(), 1, cutoff_ms(now_ms, kDefaultDayQueryRangeMs)) ||
        !bind_int64(statement.get(), 2, cutoff_ms(now_ms, kDefaultHourQueryRangeMs))) {
        if (error_message != nullptr) {
            *error_message = join_error("绑定历史总览摘要查询参数失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    while (true) {
        const auto step_status = sqlite3_step(statement.get());
        if (step_status == SQLITE_ROW) {
            HistoryOverviewSummary summary;
            summary.device_id = column_text(statement.get(), 0);
            summary.master_id = column_text(statement.get(), 1);
            summary.channel_id = column_text(statement.get(), 2);
            summary.point_key = column_text(statement.get(), 3);
            summary.point_name = column_text(statement.get(), 4);
            summary.unit = column_text(statement.get(), 5);
            summary.precision = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 6));
            summary.sample_period = column_text(statement.get(), 7);
            summary.record_count = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 8));
            summary.latest_value = sqlite3_column_double(statement.get(), 9);
            summary.latest_timestamp_ms = static_cast<TimestampMs>(sqlite3_column_int64(statement.get(), 10));
            summary.latest_bucket_start_ms = static_cast<TimestampMs>(sqlite3_column_int64(statement.get(), 11));
            summaries->push_back(std::move(summary));
            continue;
        }
        if (step_status == SQLITE_DONE) {
            return StatusCode::kOk;
        }
        if (error_message != nullptr) {
            *error_message = join_error("读取历史总览摘要失败", sqlite_error(query_database));
        }
        summaries->clear();
        return StatusCode::kIoError;
    }
}

// 读取指定设备默认范围的历史趋势记录。
StatusCode HistoryStore::get_device_history(
    const DeviceId& device_id,
    std::vector<HistoryRecord>* records,
    std::string* error_message) const
{
    return get_device_history(device_id, HistoryQueryOptions{}, records, error_message);
}

// 查询指定设备已有历史数据项列表。
StatusCode HistoryStore::get_device_history_points(
    const DeviceId& device_id,
    const std::string& sample_period,
    std::vector<HistoryPointSummary>* points,
    std::string* error_message) const
{
    if (points == nullptr) {
        if (error_message != nullptr) {
            *error_message = "历史点位输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    points->clear();
    if (device_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "设备ID不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    const auto requested_period = sample_period.empty() ? std::string("day") : sample_period;
    if (!is_valid_sample_period(requested_period)) {
        if (error_message != nullptr) {
            *error_message = "历史采样粒度非法，应为 raw_10min、hour 或 day";
        }
        return StatusCode::kInvalidArgument;
    }
    const auto normalized_period = normalize_sample_period(requested_period);

    ReadLease lease;
    const auto lease_status = acquire_read_lease(&lease, error_message);
    if (!is_ok(lease_status)) return lease_status;
    auto* query_database = lease.database;

    const char* query_sql =
        "SELECT point_key, "
        "COALESCE(MAX(NULLIF(point_name, '')), point_key), "
        "COALESCE(MAX(unit), ''), "
        "MAX(precision), "
        "MIN(bucket_start_ms) "
        "FROM history_samples "
        "WHERE device_id = ? AND sample_period = ? "
        "GROUP BY point_key "
        "ORDER BY MIN(bucket_start_ms) ASC, point_key ASC;";
    Statement statement(query_database, query_sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = join_error("准备历史点位查询语句失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }
    if (!bind_text(statement.get(), 1, device_id) ||
        !bind_text(statement.get(), 2, normalized_period)) {
        if (error_message != nullptr) {
            *error_message = join_error("绑定历史点位查询参数失败", sqlite_error(query_database));
        }
        return StatusCode::kIoError;
    }

    while (true) {
        const auto step_status = sqlite3_step(statement.get());
        if (step_status == SQLITE_ROW) {
            HistoryPointSummary point;
            point.point_key = column_text(statement.get(), 0);
            point.point_name = column_text(statement.get(), 1);
            point.unit = column_text(statement.get(), 2);
            point.precision = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 3));
            if (!point.point_key.empty()) {
                points->push_back(std::move(point));
            }
            continue;
        }
        if (step_status == SQLITE_DONE) {
            break;
        }
        if (error_message != nullptr) {
            *error_message = join_error("读取历史点位失败", sqlite_error(query_database));
        }
        points->clear();
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 在持锁状态下刷新历史聚合记录。
StatusCode HistoryStore::refresh_aggregates_locked(
    const std::vector<HistoryRecord>& raw_records,
    TimestampMs now_ms,
    std::string* error_message)
{
    auto status = refresh_aggregate_buckets_locked(
        raw_records, "raw_10min", "raw_10min", "hour", now_ms, error_message);
    if (!is_ok(status)) return status;
    return refresh_aggregate_buckets_locked(
        raw_records, "raw_10min", "hour", "day", now_ms, error_message);
}

// 在持锁状态下批量刷新指定聚合层。
StatusCode HistoryStore::refresh_aggregate_buckets_locked(
    const std::vector<HistoryRecord>& sources,
    const std::string& source_period,
    const std::string& input_period,
    const std::string& output_period,
    TimestampMs now_ms,
    std::string* error_message)
{
    if (sources.empty()) return StatusCode::kOk;
    const TimestampMs bucket_duration_ms =
        output_period == "hour"
            ? 60ULL * 60ULL * 1000ULL
            : (output_period == "day" ? 24ULL * 60ULL * 60ULL * 1000ULL : 0ULL);
    if (bucket_duration_ms == 0) {
        if (error_message != nullptr) *error_message = "历史聚合输出周期非法";
        return StatusCode::kInvalidArgument;
    }

    // 每个输出桶只保留时间上最新的元数据来源；数值仍由下面的 SQL 读取完整输入桶。
    std::map<std::tuple<DeviceId, std::string, TimestampMs>, const HistoryRecord*> targets;
    for (const auto& source : sources) {
        if (source.sample_period != source_period ||
            source.device_id.empty() || source.point_key.empty() ||
            !source.valid || source.quality != "good" ||
            !std::isfinite(source.value) || !std::isfinite(source.raw_value)) {
            continue;
        }
        const auto bucket_start_ms =
            bucket_start_for_period(source.bucket_start_ms, output_period);
        if (bucket_start_ms == 0) continue;
        const auto key = std::make_tuple(
            source.device_id, source.point_key, bucket_start_ms);
        const auto found = targets.find(key);
        if (found == targets.end() ||
            source.bucket_start_ms >= found->second->bucket_start_ms) {
            targets.insert_or_assign(key, &source);
        }
    }
    if (targets.empty()) return StatusCode::kOk;

    std::string workspace_error;
    auto status = execute_sql_locked(
        "DELETE FROM temp_history_aggregate_targets;", &workspace_error);
    if (!is_ok(status)) {
        if (error_message != nullptr) {
            *error_message = join_error("清理历史聚合目标失败", workspace_error);
        }
        return status;
    }

    const char* target_sql =
        "INSERT OR REPLACE INTO temp_history_aggregate_targets "
        "(device_id, point_key, bucket_start_ms, bucket_end_ms, bucket_text, date_text, "
        "timestamp_ms, channel_id, master_id, template_id, point_name, unit, precision) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    Statement target_statement(database_, target_sql);
    if (!target_statement.ok()) {
        if (error_message != nullptr) {
            *error_message = join_error("准备历史聚合目标写入失败", sqlite_error(database_));
        }
        return StatusCode::kIoError;
    }
    for (const auto& entry : targets) {
        const auto& source = *entry.second;
        const auto bucket_start_ms = std::get<2>(entry.first);
        sqlite3_reset(target_statement.get());
        sqlite3_clear_bindings(target_statement.get());
        if (!bind_text(target_statement.get(), 1, source.device_id) ||
            !bind_text(target_statement.get(), 2, source.point_key) ||
            !bind_int64(target_statement.get(), 3, bucket_start_ms) ||
            !bind_int64(target_statement.get(), 4, bucket_start_ms + bucket_duration_ms) ||
            !bind_text(target_statement.get(), 5, bucket_text_for_period(bucket_start_ms, output_period)) ||
            !bind_text(target_statement.get(), 6, history_date_from_timestamp(bucket_start_ms)) ||
            !bind_int64(target_statement.get(), 7, source.timestamp_ms) ||
            !bind_text(target_statement.get(), 8, source.channel_id) ||
            !bind_text(target_statement.get(), 9, source.master_id) ||
            !bind_text(target_statement.get(), 10, source.template_id) ||
            !bind_text(target_statement.get(), 11, source.point_name) ||
            !bind_text(target_statement.get(), 12, source.unit) ||
            !bind_int(target_statement.get(), 13, static_cast<int>(source.precision)) ||
            sqlite3_step(target_statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = join_error("写入历史聚合目标失败", sqlite_error(database_));
            }
            return StatusCode::kIoError;
        }
    }

    // SQL 一次完成本层全部桶的去极值均值计算和 UPSERT，避免逐桶查询、prepare 与写入。
    const char* aggregate_sql =
        "WITH aggregate_rows AS ("
        " SELECT targets.bucket_start_ms, targets.bucket_text, targets.timestamp_ms, "
        " targets.date_text, targets.channel_id, targets.master_id, targets.device_id, "
        " targets.template_id, targets.point_key, targets.point_name, targets.unit, "
        " targets.precision, "
        " CASE WHEN COUNT(*) >= 5 THEN "
        "  (SUM(samples.value) - MIN(samples.value) - MAX(samples.value)) "
        "   / CAST(COUNT(*) - 2 AS REAL) "
        "  ELSE AVG(samples.value) END AS aggregate_value, "
        " COUNT(*) AS sample_count "
        " FROM temp_history_aggregate_targets AS targets "
        " JOIN history_samples AS samples "
        "  ON samples.device_id = targets.device_id "
        " AND samples.point_key = targets.point_key "
        " AND samples.sample_period = ? "
        " AND samples.bucket_start_ms >= targets.bucket_start_ms "
        " AND samples.bucket_start_ms < targets.bucket_end_ms "
        " WHERE samples.quality = 'good' AND samples.valid = 1 "
        " AND samples.value IS NOT NULL AND samples.value = samples.value "
        " AND samples.value BETWEEN -1.7976931348623157e308 AND 1.7976931348623157e308 "
        " GROUP BY targets.device_id, targets.point_key, targets.bucket_start_ms"
        ") "
        "INSERT OR REPLACE INTO history_samples "
        "(sample_period, bucket_start_ms, bucket_text, timestamp_ms, date_text, "
        "channel_id, master_id, device_id, template_id, point_key, point_name, unit, "
        "precision, value, raw_value, quality, valid, message, created_at_ms) "
        "SELECT ?, bucket_start_ms, bucket_text, timestamp_ms, date_text, channel_id, "
        "master_id, device_id, template_id, point_key, point_name, unit, precision, "
        "aggregate_value, aggregate_value, 'good', 1, '样本数：' || sample_count, ? "
        "FROM aggregate_rows;";
    Statement aggregate_statement(database_, aggregate_sql);
    if (!aggregate_statement.ok() ||
        !bind_text(aggregate_statement.get(), 1, input_period) ||
        !bind_text(aggregate_statement.get(), 2, output_period) ||
        !bind_int64(aggregate_statement.get(), 3, now_ms) ||
        sqlite3_step(aggregate_statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = join_error("批量写入历史聚合点失败", sqlite_error(database_));
        }
        return StatusCode::kIoError;
    }
    sqlite3_reset(aggregate_statement.get());
    status = execute_sql_locked(
        "DELETE FROM temp_history_aggregate_targets;", &workspace_error);
    if (!is_ok(status)) {
        if (error_message != nullptr) {
            *error_message = join_error("释放历史聚合目标失败", workspace_error);
        }
        return status;
    }
    return StatusCode::kOk;
}

StatusCode HistoryStore::aggregate_rebuild_required_locked(
    bool* required,
    std::string* error_message) const
{
    if (required == nullptr) {
        if (error_message != nullptr) *error_message = "历史聚合状态输出参数为空";
        return StatusCode::kInvalidArgument;
    }
    *required = true;
    Statement statement(
        database_,
        "SELECT aggregate_version, dirty FROM history_aggregate_state WHERE singleton_id = 1;");
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = join_error("准备读取历史聚合状态失败", sqlite_error(database_));
        }
        return StatusCode::kIoError;
    }
    const auto step_status = sqlite3_step(statement.get());
    if (step_status == SQLITE_DONE) {
        return StatusCode::kOk;
    }
    if (step_status != SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = join_error("读取历史聚合状态失败", sqlite_error(database_));
        }
        return StatusCode::kIoError;
    }
    *required = sqlite3_column_int(statement.get(), 0) != kCurrentAggregateStateVersion ||
                sqlite3_column_int(statement.get(), 1) != 0;
    return StatusCode::kOk;
}

StatusCode HistoryStore::set_aggregate_state_locked(
    bool dirty,
    TimestampMs now_ms,
    std::string* error_message)
{
    Statement statement(
        database_,
        "INSERT INTO history_aggregate_state(singleton_id, aggregate_version, dirty, updated_at_ms) "
        "VALUES(1, ?, ?, ?) "
        "ON CONFLICT(singleton_id) DO UPDATE SET "
        "aggregate_version = excluded.aggregate_version, dirty = excluded.dirty, "
        "updated_at_ms = excluded.updated_at_ms;");
    if (!statement.ok() ||
        !bind_int(statement.get(), 1, kCurrentAggregateStateVersion) ||
        !bind_int(statement.get(), 2, dirty ? 1 : 0) ||
        !bind_int64(statement.get(), 3, now_ms) ||
        sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = join_error("写入历史聚合状态失败", sqlite_error(database_));
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

StatusCode HistoryStore::rebuild_aggregates_if_needed_locked(
    TimestampMs now_ms,
    bool* rebuilt,
    std::string* error_message)
{
    if (rebuilt != nullptr) *rebuilt = false;
    bool required = true;
    auto status = aggregate_rebuild_required_locked(&required, error_message);
    if (!is_ok(status) || !required) return status;

    // 先独立提交 dirty。这样补算事务失败或进程中断时，下一次启动仍有持久化证据可重试。
    status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (is_ok(status)) status = set_aggregate_state_locked(true, now_ms, error_message);
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) {
        (void)execute_sql_locked("ROLLBACK;", nullptr);
        return status;
    }

    status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (is_ok(status)) status = rebuild_recent_aggregates_locked(now_ms, error_message);
    if (is_ok(status)) status = set_aggregate_state_locked(false, now_ms, error_message);
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) {
        const auto original_error = error_message == nullptr ? std::string{} : *error_message;
        std::string rollback_error;
        const auto rollback_status = execute_sql_locked("ROLLBACK;", &rollback_error);
        if (!is_ok(rollback_status) && error_message != nullptr) {
            *error_message = join_error(original_error, "回滚启动补算失败：" + rollback_error);
        }
        return status;
    }
    if (rebuilt != nullptr) *rebuilt = true;
    return StatusCode::kOk;
}

// 在持锁状态下重建近期聚合记录。
StatusCode HistoryStore::rebuild_recent_aggregates_locked(TimestampMs now_ms, std::string* error_message)
{
    // 缓存生命周期严格限制在本次 rebuild；下次启动/重建仍会读取最新的内置或自定义模板。
    HistoryEnabledPointCache enabled_points_by_template;
    const char* select_sql =
        "SELECT device_id, master_id, channel_id, template_id, sample_period, "
        "bucket_start_ms, bucket_text, timestamp_ms, date_text, point_key, point_name, unit, "
        "precision, value, raw_value, quality, valid, message, id FROM history_samples "
        "WHERE sample_period = ? AND bucket_start_ms >= ? "
        "AND (bucket_start_ms > ? OR (bucket_start_ms = ? AND id > ?)) "
        "ORDER BY bucket_start_ms ASC, id ASC LIMIT ?;";

    const auto rebuild_period = [&](const std::string& period,
                                    TimestampMs cutoff,
                                    const std::string& error_prefix) -> StatusCode {
        TimestampMs last_bucket_start_ms = 0;
        sqlite3_int64 last_id = 0;
        while (true) {
            std::vector<HistoryRecord> records;
            records.reserve(kStartupAggregateBatchSize);
            TimestampMs batch_last_bucket_start_ms = last_bucket_start_ms;
            sqlite3_int64 batch_last_id = last_id;
            {
                // 结束每批 SELECT statement 后再执行聚合写入，避免同一连接同时保留大结果集和写语句。
                Statement query(database_, select_sql);
                if (!query.ok() ||
                    !bind_text(query.get(), 1, period) ||
                    !bind_int64(query.get(), 2, cutoff) ||
                    !bind_int64(query.get(), 3, last_bucket_start_ms) ||
                    !bind_int64(query.get(), 4, last_bucket_start_ms) ||
                    !bind_int64(query.get(), 5, last_id) ||
                    !bind_int64(query.get(), 6, kStartupAggregateBatchSize)) {
                    if (error_message != nullptr) *error_message = join_error(error_prefix, sqlite_error(database_));
                    return StatusCode::kIoError;
                }
                int step_status = SQLITE_OK;
                while ((step_status = sqlite3_step(query.get())) == SQLITE_ROW) {
                    batch_last_bucket_start_ms = static_cast<TimestampMs>(sqlite3_column_int64(query.get(), 5));
                    batch_last_id = sqlite3_column_int64(query.get(), 18);
                    auto record = read_history_record(query.get());
                    if (history_record_enabled_by_current_template(record, &enabled_points_by_template)) {
                        records.push_back(std::move(record));
                    }
                }
                if (step_status != SQLITE_DONE) {
                    if (error_message != nullptr) *error_message = join_error(error_prefix, sqlite_error(database_));
                    return StatusCode::kIoError;
                }
            }
            if (batch_last_bucket_start_ms == last_bucket_start_ms && batch_last_id == last_id) {
                return StatusCode::kOk;
            }
            const auto status = period == "raw_10min"
                ? refresh_aggregates_locked(records, now_ms, error_message)
                : refresh_aggregate_buckets_locked(records, "hour", "hour", "day", now_ms, error_message);
            if (!is_ok(status)) return status;
            last_bucket_start_ms = batch_last_bucket_start_ms;
            last_id = batch_last_id;
        }
    };

    auto status = rebuild_period(
        "raw_10min", cutoff_ms(now_ms, kRawHistoryRetentionMs), "读取启动历史补算样本失败");
    if (!is_ok(status)) return status;

    return rebuild_period(
        "hour", cutoff_ms(now_ms, kDayHistoryRetentionMs), "读取启动日聚合补算数据失败");
}

// 清理过期数据。
StatusCode HistoryStore::cleanup_expired(HistoryCleanupResult* result, std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) return StatusCode::kInvalidState;
    const auto status = cleanup_expired_locked(time_utils::system_now_ms(), error_message, result);
    last_cleanup_time_ = std::chrono::steady_clock::now();
    return status;
}

// 按采样周期统计历史记录数量。
StatusCode HistoryStore::count_by_period(
    const std::string& sample_period,
    std::uint64_t* count,
    std::string* error_message) const
{
    if (count == nullptr || !is_valid_sample_period(sample_period)) {
        if (error_message != nullptr) *error_message = "历史采样粒度或计数输出参数无效";
        return StatusCode::kInvalidArgument;
    }
    ReadLease lease;
    const auto lease_status = acquire_read_lease(&lease, error_message);
    if (!is_ok(lease_status)) return lease_status;
    auto* query_database = lease.database;
    Statement statement(query_database, "SELECT COUNT(*) FROM history_samples WHERE sample_period = ?;");
    if (!statement.ok() || !bind_text(statement.get(), 1, sample_period) || sqlite3_step(statement.get()) != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = join_error("统计历史数据数量失败", sqlite_error(query_database));
        return StatusCode::kIoError;
    }
    *count = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    return StatusCode::kOk;
}

// 立即刷新历史数据库 WAL；超期清理由 BackendService 统一调度并记录维护事件。
void HistoryStore::save_now()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error_message;
    if (!database_available_locked(&error_message)) {
        Logger::warn("跳过历史数据刷新：" + error_message);
        return;
    }
    const auto status = execute_sql_locked("PRAGMA wal_checkpoint(PASSIVE);PRAGMA optimize;", &error_message);
    if (!is_ok(status)) {
        Logger::error("历史采集数据刷新失败：" + error_message);
    }
}

// 清空全部历史趋势数据。
StatusCode HistoryStore::clear_all(std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    return execute_sql_locked("DELETE FROM history_samples;", error_message);
}

// 返回历史数据库文件路径。
std::string HistoryStore::database_path() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return database_path_;
}

StatusCode HistoryStore::open_database_locked(const std::string& database_path, std::string* error_message)
{
    const edge::fs::path path(database_path);
    const auto directory_status = ensure_parent_directory(path, error_message);
    if (!is_ok(directory_status)) {
        return directory_status;
    }

    sqlite3* opened_database = nullptr;
    const auto open_status = sqlite3_open_v2(
        path.string().c_str(),
        &opened_database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (open_status != SQLITE_OK) {
        const auto detail = sqlite_error(opened_database);
        if (opened_database != nullptr) {
            sqlite3_close(opened_database);
        }
        if (error_message != nullptr) {
            *error_message = "打开历史 SQLite 数据库失败：" + path.string() + "，原因=" + detail;
        }
        return StatusCode::kIoError;
    }

    database_ = opened_database;
    database_path_ = path.string();

    const auto schema_status = initialize_schema_locked(error_message);
    if (!is_ok(schema_status)) {
        close_database_locked();
        return schema_status;
    }

    std::string rebuild_error;
    bool rebuilt = false;
    const auto rebuild_status = rebuild_aggregates_if_needed_locked(
        time_utils::system_now_ms(), &rebuilt, &rebuild_error);
    if (!is_ok(rebuild_status)) {
        Logger::warn("历史 SQLite 启动补算失败，dirty 标记已保留供下次启动重试：" + rebuild_error);
    } else if (rebuilt) {
        Logger::info("历史 SQLite 聚合补算完成并清理 dirty 标记");
    }

    const auto readers_status = open_read_connections_locked(error_message);
    if (!is_ok(readers_status)) {
        close_database_locked();
        return readers_status;
    }
    last_cleanup_time_ = {};

    return StatusCode::kOk;
}

StatusCode HistoryStore::open_read_connections_locked(std::string* error_message)
{
    close_read_connections_locked();
    // SQLite 的普通 :memory: 数据库按连接隔离；这里保留原有单连接可见性语义。
    if (database_path_ == ":memory:") {
        return StatusCode::kOk;
    }

    for (auto& slot : read_connections_) {
        sqlite3* opened_database = nullptr;
        const auto open_status = sqlite3_open_v2(
            database_path_.c_str(),
            &opened_database,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (open_status != SQLITE_OK) {
            const auto detail = sqlite_error(opened_database);
            if (opened_database != nullptr) sqlite3_close(opened_database);
            close_read_connections_locked();
            if (error_message != nullptr) {
                *error_message = join_error("打开历史 SQLite 只读连接失败", detail);
            }
            return StatusCode::kIoError;
        }

        sqlite3_busy_timeout(opened_database, kHistoryReadBusyTimeoutMs);
        char* raw_error = nullptr;
        const auto configure_status = sqlite3_exec(
            opened_database,
            "PRAGMA query_only = ON;PRAGMA cache_size = -512;",
            nullptr,
            nullptr,
            &raw_error);
        if (configure_status != SQLITE_OK) {
            const std::string detail = raw_error == nullptr
                                           ? sqlite_error(opened_database)
                                           : raw_error;
            sqlite3_free(raw_error);
            sqlite3_close(opened_database);
            close_read_connections_locked();
            if (error_message != nullptr) {
                *error_message = join_error("配置历史 SQLite 只读连接失败", detail);
            }
            return StatusCode::kIoError;
        }

        auto connection = std::make_shared<ReadConnection>();
        connection->database = opened_database;
        slot = std::move(connection);
    }
    next_read_connection_ = 0;
    return StatusCode::kOk;
}

void HistoryStore::close_read_connections_locked()
{
    for (auto& connection : read_connections_) {
        connection.reset();
    }
    next_read_connection_ = 0;
}

StatusCode HistoryStore::acquire_read_lease(
    ReadLease* lease,
    std::string* error_message) const
{
    if (lease == nullptr) {
        if (error_message != nullptr) *error_message = "历史 SQLite 只读租约输出参数为空";
        return StatusCode::kInvalidArgument;
    }
    lease->database = nullptr;
    lease->lifecycle_lock = std::unique_lock<std::mutex>(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    for (std::size_t attempt = 0; attempt < read_connections_.size(); ++attempt) {
        const auto index = (next_read_connection_ + attempt) % read_connections_.size();
        auto connection = read_connections_[index];
        if (connection == nullptr || connection->database == nullptr) continue;
        lease->connection = std::move(connection);
        next_read_connection_ = (index + 1) % read_connections_.size();
        lease->lifecycle_lock.unlock();
        // 连接由 shared_ptr 保活，因此等待繁忙读连接时不占用写连接的生命周期锁。
        lease->connection_lock = std::unique_lock<std::mutex>(lease->connection->mutex);
        lease->database = lease->connection->database;
        return StatusCode::kOk;
    }

    // :memory: 数据库没有可共享的独立连接，主锁随 lease 保持到查询结束。
    lease->database = database_;
    return StatusCode::kOk;
}

// 在持锁状态下初始化数据库结构。
StatusCode HistoryStore::initialize_schema_locked(std::string* error_message)
{
    const char* aggregate_state_schema_sql =
        "CREATE TABLE IF NOT EXISTS history_aggregate_state ("
        "singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),"
        "aggregate_version INTEGER NOT NULL,"
        "dirty INTEGER NOT NULL CHECK(dirty IN (0, 1)),"
        "updated_at_ms INTEGER NOT NULL);";
    const auto configure_status = execute_sql_locked(
        "PRAGMA busy_timeout = 5000;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA wal_autocheckpoint = 100;"
        "PRAGMA cache_size = -512;"
        // 连接级临时表只承载本批受影响桶，不改变正式 schema 或 user_version。
        "CREATE TEMP TABLE IF NOT EXISTS temp_history_aggregate_targets ("
        "device_id TEXT NOT NULL,point_key TEXT NOT NULL,"
        "bucket_start_ms INTEGER NOT NULL,bucket_end_ms INTEGER NOT NULL,"
        "bucket_text TEXT NOT NULL,date_text TEXT NOT NULL,timestamp_ms INTEGER NOT NULL,"
        "channel_id TEXT NOT NULL,master_id TEXT NOT NULL,template_id TEXT NOT NULL,"
        "point_name TEXT NOT NULL,unit TEXT NOT NULL,precision INTEGER NOT NULL,"
        "PRIMARY KEY(device_id,point_key,bucket_start_ms)) WITHOUT ROWID;",
        error_message);
    if (!is_ok(configure_status)) return configure_status;

    Statement version_statement(database_, "PRAGMA user_version;");
    if (!version_statement.ok() || sqlite3_step(version_statement.get()) != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "读取 edge-history.db user_version 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto version = sqlite3_column_int(version_statement.get(), 0);
    if (version == kCurrentHistoryDatabaseVersion) {
        Statement table_statement(
            database_,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='history_samples';");
        if (!table_statement.ok() || sqlite3_step(table_statement.get()) != SQLITE_ROW) {
            if (error_message != nullptr) {
                *error_message = schema_migration_required_message(
                    "edge-history.db 标记为 version 1 但缺少 history_samples");
            }
            return StatusCode::kInvalidState;
        }
        // version 1 现场库采用可向后兼容的内部状态表；缺少状态行会被视为 dirty 并补算一次。
        return execute_sql_locked(aggregate_state_schema_sql, error_message);
    }
    if (version != 0) {
        if (error_message != nullptr) {
            *error_message = schema_migration_required_message(
                "edge-history.db 版本不受支持，仅支持 version 1");
        }
        return StatusCode::kInvalidState;
    }
    Statement tables_statement(
        database_,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1;");
    if (!tables_statement.ok()) {
        if (error_message != nullptr) *error_message = "检查 edge-history.db 表结构失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto table_step = sqlite3_step(tables_statement.get());
    if (table_step == SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = schema_migration_required_message(
                "edge-history.db 的 user_version 为 0 但已有表，判定为旧库或异常半初始化库");
        }
        return StatusCode::kInvalidState;
    }
    if (table_step != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "检查 edge-history.db 表结构失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }

    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) return status;
    const char* schema_sql =
        "CREATE TABLE IF NOT EXISTS history_samples ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sample_period TEXT NOT NULL,"
        "bucket_start_ms INTEGER NOT NULL,"
        "bucket_text TEXT NOT NULL,"
        "timestamp_ms INTEGER NOT NULL,"
        "date_text TEXT NOT NULL,"
        "channel_id TEXT NOT NULL,"
        "master_id TEXT NOT NULL,"
        "device_id TEXT NOT NULL,"
        "template_id TEXT,"
        "point_key TEXT NOT NULL,"
        "point_name TEXT,"
        "unit TEXT,"
        "precision INTEGER NOT NULL DEFAULT 0,"
        "value REAL,"
        "raw_value REAL,"
        "quality TEXT,"
        "valid INTEGER NOT NULL DEFAULT 1,"
        "message TEXT,"
        "created_at_ms INTEGER NOT NULL"
        ");"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_history_samples_unique_bucket "
        "ON history_samples(device_id, point_key, sample_period, bucket_start_ms);"
        "CREATE INDEX IF NOT EXISTS idx_history_samples_period_time "
        "ON history_samples(sample_period, bucket_start_ms);"
        "CREATE INDEX IF NOT EXISTS idx_history_samples_date "
        "ON history_samples(date_text);"
        "CREATE TABLE IF NOT EXISTS history_aggregate_state ("
        "singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),"
        "aggregate_version INTEGER NOT NULL,"
        "dirty INTEGER NOT NULL CHECK(dirty IN (0, 1)),"
        "updated_at_ms INTEGER NOT NULL);";

    status = execute_sql_locked(schema_sql, error_message);
    if (is_ok(status)) status = execute_sql_locked("PRAGMA user_version = 1;", error_message);
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    return status;
}

// 在持锁状态下清理过期。
StatusCode HistoryStore::cleanup_expired_locked(
    TimestampMs now_ms,
    std::string* error_message,
    HistoryCleanupResult* result)
{
    if (result != nullptr) *result = {};
    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) {
        return status;
    }

    const auto delete_period = [&](const char* period, TimestampMs cutoff, std::uint64_t* deleted) {
        Statement statement(database_, "DELETE FROM history_samples WHERE sample_period = ? AND bucket_start_ms < ?;");
        if (!statement.ok() || !bind_text(statement.get(), 1, period) || !bind_int64(statement.get(), 2, cutoff) ||
            sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) *error_message = join_error("执行过期历史数据清理失败", sqlite_error(database_));
            return StatusCode::kIoError;
        }
        if (deleted != nullptr) *deleted = static_cast<std::uint64_t>(sqlite3_changes(database_));
        return StatusCode::kOk;
    };
    status = delete_period("raw_10min", cutoff_ms(now_ms, kRawHistoryRetentionMs), result == nullptr ? nullptr : &result->deleted_raw_10min_count);
    if (is_ok(status)) status = delete_period("hour", cutoff_ms(now_ms, kHourHistoryRetentionMs), result == nullptr ? nullptr : &result->deleted_hour_count);
    if (is_ok(status)) status = delete_period("day", cutoff_ms(now_ms, kDayHistoryRetentionMs), result == nullptr ? nullptr : &result->deleted_day_count);
    if (is_ok(status)) {
        status = execute_sql_locked("COMMIT;", error_message);
    }
    if (!is_ok(status)) {
        const auto original_error = error_message == nullptr || error_message->empty()
                                        ? sqlite_error(database_)
                                        : *error_message;
        Logger::error("历史过期数据清理事务失败：" + original_error);
        recover_failed_transaction_locked(original_error);
        if (result != nullptr) *result = {};
        return status;
    }

    (void)execute_sql_locked("PRAGMA optimize;", nullptr);
    return StatusCode::kOk;
}

// 在持锁状态下执行SQL。
StatusCode HistoryStore::execute_sql_locked(const char* sql, std::string* error_message) const
{
    char* raw_error = nullptr;
    const auto status = sqlite3_exec(database_, sql, nullptr, nullptr, &raw_error);
    if (status != SQLITE_OK) {
        const std::string detail = raw_error == nullptr ? sqlite_error(database_) : raw_error;
        sqlite3_free(raw_error);
        if (error_message != nullptr) {
            *error_message = detail;
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 在持锁状态下检查数据库是否可用。
bool HistoryStore::database_available_locked(std::string* error_message) const
{
    if (!initialized_ || database_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = last_error_message_.empty()
                                 ? "历史 SQLite 数据库不可用"
                                 : "历史 SQLite 数据库不可用：" + last_error_message_;
        }
        return false;
    }
    return true;
}

// 历史批次写入是失效连接的恢复触发点；每次只保留一个连接，失败日志按固定窗口限频。
StatusCode HistoryStore::ensure_database_for_write_locked(std::string* error_message)
{
    if (database_available_locked(error_message)) {
        return StatusCode::kOk;
    }
    if (database_path_.empty()) {
        return StatusCode::kInvalidState;
    }

    const auto reopen_path = database_path_;
    close_database_locked();
    std::string reopen_error;
    const auto reopen_status = open_database_locked(reopen_path, &reopen_error);
    initialized_ = is_ok(reopen_status);
    if (initialized_) {
        last_error_message_.clear();
        last_reopen_failure_log_time_ = {};
        if (error_message != nullptr) {
            error_message->clear();
        }
        Logger::info("历史 SQLite 连接已在后续写入周期自动恢复，路径=" + reopen_path);
        return StatusCode::kOk;
    }

    last_error_message_ = reopen_error.empty() ? "历史 SQLite 数据库重新打开失败" : reopen_error;
    if (error_message != nullptr) {
        *error_message = last_error_message_;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_reopen_failure_log_time_ == std::chrono::steady_clock::time_point{} ||
        now - last_reopen_failure_log_time_ >= kHistoryReopenFailureLogInterval) {
        Logger::error("历史 SQLite 自动恢复仍失败，将在后续写入周期继续尝试：" + last_error_message_);
        last_reopen_failure_log_time_ = now;
    }
    return reopen_status;
}

// 在持锁状态下关闭数据库。
void HistoryStore::close_database_locked()
{
    close_read_connections_locked();
    if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
    initialized_ = false;
}

// 事务结束失败后优先恢复 autocommit；连接仍不可信时复用完整打开流程重建。
void HistoryStore::recover_failed_transaction_locked(const std::string& original_error)
{
    if (database_ == nullptr) {
        return;
    }

    bool transaction_active = sqlite3_get_autocommit(database_) == 0;
    if (transaction_active) {
        std::string rollback_error;
        const auto rollback_status = execute_sql_locked("ROLLBACK;", &rollback_error);
        transaction_active = sqlite3_get_autocommit(database_) == 0;
        if (is_ok(rollback_status) && !transaction_active) {
            Logger::warn("历史 SQLite 事务失败后的连接已回滚，后续操作将继续使用当前连接，原始错误=" + original_error);
            return;
        }
        Logger::error(
            "历史 SQLite 事务回滚未能恢复连接，原因=" +
            (rollback_error.empty() ? std::string("ROLLBACK 后仍未恢复 autocommit") : rollback_error));
    } else {
        // COMMIT 报错但 SQLite 已回到 autocommit，连接没有残留事务，可继续处理后续批次。
        Logger::warn("历史 SQLite 事务结束报错，但连接已处于 autocommit，后续批次将继续使用当前连接");
        return;
    }

    const auto reopen_path = database_path_;
    close_database_locked();
    std::string reopen_error;
    const auto reopen_status = open_database_locked(reopen_path, &reopen_error);
    initialized_ = is_ok(reopen_status);
    if (initialized_) {
        last_error_message_.clear();
        Logger::warn("历史 SQLite 连接已重建，后续批次可继续写入，路径=" + reopen_path);
        return;
    }
    last_error_message_ = reopen_error.empty() ? "历史 SQLite 连接重建失败" : reopen_error;
    Logger::error("历史 SQLite 连接重建失败：" + last_error_message_);
}

}  // namespace edge_controller
