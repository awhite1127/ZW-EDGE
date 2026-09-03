// 配置导入的单连接 SQLite 事务协调器。
#include "data/datastore/config_import_transaction.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "shared/common/sqlite_compat.h"
#include "data/datastore/alarm_store.h"
#include "data/datastore/config_store.h"
#include "data/datastore/config_store_internal.h"
#include "data/datastore/device_template_store.h"

namespace edge_controller {
namespace {

constexpr const char* kNetworkInitializedKey = "network_settings_initialized";
constexpr const char* kNetworkExplicitlyConfiguredKey =
    "network_settings_explicitly_configured";

using config_store_internal::Statement;
using config_store_internal::bind_int;
using config_store_internal::bind_int64;
using config_store_internal::bind_text;
using config_store_internal::sqlite_error;

// 仓储锁持有期间临时让仓储私有实现复用事务连接，析构时恢复其常规连接。
class ScopedBorrowedDatabase {
public:
    ScopedBorrowedDatabase(sqlite3*& slot, sqlite3* borrowed)
        : slot_(slot), original_(slot)
    {
        slot_ = borrowed;
    }

    ~ScopedBorrowedDatabase() { slot_ = original_; }

    ScopedBorrowedDatabase(const ScopedBorrowedDatabase&) = delete;
    ScopedBorrowedDatabase& operator=(const ScopedBorrowedDatabase&) = delete;

private:
    sqlite3*& slot_;
    sqlite3* original_{nullptr};
};

void prefix_error(std::string* error_message, const std::string& step)
{
    if (error_message == nullptr) return;
    if (error_message->empty()) {
        *error_message = step;
    } else {
        *error_message = step + "：" + *error_message;
    }
}

// Modbus 仓储的公开批量替换拥有独立事务；导入事务在共享连接上复用相同稳定列顺序。
StatusCode replace_modbus_mappings_in_transaction(
    sqlite3* database,
    const std::vector<ModbusRegisterMapping>& mappings,
    std::string* error_message)
{
    auto normalized = mappings;
    for (auto& mapping : normalized) normalize_modbus_register_mapping(&mapping);
    const auto validation = validate_modbus_register_mappings(normalized, error_message);
    if (!is_ok(validation)) return validation;

    {
        Statement delete_statement(database, "DELETE FROM modbus_register_mappings;");
        if (!delete_statement.ok() || sqlite3_step(delete_statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "清空 Modbus 寄存器映射失败：" + sqlite_error(database);
            }
            return StatusCode::kIoError;
        }
    }
    if (normalized.empty()) return StatusCode::kOk;

    Statement statement(
        database,
        "INSERT INTO modbus_register_mappings(mapping_id,device_id,point_key,"
        "device_name_snapshot,point_name_snapshot,start_address,data_type,value_multiplier,"
        "value_offset,byte_order,word_order,quality_address,enabled,created_at_ms,updated_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    if (!statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备写入 Modbus 寄存器映射失败：" + sqlite_error(database);
        }
        return StatusCode::kIoError;
    }
    for (const auto& mapping : normalized) {
        sqlite3_reset(statement.get());
        sqlite3_clear_bindings(statement.get());
        const bool bound =
            bind_text(statement.get(), 1, mapping.mapping_id) &&
            bind_text(statement.get(), 2, mapping.device_id) &&
            bind_text(statement.get(), 3, mapping.point_key) &&
            bind_text(statement.get(), 4, mapping.device_name_snapshot) &&
            bind_text(statement.get(), 5, mapping.point_name_snapshot) &&
            bind_int(statement.get(), 6, static_cast<int>(mapping.start_address)) &&
            bind_text(statement.get(), 7, to_string(mapping.data_type)) &&
            sqlite3_bind_double(statement.get(), 8, mapping.value_multiplier) == SQLITE_OK &&
            sqlite3_bind_double(statement.get(), 9, mapping.value_offset) == SQLITE_OK &&
            bind_text(statement.get(), 10, to_string(mapping.byte_order)) &&
            bind_text(statement.get(), 11, to_string(mapping.word_order)) &&
            bind_int(statement.get(), 12, static_cast<int>(mapping.quality_address)) &&
            bind_int(statement.get(), 13, mapping.enabled ? 1 : 0) &&
            bind_int64(
                statement.get(), 14, static_cast<sqlite3_int64>(mapping.created_at_ms)) &&
            bind_int64(
                statement.get(), 15, static_cast<sqlite3_int64>(mapping.updated_at_ms));
        if (!bound || sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入 Modbus 寄存器映射失败：" + sqlite_error(database);
            }
            return StatusCode::kIoError;
        }
    }
    return StatusCode::kOk;
}

}  // namespace

