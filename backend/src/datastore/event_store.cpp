// 事件持久化仓库：负责事件去重、发生次数累计、分页查询和清理事务。
#include "datastore/event_store.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

#include "common/sqlite_compat.h"
#include "common/time_utils.h"
#include "datastore/sqlite_helpers.h"

namespace edge_controller {
namespace {

constexpr TimestampMs kRetentionMs = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr TimestampMs kDuplicateEventCountIntervalMs = 15ULL * 60ULL * 1000ULL;
constexpr std::size_t kMaxEventRecords = 5000;
constexpr std::uint32_t kDefaultExportLimit = 500;
constexpr std::uint32_t kMaxExportLimit = 1000;
constexpr std::uint32_t kDefaultHistoryPageSize = 10;
constexpr std::uint32_t kMaxHistoryPageSize = 50;
constexpr int kCurrentEventsDatabaseVersion = 1;

using sqlite_helpers::Statement;
using sqlite_helpers::bind_int64;
using sqlite_helpers::bind_text;
using sqlite_helpers::schema_migration_required_message;
inline constexpr auto db_error = sqlite_helpers::sqlite_error;
inline constexpr auto text_column = sqlite_helpers::column_text;

// 写入错误信息并返回指定失败状态码。
StatusCode fail(std::string* output, const std::string& message, StatusCode code = StatusCode::kIoError)
{
    if (output != nullptr) *output = message;
    return code;
}

// 规范化事件指纹中的单个字段。
std::string fingerprint_part(const std::string& value)
{
    return std::to_string(value.size()) + ":" + value;
}

// 根据事件稳定字段生成去重指纹。
std::string event_fingerprint(const ServiceEvent& event)
{
    std::ostringstream stream;
    stream << fingerprint_part(event.level)
           << '|'
           << fingerprint_part(event.source)
           << '|'
           << fingerprint_part(event.target_id)
           << '|'
           << fingerprint_part(event.summary)
           << '|'
           << fingerprint_part(event.diagnosis.level)
           << '|'
           << fingerprint_part(event.diagnosis.target_id)
           << '|'
           << fingerprint_part(event.diagnosis.error_code);
    return stream.str();
}

// 计算事件去重窗口已经经过的时间。
bool count_window_elapsed(TimestampMs current_ms, TimestampMs last_counted_ms)
{
    return current_ms >= last_counted_ms &&
           current_ms - last_counted_ms >= kDuplicateEventCountIntervalMs;
}

// 将字符串转换为小写。
std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// 判断事件级别筛选值是否合法。
bool is_export_level_filter(const std::string& value)
{
    return value.empty() || value == "all" || value == "error" || value == "warning" || value == "warn" || value == "info";
}

// 将导出数量限制约束到安全范围。
std::uint32_t normalize_export_limit(std::uint32_t value)
{
    if (value == 0) {
        return kDefaultExportLimit;
    }
    return std::min(value, kMaxExportLimit);
}

// 根据导出时间范围计算截止时间。
TimestampMs export_time_range_cutoff(const std::string& value)
{
    const auto normalized = lowercase(value);
    if (normalized.empty() || normalized == "all") {
        return 0;
    }
    const auto now = time_utils::system_now_ms();
    TimestampMs range_ms = 0;
    if (normalized == "24h") {
        range_ms = 24ULL * 60ULL * 60ULL * 1000ULL;
    } else if (normalized == "3d") {
        range_ms = 3ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    } else if (normalized == "7d") {
        range_ms = 7ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    }
    return now > range_ms ? now - range_ms : 0;
}

// 判断事件导出时间范围是否合法。
bool is_valid_export_time_range(const std::string& value)
{
    const auto normalized = lowercase(value);
    return normalized.empty() || normalized == "all" || normalized == "24h" || normalized == "3d" || normalized == "7d";
}

// 转义 SQL LIKE 查询中的通配符。
std::string escape_like_pattern(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto ch : value) {
        if (ch == '\\' || ch == '%' || ch == '_') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return "%" + escaped + "%";
}

struct EventHistoryFilter {
    std::string level;
    std::string source;
    TimestampMs cutoff{0};
    std::string search_pattern;
    bool has_level{false};
};

// 构造历史页的 SQL WHERE 片段。页面搜索字段与旧 Go 内存筛选保持一致。
std::string event_history_filter_sql(const EventHistoryFilter& filter)
{
    std::string sql = " WHERE 1=1";
    if (filter.has_level) {
        if (filter.level == "warning" || filter.level == "warn") {
            sql += " AND lower(trim(level)) IN ('warning','warn')";
        } else if (filter.level == "info") {
            sql += " AND lower(trim(level)) NOT IN ('error','warning','warn')";
        } else {
            sql += " AND lower(trim(level)) = ?";
        }
    }
    if (!filter.source.empty()) {
        sql += " AND lower(trim(source)) = ?";
    }
    if (filter.cutoff > 0) {
        sql += " AND timestamp_ms >= ?";
    }
    if (!filter.search_pattern.empty()) {
        sql +=
            " AND (summary LIKE ? ESCAPE '\\' OR detail LIKE ? ESCAPE '\\' "
            "OR target_id LIKE ? ESCAPE '\\' OR diagnosis_target_id LIKE ? ESCAPE '\\' "
            "OR diagnosis_target_name LIKE ? ESCAPE '\\' OR diagnosis_error_code LIKE ? ESCAPE '\\' "
            "OR diagnosis_message LIKE ? ESCAPE '\\' OR diagnosis_suggestion LIKE ? ESCAPE '\\')";
    }
    return sql;
}

// 按 event_history_filter_sql 的占位顺序绑定参数。
bool bind_event_history_filter(sqlite3_stmt* statement, const EventHistoryFilter& filter)
{
    int index = 1;
    if (filter.has_level && filter.level != "warning" && filter.level != "warn" && filter.level != "info" &&
        !bind_text(statement, index++, filter.level)) {
        return false;
    }
    if (!filter.source.empty() && !bind_text(statement, index++, filter.source)) {
        return false;
    }
    if (filter.cutoff > 0 && !bind_int64(statement, index++, filter.cutoff)) {
        return false;
    }
    if (!filter.search_pattern.empty()) {
        for (int field = 0; field < 8; ++field) {
            if (!bind_text(statement, index++, filter.search_pattern)) {
                return false;
            }
        }
    }
    return true;
}

// 绑定事件。
bool bind_event(sqlite3_stmt* stmt, const ServiceEvent& event)
{
    const auto& diagnosis = event.diagnosis;
    return bind_text(stmt, 1, event.event_id) &&
           sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(event.first_timestamp_ms)) == SQLITE_OK &&
           sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(event.timestamp_ms)) == SQLITE_OK &&
           bind_text(stmt, 4, event.level) &&
           bind_text(stmt, 5, event.source) &&
           bind_text(stmt, 6, event.target_id) &&
           bind_text(stmt, 7, diagnosis.level) &&
           bind_text(stmt, 8, diagnosis.target_id) &&
           bind_text(stmt, 9, diagnosis.target_name) &&
           bind_text(stmt, 10, diagnosis.status) &&
           bind_text(stmt, 11, diagnosis.error_code) &&
           bind_text(stmt, 12, diagnosis.message) &&
           bind_text(stmt, 13, diagnosis.suggestion) &&
           sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(diagnosis.last_success_time_ms)) == SQLITE_OK &&
           sqlite3_bind_int64(stmt, 15, static_cast<sqlite3_int64>(diagnosis.last_error_time_ms)) == SQLITE_OK &&
           sqlite3_bind_int64(stmt, 16, diagnosis.consecutive_failures) == SQLITE_OK &&
           bind_text(stmt, 17, event.summary) &&
           bind_text(stmt, 18, event.detail) &&
           sqlite3_bind_int64(stmt, 19, static_cast<sqlite3_int64>(event.occurrence_count)) == SQLITE_OK;
}

