// 网络配置持久化：只保存期望配置，真实链路状态由 Linux 运行时单独读取。
#include "datastore/config_store.h"
#include "datastore/config_store_internal.h"
#include <mutex>
#include <string>
#include <vector>
#include "common/sqlite_compat.h"
#include "common/time_utils.h"

namespace edge_controller {

using namespace config_store_internal;

namespace {

constexpr const char* kNetworkInitializedKey = "network_settings_initialized";
constexpr const char* kNetworkExplicitlyConfiguredKey = "network_settings_explicitly_configured";

StatusCode load_boolean_meta(
    sqlite3* database,
    const char* key,
    bool* found,
    bool* value,
    std::string* error_message)
{
    *found = false;
    *value = false;
    Statement statement(database, "SELECT value FROM config_meta WHERE key = ?;");
    if (!statement.ok() || !bind_text(statement.get(), 1, key)) {
        if (error_message != nullptr) {
            *error_message = "准备读取网络配置确认状态失败：" + sqlite_error(database);
        }
        return StatusCode::kIoError;
    }
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return StatusCode::kOk;
    if (step != SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = "读取网络配置确认状态失败：" + sqlite_error(database);
        }
        return StatusCode::kIoError;
    }
    const auto text = column_text(statement.get(), 0);
    if (text != "true" && text != "false") {
        if (error_message != nullptr) *error_message = std::string(key) + " 状态值无效";
        return StatusCode::kInvalidState;
    }
    *found = true;
    *value = text == "true";
    return StatusCode::kOk;
}

}  // namespace

// 从数据库加载网络设置。
StatusCode ConfigStore::load_network_settings(
    NetworkSettings* settings,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_network_settings_locked(settings, error_message);
}

// 读取并初始化网络设置。
StatusCode ConfigStore::load_or_initialize_network_settings(
    NetworkSettings* settings,
    std::string* error_message)
{
    bool explicitly_configured = false;
    return load_or_initialize_network_settings_state(settings, &explicitly_configured, error_message);
}

