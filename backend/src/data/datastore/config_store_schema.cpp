// 配置库只创建并校验当前正式结构；非当前结构只拒绝打开并指引保全迁移。
#include "data/datastore/config_store.h"
#include "data/datastore/config_store_internal.h"
#include <string>
#include "shared/common/sqlite_compat.h"

namespace edge_controller {

using namespace config_store_internal;

namespace {

constexpr int kCurrentDatabaseVersion = 9;

// 判断数据库表是否包含指定列。
StatusCode table_column_exists(
    sqlite3* database,
    const std::string& table_name,
    const std::string& column_name,
    bool* exists,
    std::string* error_message)
{
    if (exists == nullptr) {
        if (error_message != nullptr) *error_message = "检查数据库列时缺少输出参数";
        return StatusCode::kInvalidArgument;
    }
    *exists = false;
    const auto sql = "PRAGMA table_info(" + table_name + ");";
    Statement statement(database, sql.c_str());
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "读取 " + table_name + " 表结构失败：" + sqlite_error(database);
        return StatusCode::kIoError;
    }
    int step_status = SQLITE_ROW;
    while ((step_status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (column_text(statement.get(), 1) == column_name) {
            *exists = true;
            return StatusCode::kOk;
        }
    }
    if (step_status != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "读取 " + table_name + " 表结构失败：" + sqlite_error(database);
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}


// 仅接受全新数据库或当前正式结构；其他结构必须留存原库并走经验证的迁移路径。
StatusCode validate_clean_database_baseline(sqlite3* database, std::string* error_message)
{
    Statement version_statement(database, "PRAGMA user_version;");
    if (!version_statement.ok() || sqlite3_step(version_statement.get()) != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "读取 SQLite user_version 失败：" + sqlite_error(database);
        return StatusCode::kIoError;
    }
    const auto version = sqlite3_column_int(version_statement.get(), 0);
    if (version == kCurrentDatabaseVersion) {
        return StatusCode::kOk;
    }
    if (version == 0) {
        Statement tables_statement(
            database,
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%';");
        if (!tables_statement.ok() || sqlite3_step(tables_statement.get()) != SQLITE_ROW) {
            if (error_message != nullptr) {
                *error_message = "检查全新数据库基线失败：" + sqlite_error(database);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_column_int(tables_statement.get(), 0) == 0) {
            return StatusCode::kOk;
        }
    }
    if (error_message != nullptr) {
        *error_message = schema_migration_required_message(
            "edge-config.db 不是当前正式结构；本版本不提供该结构的直接迁移");
    }
    return StatusCode::kInvalidState;
}

}  // namespace

// 在持锁状态下初始化数据库结构。
StatusCode ConfigStore::initialize_schema_locked(std::string* error_message)
{
    const auto baseline_status = validate_clean_database_baseline(database_, error_message);
    if (!is_ok(baseline_status)) {
        return baseline_status;
    }
    // 当前版本数据库不再由其他 Store 补表；ConfigStore 是配置库唯一 schema owner。
    {
        Statement version_statement(database_, "PRAGMA user_version;");
        if (!version_statement.ok() || sqlite3_step(version_statement.get()) != SQLITE_ROW) {
            if (error_message != nullptr) {
                *error_message = "读取 edge-config.db user_version 失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        const auto database_version = sqlite3_column_int(version_statement.get(), 0);
        if (database_version == kCurrentDatabaseVersion) {
            Statement tables_statement(
                database_,
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ("
                "'config_meta','system_settings','time_settings','network_settings','mqtt_settings',"
                "'web_users','channels','masters','device_templates','device_template_fields',"
                "'device_template_read_blocks','device_template_enum_items',"
                "'alarm_rules','alarm_runtime_states','device_aliases','modbus_server_settings',"
                "'modbus_register_mappings');");
            if (!tables_statement.ok() || sqlite3_step(tables_statement.get()) != SQLITE_ROW) {
                if (error_message != nullptr) {
                    *error_message = "检查 edge-config.db 当前结构失败：" + sqlite_error(database_);
                }
                return StatusCode::kIoError;
            }
            if (sqlite3_column_int(tables_statement.get(), 0) != 17) {
                if (error_message != nullptr) {
                    *error_message = schema_migration_required_message(
                        "edge-config.db 缺少当前正式数据表，判定为异常数据库");
                }
                return StatusCode::kInvalidState;
            }
            bool has_read_block_key = false;
            bool has_device_address_stride = false;
            bool has_device_count = false;
            bool has_bit_index = false;
            auto column_status = table_column_exists(
                database_, "device_template_fields", "read_block_key", &has_read_block_key, error_message);
            if (is_ok(column_status)) {
                column_status = table_column_exists(
                    database_, "device_templates", "device_address_stride",
                    &has_device_address_stride, error_message);
            }
            if (is_ok(column_status)) {
                column_status = table_column_exists(
                    database_, "masters", "device_count", &has_device_count, error_message);
            }
            if (is_ok(column_status)) {
                column_status = table_column_exists(
                    database_, "device_template_fields", "bit_index", &has_bit_index, error_message);
            }
            if (!is_ok(column_status)) return column_status;
            if (!has_read_block_key || !has_device_address_stride || !has_device_count || !has_bit_index) {
                if (error_message != nullptr) {
                    *error_message = schema_migration_required_message(
                        "edge-config.db 缺少读取区块、地址跨度、设备数量或单比特字段列");
                }
                return StatusCode::kInvalidState;
            }
            return StatusCode::kOk;
        }
    }

    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) {
        return status;
    }

    const char* schema_sql =
        "CREATE TABLE IF NOT EXISTS config_meta ("
        "key TEXT PRIMARY KEY,"
        "value TEXT NOT NULL,"
        "updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS system_settings ("
        "id INTEGER PRIMARY KEY CHECK (id = 1),"
        "device_name TEXT,"
        "site_location TEXT,"
        "display_name TEXT,"
        "updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS time_settings ("
        "id INTEGER PRIMARY KEY CHECK (id = 1),"
        "timezone TEXT NOT NULL,"
        "ntp_enabled INTEGER NOT NULL DEFAULT 0,"
        "ntp_primary TEXT,"
        "ntp_secondary TEXT,"
        "updated_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS network_settings ("
        "id INTEGER PRIMARY KEY CHECK (id = 1),"
        "interface_name TEXT NOT NULL,"
        "mode TEXT NOT NULL DEFAULT 'static',"
        "ip_address TEXT NOT NULL,"
        "netmask TEXT NOT NULL,"
        "gateway TEXT NOT NULL,"
        "dns_primary TEXT,"
        "dns_secondary TEXT,"
        "updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS mqtt_settings ("
        "id INTEGER PRIMARY KEY CHECK (id = 1),"
        "enabled INTEGER NOT NULL DEFAULT 0,"
        "broker_host TEXT,"
        "broker_port INTEGER NOT NULL DEFAULT 1883,"
        "client_id TEXT,"
        "node_id TEXT NOT NULL,"
        "username TEXT,"
        "password TEXT,"
        "topic_prefix TEXT NOT NULL,"
        "publish_interval_seconds INTEGER NOT NULL DEFAULT 10,"
        "qos INTEGER NOT NULL DEFAULT 0,"
        "retain_status INTEGER NOT NULL DEFAULT 1,"
        "keep_alive_seconds INTEGER NOT NULL DEFAULT 60,"
        "tls_enabled INTEGER NOT NULL DEFAULT 0,"
        "tls_ca_file TEXT,"
        "tls_client_cert_file TEXT,"
        "tls_client_key_file TEXT,"
        "tls_insecure INTEGER NOT NULL DEFAULT 0,"
        "updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS web_users ("
        "id INTEGER PRIMARY KEY,"
        "username TEXT NOT NULL UNIQUE,"
        "display_name TEXT NOT NULL DEFAULT '',"
        "role TEXT NOT NULL,"
        "password_hash TEXT NOT NULL,"
        "password_salt TEXT NOT NULL,"
        "password_iterations INTEGER NOT NULL,"
        "enabled INTEGER NOT NULL DEFAULT 1,"
        "password_change_recommended INTEGER NOT NULL DEFAULT 1,"
        "created_at_ms INTEGER NOT NULL,"
        "updated_at_ms INTEGER NOT NULL,"
        "last_password_change_ms INTEGER NOT NULL DEFAULT 0,"
        "last_login_at_ms INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS channels ("
        "id TEXT PRIMARY KEY,"
        "name TEXT NOT NULL,"
        "type TEXT NOT NULL,"
        "enabled INTEGER NOT NULL,"
        "device_path TEXT,"
        "port_name TEXT,"
        "tcp_host TEXT,"
        "tcp_port INTEGER NOT NULL DEFAULT 502,"
        "connect_timeout_ms INTEGER NOT NULL DEFAULT 3000,"
        "baud_rate INTEGER NOT NULL,"
        "data_bits INTEGER NOT NULL,"
        "stop_bits INTEGER NOT NULL,"
        "parity TEXT NOT NULL,"
        "timeout_ms INTEGER NOT NULL,"
        "retry_count INTEGER NOT NULL,"
        "display_order INTEGER NOT NULL DEFAULT 0,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS masters ("
        "id TEXT PRIMARY KEY,"
        "channel_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "enabled INTEGER NOT NULL,"
        "protocol TEXT NOT NULL,"
        "slave_id INTEGER NOT NULL,"
        "template_id TEXT NOT NULL,"
        "start_register INTEGER NOT NULL,"
        "device_count INTEGER NOT NULL CHECK(device_count>0 AND device_count<=256),"
        "poll_interval_ms INTEGER NOT NULL,"
        "timeout_ms INTEGER NOT NULL,"
        "retry_count INTEGER NOT NULL,"
        "remark TEXT,"
        "display_order INTEGER NOT NULL DEFAULT 0,"
        "created_at TEXT NOT NULL,"
        "updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS device_templates ("
        "template_id TEXT PRIMARY KEY,"
        "template_name TEXT NOT NULL,"
        "description TEXT,"
        "default_start_register INTEGER NOT NULL DEFAULT 0,"
        "device_address_stride INTEGER NOT NULL CHECK(device_address_stride>0),"
        "builtin INTEGER NOT NULL DEFAULT 0,"
        "created_at TEXT,"
        "updated_at TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS device_template_read_blocks ("
        "template_id TEXT NOT NULL,"
        "block_key TEXT NOT NULL CHECK(length(block_key)>0),"
        "display_name TEXT NOT NULL CHECK(length(display_name)>0),"
        "function_code INTEGER NOT NULL CHECK(function_code IN(3,4)),"
        "start_offset INTEGER NOT NULL CHECK(start_offset>=0 AND start_offset<=65535),"
        "register_count INTEGER NOT NULL CHECK(register_count>=1 AND register_count<=125),"
        "sort_order INTEGER NOT NULL DEFAULT 0,"
        "CHECK(start_offset+register_count<=65536),"
        "PRIMARY KEY(template_id,block_key),"
        "FOREIGN KEY(template_id) REFERENCES device_templates(template_id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_device_template_read_blocks_order "
        "ON device_template_read_blocks(template_id,sort_order,block_key);"
        "CREATE TABLE IF NOT EXISTS device_template_fields ("
        "template_id TEXT NOT NULL,"
        "field_key TEXT NOT NULL,"
        "field_name TEXT NOT NULL,"
        "unit TEXT,"
        "data_type TEXT NOT NULL,"
        "parser_id TEXT NOT NULL DEFAULT 'scaled_uint16',"
        "read_block_key TEXT NOT NULL CHECK(length(read_block_key)>0),"
        "register_offset INTEGER NOT NULL,"
        "register_count INTEGER NOT NULL,"
        "scale REAL NOT NULL,"
        "value_offset REAL NOT NULL,"
        "precision INTEGER NOT NULL,"
        "summary INTEGER NOT NULL DEFAULT 0,"
        "display_order INTEGER NOT NULL,"
        "invalid_rule_type TEXT NOT NULL DEFAULT 'none' "
        "CHECK(invalid_rule_type IN('none','equal','greater_or_equal','less_or_equal','inside_range')),"
        "invalid_rule_value REAL NOT NULL DEFAULT 0,"
        "invalid_rule_min REAL NOT NULL DEFAULT 0,"
        "invalid_rule_max REAL NOT NULL DEFAULT 0,"
        "byte_order TEXT NOT NULL DEFAULT 'big_endian' CHECK(byte_order IN('big_endian','little_endian')),"
        "word_order TEXT NOT NULL DEFAULT 'high_word_first' CHECK(word_order IN('high_word_first','low_word_first'))," 
        "bit_index INTEGER NOT NULL DEFAULT -1 CHECK(bit_index>=-1 AND bit_index<=15),"
        "CHECK(invalid_rule_type<>'none' OR (invalid_rule_value=0 AND invalid_rule_min=0 AND invalid_rule_max=0)),"
        "CHECK(invalid_rule_type NOT IN('equal','greater_or_equal','less_or_equal') OR "
        "(invalid_rule_min=0 AND invalid_rule_max=0)),"
        "CHECK(invalid_rule_type<>'inside_range' OR invalid_rule_value=0),"
        "CHECK(invalid_rule_type<>'inside_range' OR invalid_rule_min<=invalid_rule_max),"
        "PRIMARY KEY(template_id, field_key),"
        "FOREIGN KEY(template_id) REFERENCES device_templates(template_id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_device_template_fields_template_id ON device_template_fields(template_id);"
        "CREATE TABLE IF NOT EXISTS device_template_enum_items("
        "template_id TEXT NOT NULL,"
        "field_key TEXT NOT NULL,"
        "value INTEGER NOT NULL,"
        "label TEXT NOT NULL CHECK(length(trim(label))>0),"
        "sort_order INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(template_id,field_key,value),"
        "FOREIGN KEY(template_id,field_key) REFERENCES device_template_fields(template_id,field_key) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_device_template_enum_items_order "
        "ON device_template_enum_items(template_id,field_key,sort_order,value);"
        "CREATE TABLE IF NOT EXISTS alarm_rules("
        "device_id TEXT NOT NULL,point_key TEXT NOT NULL,enabled INTEGER NOT NULL,"
        "high_enabled INTEGER NOT NULL,high_threshold REAL NOT NULL,low_enabled INTEGER NOT NULL,"
        "low_threshold REAL NOT NULL,level TEXT NOT NULL,hysteresis REAL NOT NULL,"
        "trigger_count INTEGER NOT NULL,recovery_count INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY(device_id,point_key)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_alarm_rules_device ON alarm_rules(device_id);"
        "CREATE TABLE IF NOT EXISTS alarm_runtime_states("
        "device_id TEXT NOT NULL,point_key TEXT NOT NULL,state TEXT NOT NULL,direction TEXT NOT NULL,"
        "current_value REAL NOT NULL,threshold_value REAL NOT NULL,consecutive_trigger_count INTEGER NOT NULL,"
        "consecutive_recovery_count INTEGER NOT NULL,active_since_ms INTEGER NOT NULL,"
        "last_evaluated_at_ms INTEGER NOT NULL,acknowledged INTEGER NOT NULL DEFAULT 0,"
        "acknowledged_at_ms INTEGER NOT NULL DEFAULT 0,acknowledged_by TEXT NOT NULL DEFAULT '',"
        "updated_at_ms INTEGER NOT NULL,"
        "PRIMARY KEY(device_id,point_key)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_alarm_states_state ON alarm_runtime_states(state);"
        "CREATE INDEX IF NOT EXISTS idx_alarm_states_active_since ON alarm_runtime_states(active_since_ms);"
        "CREATE TABLE IF NOT EXISTS device_aliases("
        "device_id TEXT PRIMARY KEY,display_name TEXT NOT NULL,updated_at_ms INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS modbus_server_settings("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN(0,1)),"
        "listen_address TEXT NOT NULL DEFAULT '0.0.0.0',"
        "listen_port INTEGER NOT NULL DEFAULT 1502 CHECK(listen_port>=1 AND listen_port<=65535),"
        "unit_id INTEGER NOT NULL DEFAULT 1 CHECK(unit_id>=0 AND unit_id<=255),"
        "strict_unit_id INTEGER NOT NULL DEFAULT 0 CHECK(strict_unit_id IN(0,1)),"
        "max_clients INTEGER NOT NULL DEFAULT 8,"
        "idle_timeout_seconds INTEGER NOT NULL DEFAULT 60,"
        "max_read_registers INTEGER NOT NULL DEFAULT 125,"
        "updated_at_ms INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS modbus_register_mappings("
        "mapping_id TEXT PRIMARY KEY,"
        "device_id TEXT NOT NULL,point_key TEXT NOT NULL,"
        "device_name_snapshot TEXT NOT NULL DEFAULT '',point_name_snapshot TEXT NOT NULL DEFAULT '',"
        "start_address INTEGER NOT NULL CHECK(start_address>=0 AND start_address<=65535),"
        "data_type TEXT NOT NULL CHECK(data_type IN('uint16','int16','uint32','int32','float32')),"
        "value_multiplier REAL NOT NULL DEFAULT 1.0,value_offset REAL NOT NULL DEFAULT 0.0,"
        "byte_order TEXT NOT NULL DEFAULT 'big_endian' CHECK(byte_order IN('big_endian','little_endian')),"
        "word_order TEXT NOT NULL DEFAULT 'high_word_first' CHECK(word_order IN('high_word_first','low_word_first')),"
        "quality_address INTEGER NOT NULL CHECK(quality_address>=0 AND quality_address<=65535),"
        "enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN(0,1)),"
        "created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_modbus_mappings_device ON modbus_register_mappings(device_id);"
        "CREATE INDEX IF NOT EXISTS idx_modbus_mappings_start_address ON modbus_register_mappings(start_address);"
        "CREATE INDEX IF NOT EXISTS idx_modbus_mappings_enabled ON modbus_register_mappings(enabled);";
    status = execute_sql_locked(schema_sql, error_message);
    if (is_ok(status)) {
        // 仅全新空库的建库路径开放一次性入口。
        status = execute_sql_locked(
            "INSERT INTO config_meta(key,value,updated_at) "
            "VALUES('first_boot_admin_entry_used','false',datetime('now','localtime'));",
            error_message);
    }
    if (is_ok(status)) {
        status = execute_sql_locked("PRAGMA user_version = 9;", error_message);
    }
    if (is_ok(status)) {
        status = execute_sql_locked("COMMIT;", error_message);
    }
    if (!is_ok(status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
    }
    return status;
}

}  // namespace edge_controller