// 读取事件。
ServiceEvent read_event(sqlite3_stmt* stmt)
{
    ServiceEvent event;
    event.event_id = text_column(stmt, 0);
    event.first_timestamp_ms = static_cast<TimestampMs>(sqlite3_column_int64(stmt, 1));
    event.timestamp_ms = static_cast<TimestampMs>(sqlite3_column_int64(stmt, 2));
    event.level = text_column(stmt, 3);
    event.source = text_column(stmt, 4);
    event.target_id = text_column(stmt, 5);
    event.diagnosis.level = text_column(stmt, 6);
    event.diagnosis.target_id = text_column(stmt, 7);
    event.diagnosis.target_name = text_column(stmt, 8);
    event.diagnosis.status = text_column(stmt, 9);
    event.diagnosis.error_code = text_column(stmt, 10);
    event.diagnosis.message = text_column(stmt, 11);
    event.diagnosis.suggestion = text_column(stmt, 12);
    event.diagnosis.last_success_time_ms = static_cast<TimestampMs>(sqlite3_column_int64(stmt, 13));
    event.diagnosis.last_error_time_ms = static_cast<TimestampMs>(sqlite3_column_int64(stmt, 14));
    event.diagnosis.consecutive_failures = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 15));
    event.summary = text_column(stmt, 16);
    event.detail = text_column(stmt, 17);
    event.occurrence_count = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 18));
    return event;
}

}  // namespace