// 读取并初始化网络设置及显式确认状态。
StatusCode ConfigStore::load_or_initialize_network_settings_state(
    NetworkSettings* settings,
    bool* explicitly_configured,
    std::string* error_message)
{
    if (settings == nullptr || explicitly_configured == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少网络配置或确认状态输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto load_status = load_network_settings_locked(settings, error_message);
    if (is_ok(load_status)) {
        bool marker_found = false;
        bool marker_value = false;
        const auto marker_status = load_boolean_meta(
            database_, kNetworkExplicitlyConfiguredKey, &marker_found, &marker_value, error_message);
        if (!is_ok(marker_status)) return marker_status;
        if (marker_found) {
            *explicitly_configured = marker_value;
            return StatusCode::kOk;
        }

        // 兼容升级前数据库：只要网络行在本次初始化前已经存在，就不能把现场配置静默降级为未确认。
        // network_settings_initialized 可能来自旧版默认初始化，也可能伴随用户修改，因此仅作迁移证据，
        // 最终以“既有网络行”为保守判据并补写 true。
        bool initialized_found = false;
        bool initialized_value = false;
        const auto initialized_status = load_boolean_meta(
            database_, kNetworkInitializedKey, &initialized_found, &initialized_value, error_message);
        if (!is_ok(initialized_status)) return initialized_status;
        (void)initialized_found;
        (void)initialized_value;
        const auto migrate_status = set_meta_locked(kNetworkExplicitlyConfiguredKey, "true", error_message);
        if (!is_ok(migrate_status)) return migrate_status;
        *explicitly_configured = true;
        return StatusCode::kOk;
    }
    if (load_status != StatusCode::kNotFound) return load_status;
    if (error_message != nullptr) error_message->clear();

    auto initialized_settings = NetworkSettings{};
    normalize_network_settings(&initialized_settings);
    const auto validation_status = validate_network_settings(
        initialized_settings,
        "network_settings",
        error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (is_ok(status)) status = save_network_settings_locked(initialized_settings, error_message);
    if (is_ok(status)) status = set_meta_locked(kNetworkInitializedKey, "true", error_message);
    if (is_ok(status)) status = set_meta_locked(kNetworkExplicitlyConfiguredKey, "false", error_message);
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    if (!is_ok(status)) return status;

    *settings = std::move(initialized_settings);
    *explicitly_configured = false;
    return StatusCode::kOk;
}

// 保存网络设置。
StatusCode ConfigStore::save_network_settings(
    const NetworkSettings& settings,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_network_settings_locked(settings, error_message);
}

// 原子保存网络设置和显式确认状态。
StatusCode ConfigStore::save_network_settings_state(
    const NetworkSettings& settings,
    bool explicitly_configured,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (is_ok(status)) status = save_network_settings_locked(settings, error_message);
    if (is_ok(status)) {
        status = set_meta_locked(
            kNetworkExplicitlyConfiguredKey,
            explicitly_configured ? "true" : "false",
            error_message);
    }
    if (is_ok(status)) status = set_meta_locked(kNetworkInitializedKey, "true", error_message);
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    return status;
}

// 在持锁状态下加载网络设置。
StatusCode ConfigStore::load_network_settings_locked(
    NetworkSettings* settings,
    std::string* error_message) const
{
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少网络配置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "SELECT mode, interface_name, ip_address, netmask, gateway, dns_primary, dns_secondary "
        "FROM network_settings WHERE id = 1;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备读取 network_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto step_status = sqlite3_step(statement.get());
    if (step_status == SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "network_settings 尚未初始化";
        }
        return StatusCode::kNotFound;
    }
    if (step_status != SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = "读取 network_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    NetworkSettings loaded;
    loaded.mode = column_text(statement.get(), 0);
    loaded.interface_name = column_text(statement.get(), 1);
    loaded.ip_address = column_text(statement.get(), 2);
    loaded.netmask = column_text(statement.get(), 3);
    loaded.gateway = column_text(statement.get(), 4);
    loaded.dns_servers.clear();
    const auto dns_primary = column_text(statement.get(), 5);
    const auto dns_secondary = column_text(statement.get(), 6);
    if (!dns_primary.empty()) {
        loaded.dns_servers.push_back(dns_primary);
    }
    if (!dns_secondary.empty()) {
        loaded.dns_servers.push_back(dns_secondary);
    }
    normalize_network_settings(&loaded);
    const auto validation_status = validate_network_settings(loaded, "network_settings", error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    *settings = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存网络设置。
StatusCode ConfigStore::save_network_settings_locked(
    const NetworkSettings& settings,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    auto normalized = settings;
    normalize_network_settings(&normalized);
    const auto validation_status = validate_network_settings(normalized, "network_settings", error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    const char* sql =
        "INSERT OR REPLACE INTO network_settings "
        "(id, interface_name, mode, ip_address, netmask, gateway, dns_primary, dns_secondary, updated_at) "
        "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备写入 network_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto updated_at = time_utils::local_time_string();
    const std::string dns_primary = normalized.dns_servers.empty() ? std::string{} : normalized.dns_servers[0];
    const std::string dns_secondary = normalized.dns_servers.size() < 2 ? std::string{} : normalized.dns_servers[1];
    if (!bind_text(statement.get(), 1, normalized.interface_name) ||
        !bind_text(statement.get(), 2, normalized.mode) ||
        !bind_text(statement.get(), 3, normalized.ip_address) ||
        !bind_text(statement.get(), 4, normalized.netmask) ||
        !bind_text(statement.get(), 5, normalized.gateway) ||
        !bind_text(statement.get(), 6, dns_primary) ||
        !bind_text(statement.get(), 7, dns_secondary) ||
        !bind_text(statement.get(), 8, updated_at)) {
        if (error_message != nullptr) {
            *error_message = "绑定 network_settings 参数失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "写入 network_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
