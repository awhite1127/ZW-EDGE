#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>

#include "common/filesystem_compat.h"
#include "common/sqlite_compat.h"
#include "datastore/alarm_store.h"
#include "datastore/config_import_transaction.h"
#include "datastore/config_store.h"
#include "datastore/database_paths.h"
#include "datastore/device_template_store.h"
#include "model/builtin_device_templates.h"
#include "model/device_template.h"

namespace {

using edge_controller::AlarmStore;
using edge_controller::ConfigImportPersistencePayload;
using edge_controller::ConfigImportTransaction;
using edge_controller::ConfigStore;
using edge_controller::DatabasePaths;
using edge_controller::DeviceTemplateStore;
using edge_controller::StatusCode;
using edge_controller::is_ok;

class TempDirectory {
public:
    TempDirectory()
    {
        std::error_code error;
        const auto base = edge::fs::temp_directory_path(error);
        if (error) return;
        for (int suffix = 0; suffix < 100; ++suffix) {
            const auto candidate = base /
                ("edge-controller-config-import-test-" + std::to_string(suffix));
            error.clear();
            if (edge::fs::create_directory(candidate, error)) {
                path_ = candidate.string();
                return;
            }
        }
    }

    ~TempDirectory()
    {
        if (path_.empty()) return;
        std::error_code ignored;
        edge::fs::remove_all(path_, ignored);
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

class SqliteConnection {
public:
    ~SqliteConnection()
    {
        if (database_ != nullptr) sqlite3_close(database_);
    }

    bool open(const std::string& path, std::string* error)
    {
        const auto status = sqlite3_open_v2(
            path.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (status == SQLITE_OK) {
            sqlite3_busy_timeout(database_, 5000);
            return true;
        }
        if (error != nullptr) {
            *error = database_ == nullptr ? "sqlite open failed" : sqlite3_errmsg(database_);
        }
        return false;
    }

    bool execute(const std::string& sql, std::string* error)
    {
        char* raw_error = nullptr;
        const auto status = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &raw_error);
        if (status == SQLITE_OK) return true;
        if (error != nullptr) {
            *error = raw_error == nullptr ? sqlite3_errmsg(database_) : raw_error;
        }
        sqlite3_free(raw_error);
        return false;
    }

    bool scalar_text(const std::string& sql, std::string* value, std::string* error)
    {
        if (value == nullptr) return false;
        sqlite3_stmt* raw_statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &raw_statement, nullptr) != SQLITE_OK) {
            if (error != nullptr) *error = sqlite3_errmsg(database_);
            return false;
        }
        const auto finalize = [&]() { sqlite3_finalize(raw_statement); };
        if (sqlite3_step(raw_statement) != SQLITE_ROW) {
            if (error != nullptr) *error = sqlite3_errmsg(database_);
            finalize();
            return false;
        }
        const auto* text = sqlite3_column_text(raw_statement, 0);
        *value = text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
        finalize();
        return true;
    }

    bool scalar_int64(const std::string& sql, std::int64_t* value, std::string* error)
    {
        if (value == nullptr) return false;
        sqlite3_stmt* raw_statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &raw_statement, nullptr) != SQLITE_OK) {
            if (error != nullptr) *error = sqlite3_errmsg(database_);
            return false;
        }
        if (sqlite3_step(raw_statement) != SQLITE_ROW) {
            if (error != nullptr) *error = sqlite3_errmsg(database_);
            sqlite3_finalize(raw_statement);
            return false;
        }
        *value = sqlite3_column_int64(raw_statement, 0);
        sqlite3_finalize(raw_statement);
        return true;
    }

private:
    sqlite3* database_{nullptr};
};

int fail(const std::string& message)
{
    std::cerr << "config import atomicity test failed: " << message << '\n';
    return 1;
}

}  // namespace