// 关闭数据库连接并释放存储资源；内部加锁保证析构安全。
EventStore::~EventStore()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ignored;
    flush_dirty_locked(&ignored);
    close_locked();
}

// 打开事件数据库并初始化表结构。
StatusCode EventStore::initialize(const std::string& path, std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (database_ != nullptr && database_path_ == path) return initialize_schema_locked(error);

    std::string ignored;
    flush_dirty_locked(&ignored);
    close_locked();
    if (path.empty()) return fail(error, "事件 SQLite 数据库路径不能为空", StatusCode::kInvalidArgument);
    if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto message = db_error(database_);
        close_locked();
        return fail(error, "打开事件 SQLite 数据库失败: " + message);
    }
    database_path_ = path;
    sqlite3_busy_timeout(database_, 5000);
    dedup_states_.clear();
    dirty_state_count_ = 0;

    auto status = initialize_schema_locked(error);
    if (!is_ok(status)) {
        close_locked();
        return status;
    }
    Statement count(database_, "SELECT COUNT(*) FROM service_events;");
    if (!count.ok() || sqlite3_step(count.get()) != SQLITE_ROW) {
        return fail(error, "统计启动历史事件数量失败: " + db_error(database_));
    }
    record_count_ = static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
    last_cleanup_time_ = {};
    return StatusCode::kOk;
}

