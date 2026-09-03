// 时间配置持久化：保存时区与 NTP 参数，不直接修改系统时钟或 RTC。
#include "data/datastore/config_store.h"

#include <mutex>
#include <string>

#include "shared/common/sqlite_compat.h"
#include "shared/common/time_utils.h"
#include "data/datastore/config_store_internal.h"
#include "infrastructure/platform/linux_time_runtime.h"

namespace edge_controller {

using namespace config_store_internal;

// 从数据库加载时间设置。
StatusCode ConfigStore::load_time_settings(TimeSettings* settings, std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_time_settings_locked(settings, error_message);
}

// 读取并初始化时间设置。
StatusCode ConfigStore::load_or_initialize_time_settings(
    TimeSettings* settings,
    std::string* error_message)
{
    if (settings == nullptr) {
        if (error_message != nullptr) *error_message = "缺少时间设置输出参数";
        return StatusCode::kInvalidArgument;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto status = load_time_settings_locked(settings, error_message);
        if (is_ok(status)) return status;
        if (status != StatusCode::kNotFound) return status;
        if (error_message != nullptr) error_message->clear();
    }

    TimeSettings initialized;
    initialized.updated_at = time_utils::system_now_ms();
    const auto validation = LinuxTimeRuntime::validate_settings_shape(initialized, error_message);
    if (!is_ok(validation)) return validation;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto save_status = save_time_settings_locked(initialized, error_message);
    if (!is_ok(save_status)) return save_status;
    const auto meta_status = set_meta_locked("time_settings_initialized", "true", error_message);
    if (!is_ok(meta_status)) return meta_status;
    *settings = std::move(initialized);
    return StatusCode::kOk;
}

// 保存时间设置。
StatusCode ConfigStore::save_time_settings(const TimeSettings& settings, std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_time_settings_locked(settings, error_message);
}

// 在持锁状态下加载时间设置。
StatusCode ConfigStore::load_time_settings_locked(TimeSettings* settings, std::string* error_message) const
{
    if (settings == nullptr) {
        if (error_message != nullptr) *error_message = "缺少时间设置输出参数";
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    Statement statement(database_,
        "SELECT timezone, ntp_enabled, ntp_primary, ntp_secondary, updated_at "
        "FROM time_settings WHERE id = 1;");
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备读取 time_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "time_settings 尚未初始化";
        return StatusCode::kNotFound;
    }
    if (step != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "读取 time_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    TimeSettings loaded;
    loaded.timezone = column_text(statement.get(), 0);
    loaded.ntp_enabled = sqlite3_column_int(statement.get(), 1) != 0;
    loaded.ntp_primary = column_text(statement.get(), 2);
    loaded.ntp_secondary = column_text(statement.get(), 3);
    loaded.updated_at = static_cast<TimestampMs>(column_int64(statement.get(), 4));
    const auto validation = LinuxTimeRuntime::validate_settings_shape(loaded, error_message);
    if (!is_ok(validation)) return validation;
    *settings = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存时间设置。
StatusCode ConfigStore::save_time_settings_locked(const TimeSettings& settings, std::string* error_message)
{
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    const auto validation = LinuxTimeRuntime::validate_settings_shape(settings, error_message);
    if (!is_ok(validation)) return validation;
    Statement statement(database_,
        "INSERT OR REPLACE INTO time_settings "
        "(id, timezone, ntp_enabled, ntp_primary, ntp_secondary, updated_at) "
        "VALUES (1, ?, ?, ?, ?, ?);");
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备写入 time_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto updated_at = settings.updated_at == 0 ? time_utils::system_now_ms() : settings.updated_at;
    if (!bind_text(statement.get(), 1, settings.timezone) ||
        !bind_int(statement.get(), 2, settings.ntp_enabled ? 1 : 0) ||
        !bind_text(statement.get(), 3, settings.ntp_primary) ||
        !bind_text(statement.get(), 4, settings.ntp_secondary) ||
        !bind_int64(statement.get(), 5, static_cast<sqlite3_int64>(updated_at))) {
        if (error_message != nullptr) *error_message = "绑定 time_settings 参数失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "写入 time_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
