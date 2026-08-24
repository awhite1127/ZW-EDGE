#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/filesystem_compat.h"
#include "common/sqlite_compat.h"
#include "common/time_utils.h"
#include "datastore/history_store.h"
#include "model/data_item_keys.h"

namespace edge_controller {

// 仅测试连接级并发属性；不向产品 API 暴露数据库句柄或测试延迟开关。
struct HistoryStoreTestPeer {
    static std::size_t install_read_progress_handler(
        HistoryStore* store,
        int (*handler)(void*),
        void* state)
    {
        std::lock_guard<std::mutex> lifecycle_lock(store->mutex_);
        std::size_t installed = 0;
        for (auto& connection : store->read_connections_) {
            if (connection == nullptr) continue;
            std::lock_guard<std::mutex> connection_lock(connection->mutex);
            if (connection->database == nullptr) continue;
            sqlite3_progress_handler(connection->database, 1, handler, state);
            ++installed;
        }
        return installed;
    }

    static bool read_connections_are_read_only(HistoryStore* store)
    {
        std::lock_guard<std::mutex> lifecycle_lock(store->mutex_);
        std::size_t found = 0;
        for (auto& connection : store->read_connections_) {
            if (connection == nullptr) continue;
            std::lock_guard<std::mutex> connection_lock(connection->mutex);
            if (connection->database == nullptr) continue;
            ++found;
            if (sqlite3_db_readonly(connection->database, "main") != 1) return false;
            sqlite3_stmt* statement = nullptr;
            if (sqlite3_prepare_v2(connection->database, "PRAGMA query_only;", -1, &statement, nullptr) != SQLITE_OK) {
                return false;
            }
            const bool query_only = sqlite3_step(statement) == SQLITE_ROW &&
                                    sqlite3_column_int(statement, 0) == 1;
            sqlite3_finalize(statement);
            if (!query_only) return false;
        }
        return found == HistoryStore::kReadConnectionCount;
    }
};

}  // namespace edge_controller

namespace {

using edge_controller::HistoryExportCursor;
using edge_controller::HistoryExportQuery;
using edge_controller::HistoryRecord;
using edge_controller::HistoryStore;
using edge_controller::HistoryStoreTestPeer;
using edge_controller::StatusCode;
using edge_controller::TimestampMs;
using edge_controller::is_ok;

class TemporaryDatabase {
public:
    explicit TemporaryDatabase(std::string suffix)
    {
        const auto unique = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = (edge::fs::temp_directory_path() /
                 ("edge-history-store-" + std::move(suffix) + "-" + unique + ".db"))
                    .string();
    }

    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

    ~TemporaryDatabase()
    {
        std::error_code ignored;
        edge::fs::remove(path_, ignored);
        edge::fs::remove(path_ + "-wal", ignored);
        edge::fs::remove(path_ + "-shm", ignored);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool expect(bool condition, const std::string& message)
{
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

bool execute_sql(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(
            path.c_str(),
            &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        std::cerr << "open sqlite helper failed: "
                  << (database == nullptr ? "no database" : sqlite3_errmsg(database)) << '\n';
        if (database != nullptr) sqlite3_close(database);
        return false;
    }
    sqlite3_busy_timeout(database, 5000);
    char* raw_error = nullptr;
    const auto status = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &raw_error);
    if (status != SQLITE_OK) {
        std::cerr << "sqlite helper exec failed: "
                  << (raw_error == nullptr ? sqlite3_errmsg(database) : raw_error) << '\n';
        sqlite3_free(raw_error);
        sqlite3_close(database);
        return false;
    }
    sqlite3_close(database);
    return true;
}

std::int64_t scalar_int64(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (database != nullptr) sqlite3_close(database);
        return -1;
    }
    sqlite3_busy_timeout(database, 5000);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        if (statement != nullptr) sqlite3_finalize(statement);
        sqlite3_close(database);
        return -1;
    }
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

HistoryRecord make_raw_record(TimestampMs bucket_start_ms, double value, std::string device_id = "device-1")
{
    HistoryRecord record;
    record.device_id = std::move(device_id);
    record.master_id = "master-1";
    record.channel_id = "channel-1";
    record.template_id = "RD100";
    record.sample_period = "raw_10min";
    record.bucket_start_ms = bucket_start_ms;
    record.bucket_text = std::to_string(bucket_start_ms);
    record.timestamp_ms = bucket_start_ms + 1;
    record.date = edge_controller::history_date_from_timestamp(bucket_start_ms);
    record.point_key = edge_controller::kResistanceFieldKey;
    record.point_name = "Resistance";
    record.unit = "ohm";
    record.precision = 2;
    record.value = value;
    record.raw_value = value;
    record.quality = "good";
    record.valid = true;
    return record;
}

TimestampMs recent_bucket(TimestampMs now_ms, std::uint32_t buckets_ago)
{
    constexpr TimestampMs bucket_ms = 10ULL * 60ULL * 1000ULL;
    return (now_ms / bucket_ms) * bucket_ms - static_cast<TimestampMs>(buckets_ago) * bucket_ms;
}

struct ProgressGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool released{false};
};

int block_first_progress_callback(void* opaque)
{
    auto* gate = static_cast<ProgressGate*>(opaque);
    std::unique_lock<std::mutex> lock(gate->mutex);
    if (!gate->entered) {
        gate->entered = true;
        gate->condition.notify_all();
    }
    gate->condition.wait(lock, [&]() { return gate->released; });
    return 0;
}

bool test_reads_do_not_block_writes()
{
    TemporaryDatabase database("concurrency");
    HistoryStore store;
    std::string error;
    if (!is_ok(store.initialize(database.path())) ||
        !is_ok(store.upsert_records({make_raw_record(recent_bucket(edge_controller::time_utils::system_now_ms(), 2), 1.0)}))) {
        std::cerr << "concurrency fixture initialize failed: " << error << '\n';
        return false;
    }
    if (!expect(
            HistoryStoreTestPeer::read_connections_are_read_only(&store),
            "history queries must use two independently locked read-only connections")) {
        return false;
    }

    ProgressGate gate;
    if (!expect(
            HistoryStoreTestPeer::install_read_progress_handler(
                &store, block_first_progress_callback, &gate) == 2,
            "progress handler must be installed on both read connections")) {
        return false;
    }

    auto reader = std::async(std::launch::async, [&]() {
        std::uint64_t count = 0;
        std::string query_error;
        return store.count_by_period("raw_10min", &count, &query_error);
    });
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.condition.wait_for(lock, std::chrono::seconds(2), [&]() { return gate.entered; })) {
            gate.released = true;
            gate.condition.notify_all();
            (void)reader.get();
            std::cerr << "read query never reached the deterministic progress gate\n";
            return false;
        }
    }

    auto writer = std::async(std::launch::async, [&]() {
        return store.upsert_records({make_raw_record(
            recent_bucket(edge_controller::time_utils::system_now_ms(), 1), 2.0)});
    });
    const bool writer_completed_while_reader_blocked =
        writer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.released = true;
    }
    gate.condition.notify_all();

    const auto writer_status = writer.get();
    const auto reader_status = reader.get();
    HistoryStoreTestPeer::install_read_progress_handler(&store, nullptr, nullptr);
    return expect(
               writer_completed_while_reader_blocked,
               "a blocked long read must not hold the HistoryStore writer mutex") &&
           expect(is_ok(writer_status), "writer must commit while a WAL reader remains active") &&
           expect(is_ok(reader_status), "blocked reader must complete after release");
}