// 在持锁状态下初始化数据库结构。
StatusCode EventStore::initialize_schema_locked(std::string* error)
{
    const auto configure = execute_locked(
        "PRAGMA busy_timeout=5000;PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;"
        "PRAGMA wal_autocheckpoint=100;", error);
    if (!is_ok(configure)) return configure;

    Statement version_statement(database_, "PRAGMA user_version;");
    if (!version_statement.ok() || sqlite3_step(version_statement.get()) != SQLITE_ROW) {
        return fail(error, "读取 edge-events.db user_version 失败: " + db_error(database_));
    }
    const auto version = sqlite3_column_int(version_statement.get(), 0);
    if (version == kCurrentEventsDatabaseVersion) {
        Statement table_statement(
            database_,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='service_events';");
        if (!table_statement.ok() || sqlite3_step(table_statement.get()) != SQLITE_ROW) {
            return fail(
                error,
                schema_migration_required_message(
                    "edge-events.db 标记为 version 1 但缺少 service_events"),
                StatusCode::kInvalidState);
        }
        return StatusCode::kOk;
    }
    if (version != 0) {
        return fail(
            error,
            schema_migration_required_message(
                "edge-events.db 版本不受支持，仅支持 version 1"),
            StatusCode::kInvalidState);
    }

    Statement tables_statement(
        database_,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1;");
    if (!tables_statement.ok()) {
        return fail(error, "检查 edge-events.db 表结构失败: " + db_error(database_));
    }
    const auto table_step = sqlite3_step(tables_statement.get());
    if (table_step == SQLITE_ROW) {
        return fail(
            error,
            schema_migration_required_message(
                "edge-events.db 的 user_version 为 0 但已有表，判定为旧库或异常半初始化库"),
            StatusCode::kInvalidState);
    }
    if (table_step != SQLITE_DONE) {
        return fail(error, "检查 edge-events.db 表结构失败: " + db_error(database_));
    }

    auto status = execute_locked("BEGIN IMMEDIATE TRANSACTION;", error);
    if (!is_ok(status)) return status;
    status = execute_locked(
        "CREATE TABLE IF NOT EXISTS service_events(event_id TEXT PRIMARY KEY,first_timestamp_ms INTEGER NOT NULL,timestamp_ms INTEGER NOT NULL,level TEXT NOT NULL,source TEXT NOT NULL,target_id TEXT NOT NULL,diagnosis_level TEXT NOT NULL,diagnosis_target_id TEXT NOT NULL,diagnosis_target_name TEXT NOT NULL,diagnosis_status TEXT NOT NULL,diagnosis_error_code TEXT NOT NULL,diagnosis_message TEXT NOT NULL,diagnosis_suggestion TEXT NOT NULL,diagnosis_last_success_time_ms INTEGER NOT NULL,diagnosis_last_error_time_ms INTEGER NOT NULL,diagnosis_consecutive_failures INTEGER NOT NULL,summary TEXT NOT NULL,detail TEXT NOT NULL,occurrence_count INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS idx_service_events_timestamp ON service_events(timestamp_ms DESC);"
        "CREATE INDEX IF NOT EXISTS idx_service_events_level ON service_events(level);"
        "CREATE INDEX IF NOT EXISTS idx_service_events_source ON service_events(source);"
        "CREATE INDEX IF NOT EXISTS idx_service_events_target ON service_events(target_id);",
        error);
    if (is_ok(status)) status = execute_locked("PRAGMA user_version=1;", error);
    if (is_ok(status)) status = execute_locked("COMMIT;", error);
    if (!is_ok(status)) execute_locked("ROLLBACK;", nullptr);
    return status;
}

// 写入事件并返回合并后的持久化记录。
StatusCode EventStore::append(ServiceEvent event, std::string* error, ServiceEvent* stored_event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stored_event != nullptr) {
        *stored_event = {};
    }
    if (!available_locked(error)) return StatusCode::kInvalidState;

    event.timestamp_ms = event.timestamp_ms == 0 ? time_utils::system_now_ms() : event.timestamp_ms;
    event.first_timestamp_ms = event.first_timestamp_ms == 0 ? event.timestamp_ms : event.first_timestamp_ms;
    event.occurrence_count = 1;
    event.level = event.level.empty() ? "info" : event.level;
    event.source = event.source.empty() ? "system" : event.source;

    const auto fingerprint = event_fingerprint(event);
    auto existing = dedup_states_.find(fingerprint);
    if (existing == dedup_states_.end()) {
        auto status = insert_locked(&event, error);
        if (!is_ok(status)) return status;
        ++record_count_;
        DedupState state;
        state.event = event;
        state.last_counted_at_ms = event.timestamp_ms;
        dedup_states_[fingerprint] = std::move(state);
        if (stored_event != nullptr) {
            *stored_event = event;
        }
        if (record_count_ > kMaxEventRecords) {
            std::uint64_t deleted_count = 0;
            status = flush_dirty_locked(error);
            if (is_ok(status)) {
                status = cleanup_locked(
                    time_utils::system_now_ms(),
                    error,
                    &deleted_count);
            }
        }
        return status;
    }

    auto& state = existing->second;
    if (state.dirty) {
        // 上一次去重事件更新失败时，内存状态已经前进但数据库尚未落盘。
        // 下一次 append 必须先完成该事件，避免 count window 把失败重试误判为普通重复。
        const auto status = update_event_locked(state.event, error);
        if (!is_ok(status)) {
            return status;
        }
        state.dirty = false;
        if (dirty_state_count_ > 0) {
            --dirty_state_count_;
        }
        if (stored_event != nullptr) {
            *stored_event = state.event;
        }
        return StatusCode::kOk;
    }
    if (!count_window_elapsed(event.timestamp_ms, state.last_counted_at_ms)) {
        state.event.detail = event.detail;
        state.event.diagnosis = event.diagnosis;
        return StatusCode::kOk;
    }

    state.event.timestamp_ms = event.timestamp_ms;
    state.event.detail = event.detail;
    state.event.diagnosis = event.diagnosis;
    ++state.event.occurrence_count;
    state.last_counted_at_ms = event.timestamp_ms;
    if (!state.dirty) {
        state.dirty = true;
        ++dirty_state_count_;
    }
    const auto status = update_event_locked(state.event, error);
    if (is_ok(status)) {
        state.dirty = false;
        if (dirty_state_count_ > 0) {
            --dirty_state_count_;
        }
    }
    if (is_ok(status) && stored_event != nullptr) {
        *stored_event = state.event;
    }
    return status;
}

// 在持锁状态下写入。
StatusCode EventStore::insert_locked(ServiceEvent* event, std::string* error)
{
    if (event->event_id.empty()) {
        Statement id(database_, "SELECT 'evt-' || lower(hex(randomblob(16))); ");
        if (!id.ok() || sqlite3_step(id.get()) != SQLITE_ROW) {
            return fail(error, "生成事件ID失败: " + db_error(database_));
        }
        event->event_id = text_column(id.get(), 0);
    }

    Statement stmt(database_, "INSERT INTO service_events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!stmt.ok() || !bind_event(stmt.get(), *event) || sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return fail(error, "写入历史事件失败: " + db_error(database_));
    }
    return StatusCode::kOk;
}

// 在持锁状态下更新事件。
StatusCode EventStore::update_event_locked(const ServiceEvent& event, std::string* error) const
{
    if (!available_locked(error)) return StatusCode::kInvalidState;
    const auto& diagnosis = event.diagnosis;
    Statement stmt(
        database_,
        "UPDATE service_events SET timestamp_ms=?,diagnosis_level=?,diagnosis_target_id=?,diagnosis_target_name=?,diagnosis_status=?,diagnosis_error_code=?,diagnosis_message=?,diagnosis_suggestion=?,diagnosis_last_success_time_ms=?,diagnosis_last_error_time_ms=?,diagnosis_consecutive_failures=?,detail=?,occurrence_count=? WHERE event_id=?;");
    const bool ok = stmt.ok() &&
                    sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(event.timestamp_ms)) == SQLITE_OK &&
                    bind_text(stmt.get(), 2, diagnosis.level) &&
                    bind_text(stmt.get(), 3, diagnosis.target_id) &&
                    bind_text(stmt.get(), 4, diagnosis.target_name) &&
                    bind_text(stmt.get(), 5, diagnosis.status) &&
                    bind_text(stmt.get(), 6, diagnosis.error_code) &&
                    bind_text(stmt.get(), 7, diagnosis.message) &&
                    bind_text(stmt.get(), 8, diagnosis.suggestion) &&
                    sqlite3_bind_int64(stmt.get(), 9, static_cast<sqlite3_int64>(diagnosis.last_success_time_ms)) == SQLITE_OK &&
                    sqlite3_bind_int64(stmt.get(), 10, static_cast<sqlite3_int64>(diagnosis.last_error_time_ms)) == SQLITE_OK &&
                    sqlite3_bind_int64(stmt.get(), 11, diagnosis.consecutive_failures) == SQLITE_OK &&
                    bind_text(stmt.get(), 12, event.detail) &&
                    sqlite3_bind_int64(stmt.get(), 13, static_cast<sqlite3_int64>(event.occurrence_count)) == SQLITE_OK &&
                    bind_text(stmt.get(), 14, event.event_id);
    if (!ok || sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return fail(error, "刷新重复历史事件失败: " + db_error(database_));
    }
    return StatusCode::kOk;
}