StatusCode ConfigImportTransaction::apply(
    ConfigStore& config_store,
    DeviceTemplateStore& device_template_store,
    AlarmStore& alarm_store,
    const ConfigImportPersistencePayload& payload,
    std::string* error_message)
{
    if (error_message != nullptr) error_message->clear();

    std::unique_lock<std::mutex> config_lock(config_store.mutex_, std::defer_lock);
    std::unique_lock<std::mutex> template_lock(device_template_store.mutex_, std::defer_lock);
    std::unique_lock<std::mutex> alarm_lock(alarm_store.mutex_, std::defer_lock);
    std::lock(config_lock, template_lock, alarm_lock);

    if (!config_store.database_ready_locked(error_message)) return StatusCode::kInvalidState;
    if (!device_template_store.database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    if (!alarm_store.available_locked(error_message)) return StatusCode::kInvalidState;
    if (config_store.database_path_.empty() ||
        config_store.database_path_ != device_template_store.database_path_ ||
        config_store.database_path_ != alarm_store.database_path_) {
        if (error_message != nullptr) {
            *error_message = "配置、设备模板和告警仓储未指向同一 edge-config.db";
        }
        return StatusCode::kInvalidState;
    }

    auto status = config_store.execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) {
        prefix_error(error_message, "开启配置导入事务失败");
        return status;
    }

    std::vector<DeviceTemplateDefinition> committed_templates;
    {
        ScopedBorrowedDatabase template_database(
            device_template_store.database_, config_store.database_);
        ScopedBorrowedDatabase alarm_database(alarm_store.database_, config_store.database_);

        status = config_store.save_system_settings_locked(payload.system_settings, error_message);
        if (!is_ok(status)) prefix_error(error_message, "写入系统设置失败");

        if (is_ok(status)) {
            status = config_store.save_time_settings_locked(payload.time_settings, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入时间设置失败");
        }
        if (is_ok(status)) {
            status = config_store.save_network_settings_locked(payload.network_settings, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入网络设置失败");
        }
        if (is_ok(status)) {
            status = config_store.set_meta_locked(
                kNetworkExplicitlyConfiguredKey,
                payload.network_settings_explicitly_configured ? "true" : "false",
                error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入网络确认状态失败");
        }
        if (is_ok(status)) {
            status = config_store.set_meta_locked(
                kNetworkInitializedKey, "true", error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入网络初始化状态失败");
        }
        if (is_ok(status)) {
            status = config_store.save_mqtt_settings_locked(payload.mqtt_settings, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入 MQTT 设置失败");
        }
        if (is_ok(status)) {
            status = device_template_store.replace_custom_templates_for_import_locked(
                payload.custom_device_types, &committed_templates, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入自定义设备类型失败");
        }
        if (is_ok(status)) {
            status = config_store.save_channels_locked(payload.channels, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入通道配置失败");
        }
        if (is_ok(status)) {
            status = config_store.save_masters_locked(payload.masters, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入主站配置失败");
        }
        if (is_ok(status) && payload.replace_alarm_data) {
            status = alarm_store.replace_alarm_data_for_import_locked(
                payload.alarm_rules, payload.alarm_runtime_states, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入告警配置失败");
        }
        if (is_ok(status)) {
            status = config_store.save_modbus_server_settings_locked(
                payload.modbus_server_settings, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入 Modbus Server 设置失败");
        }
        if (is_ok(status)) {
            status = replace_modbus_mappings_in_transaction(
                config_store.database_, payload.modbus_register_mappings, error_message);
            if (!is_ok(status)) prefix_error(error_message, "写入 Modbus 寄存器映射失败");
        }
    }

    if (is_ok(status)) {
        status = config_store.execute_sql_locked("COMMIT;", error_message);
        if (!is_ok(status)) prefix_error(error_message, "提交配置导入事务失败");
    }
    if (!is_ok(status)) {
        std::string rollback_error;
        const auto rollback_status =
            config_store.execute_sql_locked("ROLLBACK;", &rollback_error);
        if (!is_ok(rollback_status) && error_message != nullptr) {
            *error_message += "；回滚配置导入事务失败";
            if (!rollback_error.empty()) *error_message += "：" + rollback_error;
        }
        return status;
    }

    // 仅在 SQLite COMMIT 成功后发布模板注册表，避免后续步骤失败时内存先行。
    set_device_templates(std::move(committed_templates));
    return StatusCode::kOk;
}

}  // namespace edge_controller