bool test_clean_restart_skips_and_dirty_restart_rebuilds()
{
    TemporaryDatabase database("marker");
    const auto now_ms = edge_controller::time_utils::system_now_ms();
    {
        HistoryStore store;
        std::string error;
        if (!is_ok(store.initialize(database.path())) ||
            !is_ok(store.upsert_records({make_raw_record(recent_bucket(now_ms, 2), 10.0)}))) {
            std::cerr << "marker fixture initialize failed: " << error << '\n';
            return false;
        }
    }

    if (!execute_sql(
            database.path(),
            "CREATE TABLE rebuild_probe(insert_count INTEGER NOT NULL);"
            "INSERT INTO rebuild_probe VALUES(0);"
            "CREATE TRIGGER probe_aggregate_insert AFTER INSERT ON history_samples "
            "WHEN NEW.sample_period IN ('hour', 'day') BEGIN "
            "UPDATE rebuild_probe SET insert_count = insert_count + 1; END;")) {
        return false;
    }
    {
        HistoryStore store;
        std::string error;
        if (!is_ok(store.initialize(database.path()))) {
            std::cerr << "clean restart failed: " << error << '\n';
            return false;
        }
    }
    if (!expect(
            scalar_int64(database.path(), "SELECT insert_count FROM rebuild_probe;") == 0,
            "a clean marker must skip the full retained-window aggregate rebuild")) {
        return false;
    }

    if (!execute_sql(
            database.path(),
            "UPDATE history_aggregate_state SET dirty = 1 WHERE singleton_id = 1;"
            "DELETE FROM history_samples WHERE sample_period IN ('hour', 'day');"
            "UPDATE rebuild_probe SET insert_count = 0;")) {
        return false;
    }
    {
        HistoryStore store;
        std::string error;
        if (!is_ok(store.initialize(database.path()))) {
            std::cerr << "dirty restart failed: " << error << '\n';
            return false;
        }
    }
    return expect(
               scalar_int64(database.path(), "SELECT insert_count FROM rebuild_probe;") > 0,
               "a dirty marker must execute aggregate rebuild writes") &&
           expect(
               scalar_int64(database.path(), "SELECT dirty FROM history_aggregate_state WHERE singleton_id = 1;") == 0,
               "a successful dirty rebuild must clear the marker") &&
           expect(
               scalar_int64(database.path(), "SELECT COUNT(*) FROM history_samples WHERE sample_period = 'hour';") > 0 &&
                   scalar_int64(database.path(), "SELECT COUNT(*) FROM history_samples WHERE sample_period = 'day';") > 0,
               "dirty rebuild must restore both aggregate layers");
}