// 在持锁状态下刷新尚未持久化的事件。
StatusCode EventStore::flush_dirty_locked(std::string* error) const
{
    if (dirty_state_count_ == 0) {
        return StatusCode::kOk;
    }
    for (auto& item : dedup_states_) {
        if (!item.second.dirty) continue;
        const auto status = update_event_locked(item.second.event, error);
        if (!is_ok(status)) return status;
        item.second.dirty = false;
        if (dirty_state_count_ > 0) {
            --dirty_state_count_;
        }
    }
    return StatusCode::kOk;
}

// 列出最近。
StatusCode EventStore::list_recent(std::size_t limit, std::vector<ServiceEvent>* output, std::string* error) const
{
    if (output == nullptr) return fail(error, "历史事件输出参数为空", StatusCode::kInvalidArgument);
    output->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = flush_dirty_locked(error);
    if (!is_ok(status)) return status;
    if (limit == 0) return StatusCode::kOk;

    Statement stmt(
        database_,
        "SELECT event_id,first_timestamp_ms,timestamp_ms,level,source,target_id,diagnosis_level,diagnosis_target_id,diagnosis_target_name,diagnosis_status,diagnosis_error_code,diagnosis_message,diagnosis_suggestion,diagnosis_last_success_time_ms,diagnosis_last_error_time_ms,diagnosis_consecutive_failures,summary,detail,occurrence_count FROM service_events ORDER BY timestamp_ms DESC,event_id DESC LIMIT ?;");
    if (!stmt.ok() || sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
        return fail(error, "准备历史事件查询失败: " + db_error(database_));
    }
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        output->push_back(read_event(stmt.get()));
    }
    return rc == SQLITE_DONE ? StatusCode::kOk : fail(error, "查询历史事件失败: " + db_error(database_));
}

