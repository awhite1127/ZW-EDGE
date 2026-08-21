// 配置仓库门面：管理配置库连接和公共事务，具体领域读写拆分在 config_store_*.cpp。
#include "datastore/config_store.h"
#include "datastore/config_store_internal.h"
#include <mutex>
#include <string>
#include <vector>
#include "common/filesystem_compat.h"
#include "common/logger.h"
#include "common/sqlite_compat.h"

namespace edge_controller {

using namespace config_store_internal;

// 关闭数据库连接并释放存储资源；内部加锁保证析构安全。
ConfigStore::~ConfigStore()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_database_locked();
}

// 初始化配置数据库连接和表结构。
StatusCode ConfigStore::initialize(
    const std::string& database_path,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_database_locked();
    database_path_.clear();
    initialized_ = false;

    const auto resolved_path = database_path.empty()
                                   ? DatabasePaths(std::string{}).config_database()
                                   : database_path;
    std::string error;
    auto status = open_database_locked(resolved_path, &error);
    if (is_ok(status)) {
        status = initialize_schema_locked(&error);
    }
    if (!is_ok(status)) {
        const auto message = error.empty() ? "配置 SQLite 存储初始化失败" : error;
        Logger::warn(message);
        close_database_locked();
        if (error_message != nullptr) *error_message = message;
        return status;
    }

    initialized_ = true;
    Logger::info("配置 SQLite 存储初始化完成，路径=" + database_path_);
    return StatusCode::kOk;
}

StatusCode ConfigStore::open_database_locked(const std::string& database_path, std::string* error_message)
{
    const edge::fs::path path(database_path);
    const auto directory_status = ensure_parent_directory(path, error_message);
    if (!is_ok(directory_status)) {
        return directory_status;
    }

    sqlite3* opened_database = nullptr;
    const auto open_status = sqlite3_open_v2(
        database_path.c_str(),
        &opened_database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (open_status != SQLITE_OK) {
        const auto detail = sqlite_error(opened_database);
        if (opened_database != nullptr) {
            sqlite3_close(opened_database);
        }
        if (error_message != nullptr) {
            *error_message = "打开配置 SQLite 数据库失败：" + database_path + "，原因=" + detail;
        }
        return StatusCode::kIoError;
    }

    database_ = opened_database;
    database_path_ = database_path;
    const auto configure_status = configure_database_locked(error_message);
    if (!is_ok(configure_status)) {
        close_database_locked();
        return configure_status;
    }
    return StatusCode::kOk;
}

// 在持锁状态下打开并配置 SQLite 数据库。
StatusCode ConfigStore::configure_database_locked(std::string* error_message)
{
    const char* configure_sql =
        "PRAGMA busy_timeout = 5000;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA wal_autocheckpoint = 100;"
        "PRAGMA foreign_keys = ON;";
    std::string detail;
    const auto configure_status = execute_sql_locked(configure_sql, &detail);
    if (!is_ok(configure_status)) {
        if (error_message != nullptr) {
            *error_message = "配置 SQLite 连接失败：" +
                             (detail.empty() ? sqlite_error(database_) : detail);
        }
        return configure_status;
    }
    return StatusCode::kOk;
}

// 返回配置数据库文件路径。
std::string ConfigStore::database_path() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return database_path_;
}

// 在持锁状态下关闭数据库。
void ConfigStore::close_database_locked()
{
    if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
    initialized_ = false;
}

}  // namespace edge_controller
