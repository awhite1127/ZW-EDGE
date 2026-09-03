// SQLite 公共辅助：封装语句准备、绑定、事务和错误转换，供各配置领域复用。
#include "data/datastore/config_store.h"
#include "data/datastore/config_store_internal.h"
#include <string>
#include "shared/common/sqlite_compat.h"
#include "shared/common/time_utils.h"

namespace edge_controller {

using namespace config_store_internal;

namespace {

constexpr const char* kFirstBootAdminEntryUsedKey = "first_boot_admin_entry_used";

}  // namespace

// 加载首次启动管理员入口可用状态。
StatusCode ConfigStore::load_first_boot_admin_entry_available(
    bool* available,
    std::string* error_message) const
{
    if (available == nullptr) {
        if (error_message != nullptr) *error_message = "首次部署入口状态参数为空";
        return StatusCode::kInvalidArgument;
    }
    *available = false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    Statement statement(database_, "SELECT value FROM config_meta WHERE key = ?;");
    if (!statement.ok() || !bind_text(statement.get(), 1, kFirstBootAdminEntryUsedKey)) {
        if (error_message != nullptr) *error_message = "准备读取首次部署入口状态失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        // 升级前的既有数据库没有该标记；为避免升级后开放免密入口，安全地按已使用处理。
        return StatusCode::kOk;
    }
    if (step != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "读取首次部署入口状态失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }

    const auto value = column_text(statement.get(), 0);
    if (value == "false") {
        *available = true;
        return StatusCode::kOk;
    }
    if (value == "true") {
        return StatusCode::kOk;
    }
    if (error_message != nullptr) *error_message = "首次部署入口状态值无效";
    return StatusCode::kInvalidState;
}

// 消费首次启动管理员入口，使其后续不可再次使用。
StatusCode ConfigStore::consume_first_boot_admin_entry(
    bool* consumed,
    std::string* error_message)
{
    if (consumed == nullptr) {
        if (error_message != nullptr) *error_message = "首次部署入口消费结果参数为空";
        return StatusCode::kInvalidArgument;
    }
    *consumed = false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "UPDATE config_meta SET value = 'true', updated_at = ? "
        "WHERE key = ? AND value = 'false';";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备关闭首次部署入口失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto updated_at = time_utils::local_time_string();
    if (!bind_text(statement.get(), 1, updated_at) ||
        !bind_text(statement.get(), 2, kFirstBootAdminEntryUsedKey)) {
        if (error_message != nullptr) *error_message = "绑定首次部署入口状态失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "关闭首次部署入口失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    *consumed = sqlite3_changes(database_) == 1;
    return StatusCode::kOk;
}

// 重置首次启动管理员入口状态。
StatusCode ConfigStore::reset_first_boot_admin_entry(std::string* error_message)
{
    return set_meta(kFirstBootAdminEntryUsedKey, "false", error_message);
}

// 设置元数据。
StatusCode ConfigStore::set_meta(
    const std::string& key,
    const std::string& value,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return set_meta_locked(key, value, error_message);
}

// 在持锁状态下写入配置数据库元数据。
StatusCode ConfigStore::set_meta_locked(
    const std::string& key,
    const std::string& value,
    std::string* error_message)
{
    if (key.empty()) {
        if (error_message != nullptr) {
            *error_message = "配置元数据 key 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "INSERT OR REPLACE INTO config_meta (key, value, updated_at) "
        "VALUES (?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备写入 config_meta 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto updated_at = time_utils::local_time_string();
    if (!bind_text(statement.get(), 1, key) ||
        !bind_text(statement.get(), 2, value) ||
        !bind_text(statement.get(), 3, updated_at)) {
        if (error_message != nullptr) {
            *error_message = "绑定 config_meta 参数失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "写入 config_meta 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 在持锁状态下执行SQL。
StatusCode ConfigStore::execute_sql_locked(const char* sql, std::string* error_message) const
{
    if (!database_open_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    char* raw_error = nullptr;
    const auto status = sqlite3_exec(database_, sql, nullptr, nullptr, &raw_error);
    if (status != SQLITE_OK) {
        if (error_message != nullptr) {
            *error_message = raw_error == nullptr ? sqlite_error(database_) : raw_error;
        }
        sqlite3_free(raw_error);
        return StatusCode::kIoError;
    }
    sqlite3_free(raw_error);
    return StatusCode::kOk;
}

// 在持锁状态下判断数据库句柄是否已打开。
bool ConfigStore::database_open_locked(std::string* error_message) const
{
    if (database_ != nullptr) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = "配置 SQLite 数据库未打开";
    }
    return false;
}

// 在持锁状态下判断数据库是否可正常读写。
bool ConfigStore::database_ready_locked(std::string* error_message) const
{
    if (database_ != nullptr && initialized_) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = database_ == nullptr ? "配置 SQLite 数据库未打开" : "配置 SQLite 数据库未初始化";
    }
    return false;
}

}  // namespace edge_controller