// 在同一个 EventStore 临界区中返回统计、匹配总数和当前页，避免页面看到跨写入时点的结果。
StatusCode EventStore::query_history(
    const EventHistoryQuery& query,
    EventHistoryResult* output,
    std::string* error) const
{
    if (output == nullptr) {
        return fail(error, "历史事件分页输出参数为空", StatusCode::kInvalidArgument);
    }
    *output = {};

    EventHistoryFilter filter;
    filter.level = lowercase(query.level);
    filter.source = lowercase(query.source);
    if (filter.source == "all") {
        filter.source.clear();
    }
    if (!is_export_level_filter(filter.level)) {
        return fail(error, "历史事件级别筛选非法", StatusCode::kInvalidArgument);
    }
    if (!is_valid_export_time_range(query.time_range)) {
        return fail(error, "历史事件时间范围非法", StatusCode::kInvalidArgument);
    }
    filter.has_level = !filter.level.empty() && filter.level != "all";
    filter.cutoff = export_time_range_cutoff(query.time_range);
    if (!query.search.empty()) {
        filter.search_pattern = escape_like_pattern(query.search);
    }

    const auto page_size = query.page_size == 0
                               ? kDefaultHistoryPageSize
                               : std::min(query.page_size, kMaxHistoryPageSize);
    const auto requested_page = query.page == 0 ? 1U : query.page;

    std::lock_guard<std::mutex> lock(mutex_);
    auto status = flush_dirty_locked(error);
    if (!is_ok(status)) return status;
    if (!available_locked(error)) return StatusCode::kInvalidState;

    // 级别统计不受当前筛选影响，与旧页面顶部卡片语义一致；未知级别按 info 展示。
    Statement level_stats(
        database_,
        "SELECT "
        "SUM(CASE WHEN lower(trim(level))='error' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN lower(trim(level)) IN ('warning','warn') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN lower(trim(level)) NOT IN ('error','warning','warn') THEN 1 ELSE 0 END) "
        "FROM service_events;");
    if (!level_stats.ok() || sqlite3_step(level_stats.get()) != SQLITE_ROW) {
        return fail(error, "统计历史事件级别失败: " + db_error(database_));
    }
    output->level_stats.error = static_cast<std::uint64_t>(sqlite3_column_int64(level_stats.get(), 0));
    output->level_stats.warning = static_cast<std::uint64_t>(sqlite3_column_int64(level_stats.get(), 1));
    output->level_stats.info = static_cast<std::uint64_t>(sqlite3_column_int64(level_stats.get(), 2));

    Statement source_stats(
        database_,
        "SELECT lower(trim(source)),COUNT(*) FROM service_events "
        "GROUP BY lower(trim(source)) ORDER BY lower(trim(source));");
    if (!source_stats.ok()) {
        return fail(error, "准备历史事件来源统计失败: " + db_error(database_));
    }
    int step_status = SQLITE_OK;
    while ((step_status = sqlite3_step(source_stats.get())) == SQLITE_ROW) {
        output->source_stats.push_back(EventSourceStat{
            text_column(source_stats.get(), 0),
            static_cast<std::uint64_t>(sqlite3_column_int64(source_stats.get(), 1)),
        });
    }
    if (step_status != SQLITE_DONE) {
        return fail(error, "统计历史事件来源失败: " + db_error(database_));
    }

    const auto where_sql = event_history_filter_sql(filter);
    const auto count_sql = "SELECT COUNT(*) FROM service_events" + where_sql + ";";
    Statement count(database_, count_sql.c_str());
    if (!count.ok() || !bind_event_history_filter(count.get(), filter) ||
        sqlite3_step(count.get()) != SQLITE_ROW) {
        return fail(error, "统计筛选后历史事件失败: " + db_error(database_));
    }
    output->total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    if (output->total == 0) {
        return StatusCode::kOk;
    }

    const auto total_pages = (output->total + page_size - 1U) / page_size;
    const auto page = std::min<std::uint64_t>(requested_page, total_pages);
    const auto offset = (page - 1U) * page_size;
    const std::string rows_sql =
        "SELECT event_id,first_timestamp_ms,timestamp_ms,level,source,target_id,"
        "diagnosis_level,diagnosis_target_id,diagnosis_target_name,diagnosis_status,"
        "diagnosis_error_code,diagnosis_message,diagnosis_suggestion,"
        "diagnosis_last_success_time_ms,diagnosis_last_error_time_ms,"
        "diagnosis_consecutive_failures,summary,detail,occurrence_count "
        "FROM service_events" + where_sql +
        " ORDER BY timestamp_ms DESC,event_id DESC LIMIT " + std::to_string(page_size) +
        " OFFSET " + std::to_string(offset) + ";";
    Statement rows(database_, rows_sql.c_str());
    if (!rows.ok() || !bind_event_history_filter(rows.get(), filter)) {
        return fail(error, "准备历史事件分页查询失败: " + db_error(database_));
    }
    while ((step_status = sqlite3_step(rows.get())) == SQLITE_ROW) {
        output->rows.push_back(read_event(rows.get()));
    }
    return step_status == SQLITE_DONE
               ? StatusCode::kOk
               : fail(error, "查询历史事件分页记录失败: " + db_error(database_));
}