int main()
{
    TempDirectory data_directory;
    if (data_directory.path().empty()) return fail("could not create temporary directory");

    std::string error;
    const DatabasePaths paths(data_directory.path());
    ConfigStore config_store;
    auto status = config_store.initialize(paths.config_database(), &error);
    if (!is_ok(status)) return fail("could not initialize ConfigStore: " + error);

    edge_controller::SystemSettings baseline_system;
    status = config_store.load_or_initialize_system_settings(&baseline_system, &error);
    if (!is_ok(status)) return fail("could not initialize system settings: " + error);
    edge_controller::TimeSettings baseline_time;
    status = config_store.load_or_initialize_time_settings(&baseline_time, &error);
    if (!is_ok(status)) return fail("could not initialize time settings: " + error);
    edge_controller::NetworkSettings baseline_network;
    bool baseline_network_explicit = false;
    status = config_store.load_or_initialize_network_settings_state(
        &baseline_network, &baseline_network_explicit, &error);
    if (!is_ok(status)) return fail("could not initialize network settings: " + error);
    edge_controller::MqttSettings baseline_mqtt;
    status = config_store.load_or_initialize_mqtt_settings(&baseline_mqtt, &error);
    if (!is_ok(status)) return fail("could not initialize MQTT settings: " + error);
    edge_controller::ModbusServerSettings baseline_modbus;
    status = config_store.load_or_initialize_modbus_server_settings(&baseline_modbus, &error);
    if (!is_ok(status)) return fail("could not initialize Modbus settings: " + error);

    DeviceTemplateStore template_store;
    status = template_store.initialize(paths.config_database(), &error);
    if (!is_ok(status)) return fail("could not initialize DeviceTemplateStore: " + error);
    AlarmStore alarm_store;
    status = alarm_store.initialize(paths.config_database(), &error);
    if (!is_ok(status)) return fail("could not initialize AlarmStore: " + error);

    ConfigImportPersistencePayload payload;
    payload.system_settings = baseline_system;
    payload.system_settings.display_name = "Atomic Import Candidate";
    payload.time_settings = baseline_time;
    payload.network_settings = baseline_network;
    payload.network_settings_explicitly_configured = true;
    payload.mqtt_settings = baseline_mqtt;
    payload.replace_alarm_data = true;
    payload.modbus_server_settings = baseline_modbus;

    const auto builtins = edge_controller::builtin_device_templates();
    if (builtins.empty()) return fail("no builtin template available for test payload");
    auto custom_template = builtins.front();
    custom_template.template_id = "atomic_import_custom";
    custom_template.display_name = "Atomic Import Custom";
    custom_template.builtin = false;
    custom_template.write_commands.clear();
    payload.custom_device_types.push_back(std::move(custom_template));

    edge_controller::AlarmRule rule;
    rule.device_id = "atomic-device";
    rule.point_key = "atomic-point";
    rule.updated_at_ms = 1;
    payload.alarm_rules = {rule};

    const auto old_port = baseline_modbus.listen_port;
    const auto rejected_port = static_cast<std::uint16_t>(
        old_port == 65535 ? old_port - 1 : old_port + 1);
    payload.modbus_server_settings.listen_port = rejected_port;

    SqliteConnection injection_database;
    if (!injection_database.open(paths.config_database(), &error)) return fail(error);

    // 第一个触发器在事务后段制造写失败；第二个只会阻止旧实现的逐项补偿。
    // SQLite 自身 ROLLBACK 不执行 INSERT，因此新的单事务实现不依赖第二次业务写入。
    const std::string triggers =
        "CREATE TRIGGER reject_import_modbus BEFORE INSERT ON modbus_server_settings "
        "WHEN NEW.listen_port=" + std::to_string(rejected_port) +
        " BEGIN SELECT RAISE(ABORT,'injected late import failure'); END;"
        "CREATE TRIGGER reject_compensation_system BEFORE INSERT ON system_settings "
        "WHEN NEW.display_name<>'Atomic Import Candidate' "
        "BEGIN SELECT RAISE(ABORT,'compensation must not be required'); END;";
    if (!injection_database.execute(triggers, &error)) return fail(error);

    error.clear();
    const auto import_status = ConfigImportTransaction::apply(
        config_store, template_store, alarm_store, payload, &error);
    if (is_ok(import_status)) return fail("injected persistence failure was unexpectedly accepted");
    if (error.find("injected late import failure") == std::string::npos) {
        return fail("injected transaction failure was not reported: " + error);
    }

    std::string persisted_display_name;
    if (!injection_database.scalar_text(
            "SELECT display_name FROM system_settings WHERE id=1;",
            &persisted_display_name,
            &error)) {
        return fail(error);
    }
    if (persisted_display_name != baseline_system.display_name) {
        return fail("system_settings retained a partial imported value");
    }

    std::int64_t persisted_custom_templates = -1;
    if (!injection_database.scalar_int64(
            "SELECT COUNT(*) FROM device_templates "
            "WHERE template_id='atomic_import_custom';",
            &persisted_custom_templates,
            &error)) {
        return fail(error);
    }
    if (persisted_custom_templates != 0) {
        return fail("device template repository retained a partial imported value");
    }

    std::int64_t persisted_alarm_rules = -1;
    if (!injection_database.scalar_int64(
            "SELECT COUNT(*) FROM alarm_rules "
            "WHERE device_id='atomic-device' AND point_key='atomic-point';",
            &persisted_alarm_rules,
            &error)) {
        return fail(error);
    }
    if (persisted_alarm_rules != 0) {
        return fail("alarm repository retained a partial imported value");
    }

    std::int64_t persisted_modbus_port = 0;
    if (!injection_database.scalar_int64(
            "SELECT listen_port FROM modbus_server_settings WHERE id=1;",
            &persisted_modbus_port,
            &error)) {
        return fail(error);
    }
    if (persisted_modbus_port != old_port) {
        return fail("Modbus settings retained a partial imported value");
    }
    if (edge_controller::find_device_template("atomic_import_custom") != nullptr) {
        return fail("template registry was published before transaction commit");
    }

    std::cout << "config import atomicity test passed\n";
    return 0;
}