bool test_aggregate_failure_commits_dirty_with_raw()
{
    TemporaryDatabase database("aggregate-failure");
    const auto now_ms = edge_controller::time_utils::system_now_ms();
    {
        HistoryStore store;
        std::string error;
        if (!is_ok(store.initialize(database.path()))) {
            std::cerr << "aggregate failure fixture initialize failed: " << error << '\n';
            return false;
        }
        if (!execute_sql(
                database.path(),
                "CREATE TRIGGER fail_aggregate_insert BEFORE INSERT ON history_samples "
                "WHEN NEW.sample_period IN ('hour', 'day') BEGIN "
                "SELECT RAISE(FAIL, 'forced aggregate failure'); END;")) {
            return false;
        }
        if (!expect(
                is_ok(store.upsert_records({make_raw_record(recent_bucket(now_ms, 1), 20.0)})),
                "raw samples must retain the existing success semantics when aggregate fallback is marked dirty")) {
            return false;
        }
    }

    if (!expect(
            scalar_int64(database.path(), "SELECT COUNT(*) FROM history_samples WHERE sample_period = 'raw_10min';") == 1,
            "aggregate failure must commit its accepted raw sample") ||
        !expect(
            scalar_int64(database.path(), "SELECT dirty FROM history_aggregate_state WHERE singleton_id = 1;") == 1,
            "aggregate failure and raw commit must atomically persist dirty=1")) {
        return false;
    }
    if (!execute_sql(database.path(), "DROP TRIGGER fail_aggregate_insert;")) return false;
    {
        HistoryStore store;
        std::string error;
        if (!is_ok(store.initialize(database.path()))) {
            std::cerr << "aggregate failure recovery restart failed: " << error << '\n';
            return false;
        }
    }
    return expect(
               scalar_int64(database.path(), "SELECT dirty FROM history_aggregate_state WHERE singleton_id = 1;") == 0,
               "restart after aggregate failure must rebuild and clear dirty") &&
           expect(
               scalar_int64(database.path(), "SELECT COUNT(*) FROM history_samples WHERE sample_period = 'hour';") == 1,
               "restart after aggregate failure must restore the missing hour aggregate");
}

bool test_keyset_cursor_and_offset_compatibility()
{
    TemporaryDatabase database("cursor");
    const auto now_ms = edge_controller::time_utils::system_now_ms();
    std::vector<HistoryRecord> source;
    for (std::uint32_t buckets_ago = 5; buckets_ago > 0; --buckets_ago) {
        source.push_back(make_raw_record(
            recent_bucket(now_ms, buckets_ago), static_cast<double>(buckets_ago), "cursor-device"));
    }

    HistoryStore store;
    std::string error;
    if (!is_ok(store.initialize(database.path())) || !is_ok(store.upsert_records(source))) {
        std::cerr << "cursor fixture initialize failed: " << error << '\n';
        return false;
    }

    HistoryExportQuery query;
    query.device_id = "cursor-device";
    query.sample_period = "raw_10min";
    query.limit = 2;
    query.offset = 1;
    std::vector<HistoryRecord> offset_page;
    if (!is_ok(store.export_records(query, &offset_page, &error)) ||
        !expect(
            offset_page.size() == 2 && offset_page[0].bucket_start_ms == source[1].bucket_start_ms,
            "public history export offset semantics must remain compatible")) {
        return false;
    }

    query.offset = 0;
    std::vector<HistoryRecord> first_page;
    HistoryExportCursor first_cursor;
    if (!is_ok(store.export_records_after(query, nullptr, &first_page, &first_cursor, &error)) ||
        !expect(first_page.size() == 2 && first_cursor.valid, "first keyset page must return a stable cursor")) {
        return false;
    }
    // 轮询会用 INSERT OR REPLACE 更新当前 10 分钟桶；游标不得因 rowid 变化而重复该桶。
    if (!is_ok(store.upsert_records({make_raw_record(
            source[1].bucket_start_ms, 99.0, "cursor-device")}))) {
        return false;
    }
    if (!execute_sql(
            database.path(),
            "DELETE FROM history_samples WHERE sample_period = 'raw_10min' AND device_id = 'cursor-device' "
            "AND bucket_start_ms = " + std::to_string(source[0].bucket_start_ms) + ";")) {
        return false;
    }

    std::vector<HistoryRecord> second_page;
    HistoryExportCursor second_cursor;
    if (!is_ok(store.export_records_after(
            query, &first_cursor, &second_page, &second_cursor, &error))) {
        std::cerr << "second keyset page failed: " << error << '\n';
        return false;
    }
    return expect(
        second_page.size() == 2 &&
            second_page[0].bucket_start_ms == source[2].bucket_start_ms &&
            second_page[1].bucket_start_ms == source[3].bucket_start_ms,
        "keyset pagination must not skip a row when an earlier row is deleted between batches");
}

}  // namespace

int main()
{
    if (!test_reads_do_not_block_writes()) return 1;
    if (!test_clean_restart_skips_and_dirty_restart_rebuilds()) return 1;
    if (!test_aggregate_failure_commits_dirty_with_raw()) return 1;
    if (!test_keyset_cursor_and_offset_compatibility()) return 1;
    return 0;
}