// 导出事件。
StatusCode EventStore::export_events(
    const EventExportQuery& query,
    std::vector<ServiceEvent>* output,
    std::string* error) const
{
    if (output == nullptr) return fail(error, "历史事件导出输出参数为空", StatusCode::kInvalidArgument);
    output->clear();

    const auto level = lowercase(query.level);
    const auto source = query.source == "all" ? std::string{} : query.source;
    if (!is_export_level_filter(level)) {
        return fail(error, "历史事件级别筛选非法", StatusCode::kInvalidArgument);
    }
    if (!is_valid_export_time_range(query.time_range)) {
        return fail(error, "历史事件时间范围非法", StatusCode::kInvalidArgument);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto status = flush_dirty_locked(error);
    if (!is_ok(status)) return status;
    if (!available_locked(error)) return StatusCode::kInvalidState;

    const auto cutoff = export_time_range_cutoff(query.time_range);
    std::string sql =
        "SELECT event_id,first_timestamp_ms,timestamp_ms,level,source,target_id,"
        "diagnosis_level,diagnosis_target_id,diagnosis_target_name,diagnosis_status,"
        "diagnosis_error_code,diagnosis_message,diagnosis_suggestion,"
        "diagnosis_last_success_time_ms,diagnosis_last_error_time_ms,"
        "diagnosis_consecutive_failures,summary,detail,occurrence_count "
        "FROM service_events WHERE 1=1";
    const bool has_level = !level.empty() && level != "all";
    if (has_level) {
        sql += (level == "warning" || level == "warn") ? " AND level IN ('warning','warn')" : " AND level = ?";
    }
    if (!source.empty()) {
        sql += " AND source = ?";
    }
    if (cutoff > 0) {
        sql += " AND timestamp_ms >= ?";
    }
    if (!query.search.empty()) {
        sql +=
            " AND (summary LIKE ? ESCAPE '\\' OR detail LIKE ? ESCAPE '\\' "
            "OR target_id LIKE ? ESCAPE '\\' OR diagnosis_message LIKE ? ESCAPE '\\' "
            "OR diagnosis_target_id LIKE ? ESCAPE '\\' OR diagnosis_target_name LIKE ? ESCAPE '\\' "
            "OR diagnosis_status LIKE ? ESCAPE '\\' OR diagnosis_error_code LIKE ? ESCAPE '\\')";
    }
    sql += " ORDER BY timestamp_ms DESC,event_id DESC LIMIT ? OFFSET ?;";

    Statement stmt(database_, sql.c_str());
    if (!stmt.ok()) {
        return fail(error, "准备历史事件导出查询失败: " + db_error(database_));
    }

    int bind_index = 1;
    bool bind_ok = true;
    if (has_level && level != "warning" && level != "warn") {
        bind_ok = bind_ok && bind_text(stmt.get(), bind_index++, level);
    }
    if (!source.empty()) {
        bind_ok = bind_ok && bind_text(stmt.get(), bind_index++, source);
    }
    if (cutoff > 0) {
        bind_ok = bind_ok && bind_int64(stmt.get(), bind_index++, cutoff);
    }
    if (!query.search.empty()) {
        const auto pattern = escape_like_pattern(query.search);
        bind_ok = bind_ok &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern) &&
                  bind_text(stmt.get(), bind_index++, pattern);
    }
    bind_ok = bind_ok &&
              bind_int64(stmt.get(), bind_index++, normalize_export_limit(query.limit)) &&
              bind_int64(stmt.get(), bind_index++, query.offset);
    if (!bind_ok) {
        return fail(error, "绑定历史事件导出查询参数失败: " + db_error(database_));
    }

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        output->push_back(read_event(stmt.get()));
    }
    return rc == SQLITE_DONE ? StatusCode::kOk : fail(error, "查询历史事件导出记录失败: " + db_error(database_));
}

// 清空全部。
StatusCode EventStore::clear_all(std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    dedup_states_.clear();
    dirty_state_count_ = 0;
    const auto status = execute_locked("DELETE FROM service_events;", error);
    if (is_ok(status)) record_count_ = 0;
    return status;
}

// 保存立即。
StatusCode EventStore::save_now(std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_dirty_locked(error);
}

// 清理过期数据。
StatusCode EventStore::cleanup_expired(std::uint64_t* deleted_count, std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto status = flush_dirty_locked(error);
    if (!is_ok(status)) return status;
    const auto cleanup_status = cleanup_locked(time_utils::system_now_ms(), error, deleted_count);
    last_cleanup_time_ = std::chrono::steady_clock::now();
    return cleanup_status;
}

// 刷新待写计数并重置基于墙上时间的进程内去重窗口。
StatusCode EventStore::reset_deduplication(std::string* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto status = flush_dirty_locked(error);
    if (!is_ok(status)) return status;
    dedup_states_.clear();
    dirty_state_count_ = 0;
    return StatusCode::kOk;
}

