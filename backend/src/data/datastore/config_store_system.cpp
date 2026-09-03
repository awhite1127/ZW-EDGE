// 系统标识配置持久化；字段保持精简，避免与采集模型或硬件适配耦合。
#include "data/datastore/config_store.h"
#include "data/datastore/config_store_internal.h"
#include <mutex>
#include <string>
#include "shared/common/sqlite_compat.h"
#include "shared/common/time_utils.h"

namespace edge_controller {

using namespace config_store_internal;

// 读取系统显示设置。
StatusCode ConfigStore::load_system_settings(
    SystemSettings* settings,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_system_settings_locked(settings, error_message);
}

// 读取并初始化系统设置。
StatusCode ConfigStore::load_or_initialize_system_settings(
    SystemSettings* settings,
    std::string* error_message)
{
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少系统设置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto load_status = load_system_settings_locked(settings, error_message);
        if (is_ok(load_status)) {
            return StatusCode::kOk;
        }
        if (load_status != StatusCode::kNotFound) {
            return load_status;
        }
        if (error_message != nullptr) {
            error_message->clear();
        }
    }

    auto initialized_settings = SystemSettings{};
    normalize_system_settings(&initialized_settings);
    const auto validation_status = validate_system_settings(
        initialized_settings,
        "system_settings",
        error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto save_status = save_system_settings_locked(initialized_settings, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }
    const auto meta_status = set_meta_locked(
        "system_settings_initialized",
        "true",
        error_message);
    if (!is_ok(meta_status)) {
        return meta_status;
    }

    *settings = std::move(initialized_settings);
    return StatusCode::kOk;
}

// 保存系统显示设置。
StatusCode ConfigStore::save_system_settings(
    const SystemSettings& settings,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_system_settings_locked(settings, error_message);
}

// 在持锁状态下加载系统设置。
StatusCode ConfigStore::load_system_settings_locked(
    SystemSettings* settings,
    std::string* error_message) const
{
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少系统设置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "SELECT device_name, site_location, display_name "
        "FROM system_settings WHERE id = 1;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备读取 system_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto step_status = sqlite3_step(statement.get());
    if (step_status == SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "system_settings 尚未初始化";
        }
        return StatusCode::kNotFound;
    }
    if (step_status != SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = "读取 system_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    SystemSettings loaded;
    loaded.device_name = column_text(statement.get(), 0);
    loaded.site_location = column_text(statement.get(), 1);
    loaded.display_name = column_text(statement.get(), 2);
    normalize_system_settings(&loaded);
    const auto validation_status = validate_system_settings(loaded, "system_settings", error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    *settings = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存系统设置。
StatusCode ConfigStore::save_system_settings_locked(
    const SystemSettings& settings,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    const auto validation_status = validate_system_settings(settings, "system_settings", error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    const char* sql =
        "INSERT OR REPLACE INTO system_settings "
        "(id, device_name, site_location, display_name, updated_at) "
        "VALUES (1, ?, ?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备写入 system_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto updated_at = time_utils::local_time_string();
    if (!bind_text(statement.get(), 1, settings.device_name) ||
        !bind_text(statement.get(), 2, settings.site_location) ||
        !bind_text(statement.get(), 3, settings.display_name) ||
        !bind_text(statement.get(), 4, updated_at)) {
        if (error_message != nullptr) {
            *error_message = "绑定 system_settings 参数失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "写入 system_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
