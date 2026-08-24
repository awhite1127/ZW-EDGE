#include "datastore/alarm_store.h"
#include "service/alarm_evaluator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/filesystem_compat.h"
#include "common/sqlite_compat.h"

namespace {

using edge_controller::AlarmEvaluator;
using edge_controller::AlarmPointContext;
using edge_controller::AlarmRule;
using edge_controller::AlarmRuntimeState;
using edge_controller::AlarmStore;
using edge_controller::DataQuality;
using edge_controller::DeviceStatus;
using edge_controller::PointValue;
using edge_controller::ServiceEvent;
using edge_controller::StatusCode;
using edge_controller::TimestampMs;
using edge_controller::is_ok;

constexpr const char* kAlarmSchema =
    "CREATE TABLE alarm_rules("
    "device_id TEXT NOT NULL,point_key TEXT NOT NULL,enabled INTEGER NOT NULL,"
    "high_enabled INTEGER NOT NULL,high_threshold REAL NOT NULL,low_enabled INTEGER NOT NULL,"
    "low_threshold REAL NOT NULL,level TEXT NOT NULL,hysteresis REAL NOT NULL,"
    "trigger_count INTEGER NOT NULL,recovery_count INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL,"
    "PRIMARY KEY(device_id,point_key));"
    "CREATE TABLE alarm_runtime_states("
    "device_id TEXT NOT NULL,point_key TEXT NOT NULL,state TEXT NOT NULL,direction TEXT NOT NULL,"
    "current_value REAL NOT NULL,threshold_value REAL NOT NULL,"
    "consecutive_trigger_count INTEGER NOT NULL,consecutive_recovery_count INTEGER NOT NULL,"
    "active_since_ms INTEGER NOT NULL,last_evaluated_at_ms INTEGER NOT NULL,"
    "acknowledged INTEGER NOT NULL DEFAULT 0,acknowledged_at_ms INTEGER NOT NULL DEFAULT 0,"
    "acknowledged_by TEXT NOT NULL DEFAULT '',updated_at_ms INTEGER NOT NULL,"
    "PRIMARY KEY(device_id,point_key));";

bool expect(bool condition, const std::string& message)
{
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

class TemporaryAlarmDatabase {
public:
    TemporaryAlarmDatabase()
    {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        auto directory = edge::fs::temp_directory_path(error);
        if (error) {
            error.clear();
            directory = edge::fs::current_path(error);
        }
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = (directory /
                 ("edge-alarm-evaluator-batch-" + std::to_string(unique) + "-" +
                  std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".db"))
                    .string();
    }

    ~TemporaryAlarmDatabase()
    {
        std::error_code ignored;
        edge::fs::remove(path_, ignored);
        ignored.clear();
        edge::fs::remove(path_ + "-wal", ignored);
        ignored.clear();
        edge::fs::remove(path_ + "-shm", ignored);
    }

    const std::string& path() const { return path_; }

    bool execute(const std::string& sql, std::string* error) const
    {
        sqlite3* database = nullptr;
        const auto open_status = sqlite3_open_v2(
            path_.c_str(),
            &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (open_status != SQLITE_OK) {
            if (error != nullptr) {
                *error = database == nullptr ? "cannot allocate SQLite connection" : sqlite3_errmsg(database);
            }
            if (database != nullptr) sqlite3_close(database);
            return false;
        }
        sqlite3_busy_timeout(database, 5000);
        char* sqlite_error = nullptr;
        const auto status = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &sqlite_error);
        if (status != SQLITE_OK && error != nullptr) {
            *error = sqlite_error == nullptr ? sqlite3_errmsg(database) : sqlite_error;
        }
        if (sqlite_error != nullptr) sqlite3_free(sqlite_error);
        sqlite3_close(database);
        return status == SQLITE_OK;
    }

private:
    std::string path_;
};

AlarmRule make_rule(const std::string& device_id)
{
    AlarmRule rule;
    rule.device_id = device_id;
    rule.point_key = "temperature";
    rule.enabled = true;
    rule.high_enabled = true;
    rule.high_threshold = 10.0;
    rule.level = "warning";
    rule.hysteresis = 1.0;
    rule.trigger_count = 1;
    rule.recovery_count = 1;
    rule.updated_at_ms = 1;
    return rule;
}

AlarmPointContext make_context(const std::string& device_id)
{
    AlarmPointContext context;
    context.device_id = device_id;
    context.device_name = device_id;
    context.master_id = "master";
    context.template_id = "template";
    context.point_key = "temperature";
    context.point_name = "Temperature";
    context.unit = "C";
    context.precision = 1;
    return context;
}

DeviceStatus make_status(const std::string& device_id, double value, TimestampMs timestamp_ms)
{
    DeviceStatus status;
    status.device_id = device_id;
    status.device_name = device_id;
    status.master_id = "master";
    status.template_id = "template";
    status.online = true;
    status.last_collect_success = true;
    status.communication_quality = DataQuality::kGood;
    status.updated_at_ms = timestamp_ms;

    PointValue point;
    point.key = "temperature";
    point.name = "Temperature";
    point.value = value;
    point.quality = DataQuality::kGood;
    point.valid = true;
    point.sample_time_ms = timestamp_ms;
    status.points.push_back(std::move(point));
    return status;
}

bool initialize_fixture(
    TemporaryAlarmDatabase* database,
    AlarmStore* store,
    AlarmEvaluator* evaluator,
    const std::vector<std::string>& device_ids,
    AlarmEvaluator::EventCallback callback,
    std::string* error)
{
    if (database == nullptr || store == nullptr || evaluator == nullptr) return false;
    if (!database->execute(kAlarmSchema, error)) return false;
    if (!is_ok(store->initialize(database->path(), error))) return false;

    std::vector<AlarmPointContext> contexts;
    contexts.reserve(device_ids.size());
    for (const auto& device_id : device_ids) {
        if (!is_ok(store->upsert_rule(make_rule(device_id), error))) return false;
        contexts.push_back(make_context(device_id));
    }
    return is_ok(evaluator->initialize(store, contexts, std::move(callback), error));
}

bool expect_runtime_state_count(AlarmStore* store, std::size_t expected, const std::string& phase)
{
    std::vector<AlarmRuntimeState> states;
    std::string error;
    if (store == nullptr || !is_ok(store->list_runtime_states(&states, &error))) {
        std::cerr << phase << " could not list runtime states: " << error << '\n';
        return false;
    }
    return expect(states.size() == expected, phase + " runtime state count mismatch");
}

bool test_failure_atomicity()
{
    TemporaryAlarmDatabase database;
    AlarmStore store;
    AlarmEvaluator evaluator;
    std::mutex event_mutex;
    std::vector<std::string> event_targets;
    std::string error;
    if (!initialize_fixture(
            &database,
            &store,
            &evaluator,
            {"a-ok", "z-fail"},
            [&](ServiceEvent event) {
                std::lock_guard<std::mutex> lock(event_mutex);
                event_targets.push_back(std::move(event.target_id));
            },
            &error)) {
        std::cerr << "failure fixture initialization failed: " << error << '\n';
        return false;
    }

    // persistence_updates 按键排序，因此 a-ok 会先成功写入，再由 z-fail 注入错误。
    if (!database.execute(
            "CREATE TRIGGER fail_alarm_upsert BEFORE INSERT ON alarm_runtime_states "
            "WHEN NEW.device_id='z-fail' BEGIN SELECT RAISE(ABORT,'injected upsert failure'); END;",
            &error)) {
        std::cerr << "could not create upsert failure trigger: " << error << '\n';
        return false;
    }

    const std::vector<DeviceStatus> high_batch{
        make_status("z-fail", 20.0, 100),
        make_status("a-ok", 20.0, 101),
    };
    error.clear();
    if (!expect(
            !is_ok(evaluator.evaluate_batch(high_batch, &error)),
            "injected upsert failure must fail the complete batch") ||
        !expect_runtime_state_count(&store, 0, "failed upsert batch") ||
        !expect(evaluator.list_active_alarms().empty(), "failed upsert batch changed evaluator memory") ||
        !expect(event_targets.empty(), "failed upsert batch dispatched events")) {
        return false;
    }

    if (!database.execute("DROP TRIGGER fail_alarm_upsert;", &error)) {
        std::cerr << "could not drop upsert failure trigger: " << error << '\n';
        return false;
    }
    error.clear();
    if (!is_ok(evaluator.evaluate_batch(high_batch, &error)) ||
        !expect_runtime_state_count(&store, 2, "successful upsert batch") ||
        !expect(evaluator.list_active_alarms().size() == 2, "successful upsert batch did not publish memory")) {
        std::cerr << "successful upsert batch failed: " << error << '\n';
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        if (!expect(
                event_targets == std::vector<std::string>({"z-fail", "a-ok"}),
                "trigger events did not preserve input device order")) {
            return false;
        }
        event_targets.clear();
    }

    // 删除也复用一个 statement 并共享事务；第二个 delete 失败时第一个必须恢复。
    if (!database.execute(
            "CREATE TRIGGER fail_alarm_delete BEFORE DELETE ON alarm_runtime_states "
            "WHEN OLD.device_id='z-fail' BEGIN SELECT RAISE(ABORT,'injected delete failure'); END;",
            &error)) {
        std::cerr << "could not create delete failure trigger: " << error << '\n';
        return false;
    }
    const std::vector<DeviceStatus> recovery_batch{
        make_status("z-fail", 5.0, 200),
        make_status("a-ok", 5.0, 201),
    };
    error.clear();
    if (!expect(
            !is_ok(evaluator.evaluate_batch(recovery_batch, &error)),
            "injected delete failure must fail the complete batch") ||
        !expect_runtime_state_count(&store, 2, "failed delete batch") ||
        !expect(evaluator.list_active_alarms().size() == 2, "failed delete batch changed evaluator memory") ||
        !expect(event_targets.empty(), "failed delete batch dispatched recovery events")) {
        return false;
    }

    if (!database.execute("DROP TRIGGER fail_alarm_delete;", &error)) {
        std::cerr << "could not drop delete failure trigger: " << error << '\n';
        return false;
    }
    error.clear();
    if (!is_ok(evaluator.evaluate_batch(recovery_batch, &error)) ||
        !expect_runtime_state_count(&store, 0, "successful delete batch") ||
        !expect(evaluator.list_active_alarms().empty(), "successful delete batch did not publish memory")) {
        std::cerr << "successful delete batch failed: " << error << '\n';
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(event_mutex);
        return expect(
            event_targets == std::vector<std::string>({"z-fail", "a-ok"}),
            "recovery events did not preserve input device order");
    }
}

bool test_concurrent_batches_and_unlocked_dispatch()
{
    TemporaryAlarmDatabase database;
    AlarmStore store;
    AlarmEvaluator evaluator;
    std::mutex event_mutex;
    std::vector<std::string> event_targets;
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool first_callback_entered = false;
    bool release_first_callback = false;
    std::string error;
    if (!initialize_fixture(
            &database,
            &store,
            &evaluator,
            {"a-1", "a-2", "b-1", "b-2"},
            [&](ServiceEvent event) {
                const auto target_id = event.target_id;
                {
                    std::lock_guard<std::mutex> lock(event_mutex);
                    event_targets.push_back(target_id);
                }
                if (target_id == "a-1") {
                    std::unique_lock<std::mutex> lock(callback_mutex);
                    first_callback_entered = true;
                    callback_condition.notify_all();
                    callback_condition.wait(lock, [&]() { return release_first_callback; });
                }
            },
            &error)) {
        std::cerr << "concurrency fixture initialization failed: " << error << '\n';
        return false;
    }

    const std::vector<DeviceStatus> batch_a{
        make_status("a-1", 20.0, 300),
        make_status("a-2", 20.0, 301),
    };
    const std::vector<DeviceStatus> batch_b{
        make_status("b-1", 20.0, 302),
        make_status("b-2", 20.0, 303),
    };
    StatusCode status_a = StatusCode::kInternalError;
    StatusCode status_b = StatusCode::kInternalError;
    std::string error_a;
    std::string error_b;
    std::thread thread_a([&]() { status_a = evaluator.evaluate_batch(batch_a, &error_a); });

    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        if (!callback_condition.wait_for(
                lock, std::chrono::seconds(5), [&]() { return first_callback_entered; })) {
            release_first_callback = true;
            lock.unlock();
            callback_condition.notify_all();
            thread_a.join();
            std::cerr << "first batch never reached its event callback\n";
            return false;
        }
    }

    std::mutex completion_mutex;
    std::condition_variable completion_condition;
    bool batch_b_completed = false;
    std::thread thread_b([&]() {
        status_b = evaluator.evaluate_batch(batch_b, &error_b);
        {
            std::lock_guard<std::mutex> lock(completion_mutex);
            batch_b_completed = true;
        }
        completion_condition.notify_one();
    });

    bool completed_while_callback_blocked = false;
    {
        std::unique_lock<std::mutex> lock(completion_mutex);
        completed_while_callback_blocked = completion_condition.wait_for(
            lock, std::chrono::seconds(5), [&]() { return batch_b_completed; });
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        release_first_callback = true;
    }
    callback_condition.notify_all();
    thread_b.join();
    thread_a.join();

    if (!expect(
            completed_while_callback_blocked,
            "second batch was blocked by the first batch's external event callback") ||
        !expect(is_ok(status_a), "first concurrent batch failed: " + error_a) ||
        !expect(is_ok(status_b), "second concurrent batch failed: " + error_b) ||
        !expect_runtime_state_count(&store, 4, "concurrent batches") ||
        !expect(evaluator.list_active_alarms().size() == 4, "concurrent batches lost evaluator state")) {
        return false;
    }
    std::lock_guard<std::mutex> lock(event_mutex);
    return expect(
        event_targets == std::vector<std::string>({"a-1", "a-2", "b-1", "b-2"}),
        "concurrent batch events interleaved or violated FIFO order");
}

}  // namespace

int main()
{
    if (!test_failure_atomicity()) return 1;
    if (!test_concurrent_batches_and_unlocked_dispatch()) return 1;
    return 0;
}