// 统计数量。
StatusCode EventStore::record_count(std::uint64_t* count, std::string* error) const
{
    if (count == nullptr) return fail(error, "历史事件计数输出参数为空", StatusCode::kInvalidArgument);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available_locked(error)) return StatusCode::kInvalidState;
    *count = static_cast<std::uint64_t>(record_count_);
    return StatusCode::kOk;
}

// 在持锁状态下清理。
StatusCode EventStore::cleanup_locked(TimestampMs now, std::string* error, std::uint64_t* deleted_count)
{
    const TimestampMs cutoff = now > kRetentionMs ? now - kRetentionMs : 0;
    auto status = execute_locked("BEGIN IMMEDIATE;", error);
    if (!is_ok(status)) return status;

    std::size_t deleted = 0;
    {
        Statement expired(database_, "DELETE FROM service_events WHERE timestamp_ms < ?;");
        if (!expired.ok() ||
            sqlite3_bind_int64(expired.get(), 1, static_cast<sqlite3_int64>(cutoff)) != SQLITE_OK ||
            sqlite3_step(expired.get()) != SQLITE_DONE) {
            execute_locked("ROLLBACK;", nullptr);
            return fail(error, "清理过期历史事件失败: " + db_error(database_));
        }
        deleted += static_cast<std::size_t>(sqlite3_changes(database_));
    }

    {
        Statement overflow(
            database_,
            "DELETE FROM service_events WHERE event_id IN ("
            "SELECT event_id FROM service_events "
            "ORDER BY timestamp_ms DESC,event_id DESC LIMIT -1 OFFSET ?);");
        if (!overflow.ok() ||
            sqlite3_bind_int64(
                overflow.get(),
                1,
                static_cast<sqlite3_int64>(kMaxEventRecords)) != SQLITE_OK ||
            sqlite3_step(overflow.get()) != SQLITE_DONE) {
            execute_locked("ROLLBACK;", nullptr);
            return fail(error, "裁剪历史事件数量失败: " + db_error(database_));
        }
        deleted += static_cast<std::size_t>(sqlite3_changes(database_));
    }

    status = execute_locked("COMMIT;", error);
    if (!is_ok(status)) {
        execute_locked("ROLLBACK;", nullptr);
        return status;
    }
    record_count_ = deleted >= record_count_ ? 0 : record_count_ - deleted;
    if (deleted_count != nullptr) {
        *deleted_count = static_cast<std::uint64_t>(deleted);
    }
    if (deleted > 0 && !dedup_states_.empty()) {
        // 只淘汰已从数据库删除的去重项；保留仍在库中的窗口，避免达到容量上限后
        // 每次重复事件都退化为“插入一条再删除一条”。
        Statement retained(database_, "SELECT event_id FROM service_events;");
        if (!retained.ok()) {
            dedup_states_.clear();
            dirty_state_count_ = 0;
            return StatusCode::kOk;
        }
        std::unordered_set<std::string> retained_event_ids;
        retained_event_ids.reserve(record_count_);
        int step_status = SQLITE_OK;
        while ((step_status = sqlite3_step(retained.get())) == SQLITE_ROW) {
            retained_event_ids.insert(text_column(retained.get(), 0));
        }
        if (step_status != SQLITE_DONE) {
            dedup_states_.clear();
            dirty_state_count_ = 0;
            return StatusCode::kOk;
        }
        for (auto iterator = dedup_states_.begin(); iterator != dedup_states_.end();) {
            if (retained_event_ids.find(iterator->second.event.event_id) ==
                retained_event_ids.end()) {
                iterator = dedup_states_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        dirty_state_count_ = 0;
    }
    return StatusCode::kOk;
}

// 在持锁状态下执行。
StatusCode EventStore::execute_locked(const char* sql, std::string* error) const
{
    if (!available_locked(error)) return StatusCode::kInvalidState;
    char* message = nullptr;
    const int rc = sqlite3_exec(database_, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return StatusCode::kOk;
    const std::string value = message == nullptr ? db_error(database_) : message;
    sqlite3_free(message);
    return fail(error, value);
}

// 在持锁状态下检查数据库是否可用。
bool EventStore::available_locked(std::string* error) const
{
    if (database_ != nullptr) return true;
    fail(error, "事件 SQLite 数据库尚未初始化", StatusCode::kInvalidState);
    return false;
}

// 在持锁状态下关闭。
void EventStore::close_locked()
{
    if (database_ != nullptr) sqlite3_close(database_);
    database_ = nullptr;
    database_path_.clear();
    dedup_states_.clear();
    dirty_state_count_ = 0;
    record_count_ = 0;
}

}  // namespace edge_controller
