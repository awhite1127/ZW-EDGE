// MQTT 配置持久化：密码采用只写语义，查询模型不得回显已有明文。
#include "datastore/config_store.h"
#include "datastore/config_store_internal.h"
#include <mutex>
#include <string>
#include "common/sqlite_compat.h"
#include "common/time_utils.h"

namespace edge_controller {

using namespace config_store_internal;

// 从数据库加载 MQTT 设置。
StatusCode ConfigStore::load_mqtt_settings(
    MqttSettings* settings,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_mqtt_settings_locked(settings, error_message);
}

// 读取并初始化MQTT设置。
StatusCode ConfigStore::load_or_initialize_mqtt_settings(
    MqttSettings* settings,
    std::string* error_message)
{
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 MQTT 配置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto load_status = load_mqtt_settings_locked(settings, error_message);
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

    auto initialized_settings = MqttSettings{};
    normalize_mqtt_settings(&initialized_settings);
    const auto validation_status = validate_mqtt_settings(
        initialized_settings,
        "mqtt_settings",
        error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto save_status = save_mqtt_settings_locked(initialized_settings, error_message);
    if (!is_ok(save_status)) {
        return save_status;
    }
    const auto meta_status = set_meta_locked(
        "mqtt_settings_initialized",
        "true",
        error_message);
    if (!is_ok(meta_status)) {
        return meta_status;
    }

    *settings = std::move(initialized_settings);
    return StatusCode::kOk;
}

// 保存MQTT设置。
StatusCode ConfigStore::save_mqtt_settings(
    const MqttSettings& settings,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_mqtt_settings_locked(settings, error_message);
}

// 在持锁状态下加载MQTT设置。
StatusCode ConfigStore::load_mqtt_settings_locked(
    MqttSettings* settings,
    std::string* error_message) const
{
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 MQTT 配置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const char* sql =
        "SELECT enabled, broker_host, broker_port, client_id, node_id, username, password, "
        "topic_prefix, publish_interval_seconds, qos, retain_status, keep_alive_seconds, "
        "tls_enabled, tls_ca_file, tls_client_cert_file, tls_client_key_file, tls_insecure "
        "FROM mqtt_settings WHERE id = 1;";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备读取 mqtt_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto step_status = sqlite3_step(statement.get());
    if (step_status == SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "mqtt_settings 尚未初始化";
        }
        return StatusCode::kNotFound;
    }
    if (step_status != SQLITE_ROW) {
        if (error_message != nullptr) {
            *error_message = "读取 mqtt_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    MqttSettings loaded;
    const auto broker_port = sqlite3_column_int(statement.get(), 2);
    loaded.enabled = sqlite3_column_int(statement.get(), 0) != 0;
    loaded.broker_host = column_text(statement.get(), 1);
    loaded.broker_port = broker_port < 0 || broker_port > 65535 ? 0 : static_cast<std::uint16_t>(broker_port);
    loaded.client_id = column_text(statement.get(), 3);
    loaded.node_id = column_text(statement.get(), 4);
    loaded.username = column_text(statement.get(), 5);
    loaded.password = column_text(statement.get(), 6);
    loaded.topic_prefix = column_text(statement.get(), 7);
    loaded.publish_interval_seconds = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 8));
    loaded.qos = sqlite3_column_int(statement.get(), 9);
    loaded.retain_status = sqlite3_column_int(statement.get(), 10) != 0;
    loaded.keep_alive_seconds = static_cast<std::uint32_t>(sqlite3_column_int(statement.get(), 11));
    loaded.tls_enabled = sqlite3_column_int(statement.get(), 12) != 0;
    loaded.tls_ca_file = column_text(statement.get(), 13);
    loaded.tls_client_cert_file = column_text(statement.get(), 14);
    loaded.tls_client_key_file = column_text(statement.get(), 15);
    loaded.tls_insecure = sqlite3_column_int(statement.get(), 16) != 0;

    normalize_mqtt_settings(&loaded);
    const auto validation_status = validate_mqtt_settings(loaded, "mqtt_settings", error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    *settings = std::move(loaded);
    return StatusCode::kOk;
}

// 在持锁状态下保存MQTT设置。
StatusCode ConfigStore::save_mqtt_settings_locked(
    const MqttSettings& settings,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    auto normalized = settings;
    normalize_mqtt_settings(&normalized);
    const auto validation_status = validate_mqtt_settings(normalized, "mqtt_settings", error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }

    const char* sql =
        "INSERT OR REPLACE INTO mqtt_settings "
        "(id, enabled, broker_host, broker_port, client_id, node_id, username, password, topic_prefix, "
        "publish_interval_seconds, qos, retain_status, keep_alive_seconds, tls_enabled, tls_ca_file, "
        "tls_client_cert_file, tls_client_key_file, tls_insecure, updated_at) "
        "VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    Statement statement(database_, sql);
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备写入 mqtt_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto updated_at = time_utils::local_time_string();
    if (!bind_int(statement.get(), 1, normalized.enabled ? 1 : 0) ||
        !bind_text(statement.get(), 2, normalized.broker_host) ||
        !bind_int(statement.get(), 3, static_cast<int>(normalized.broker_port)) ||
        !bind_text(statement.get(), 4, normalized.client_id) ||
        !bind_text(statement.get(), 5, normalized.node_id) ||
        !bind_text(statement.get(), 6, normalized.username) ||
        !bind_text(statement.get(), 7, normalized.password) ||
        !bind_text(statement.get(), 8, normalized.topic_prefix) ||
        !bind_int(statement.get(), 9, static_cast<int>(normalized.publish_interval_seconds)) ||
        !bind_int(statement.get(), 10, normalized.qos) ||
        !bind_int(statement.get(), 11, normalized.retain_status ? 1 : 0) ||
        !bind_int(statement.get(), 12, static_cast<int>(normalized.keep_alive_seconds)) ||
        !bind_int(statement.get(), 13, normalized.tls_enabled ? 1 : 0) ||
        !bind_text(statement.get(), 14, normalized.tls_ca_file) ||
        !bind_text(statement.get(), 15, normalized.tls_client_cert_file) ||
        !bind_text(statement.get(), 16, normalized.tls_client_key_file) ||
        !bind_int(statement.get(), 17, normalized.tls_insecure ? 1 : 0) ||
        !bind_text(statement.get(), 18, updated_at)) {
        if (error_message != nullptr) {
            *error_message = "绑定 mqtt_settings 参数失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "写入 mqtt_settings 失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

}  // namespace edge_controller
