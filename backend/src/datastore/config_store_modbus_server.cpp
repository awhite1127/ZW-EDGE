// 北向 Modbus Server 配置与寄存器映射持久化；不拥有运行时 Bank 或网络生命周期。
#include "datastore/config_store.h"
#include "datastore/config_store_internal.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/sqlite_compat.h"

namespace edge_controller {

using namespace config_store_internal;

namespace {

// 绑定映射。
bool bind_mapping(sqlite3_stmt* statement, const ModbusRegisterMapping& mapping)
{
    return bind_text(statement, 1, mapping.mapping_id) &&
        bind_text(statement, 2, mapping.device_id) &&
        bind_text(statement, 3, mapping.point_key) &&
        bind_text(statement, 4, mapping.device_name_snapshot) &&
        bind_text(statement, 5, mapping.point_name_snapshot) &&
        bind_int(statement, 6, static_cast<int>(mapping.start_address)) &&
        bind_text(statement, 7, to_string(mapping.data_type)) &&
        sqlite3_bind_double(statement, 8, mapping.value_multiplier) == SQLITE_OK &&
        sqlite3_bind_double(statement, 9, mapping.value_offset) == SQLITE_OK &&
        bind_text(statement, 10, to_string(mapping.byte_order)) &&
        bind_text(statement, 11, to_string(mapping.word_order)) &&
        bind_int(statement, 12, static_cast<int>(mapping.quality_address)) &&
        bind_int(statement, 13, mapping.enabled ? 1 : 0) &&
        bind_int64(statement, 14, static_cast<sqlite3_int64>(mapping.created_at_ms)) &&
        bind_int64(statement, 15, static_cast<sqlite3_int64>(mapping.updated_at_ms));
}

// 解码映射。
StatusCode decode_mapping(sqlite3_stmt* statement, ModbusRegisterMapping* mapping, std::string* error_message)
{
    ModbusRegisterMapping loaded;
    loaded.mapping_id = column_text(statement, 0);
    loaded.device_id = column_text(statement, 1);
    loaded.point_key = column_text(statement, 2);
    loaded.device_name_snapshot = column_text(statement, 3);
    loaded.point_name_snapshot = column_text(statement, 4);
    const auto start_address = sqlite3_column_int(statement, 5);
    const auto quality_address = sqlite3_column_int(statement, 11);
    if (start_address < 0 || start_address > 65535 || quality_address < 0 || quality_address > 65535 ||
        !parse_modbus_register_data_type(column_text(statement, 6), &loaded.data_type) ||
        !parse_modbus_byte_order(column_text(statement, 9), &loaded.byte_order) ||
        !parse_modbus_word_order(column_text(statement, 10), &loaded.word_order)) {
        if (error_message != nullptr) *error_message = "modbus_register_mappings 中存在非法枚举或地址";
        return StatusCode::kInvalidState;
    }
    loaded.start_address = static_cast<RegisterAddress>(start_address);
    loaded.value_multiplier = sqlite3_column_double(statement, 7);
    loaded.value_offset = sqlite3_column_double(statement, 8);
    loaded.quality_address = static_cast<RegisterAddress>(quality_address);
    loaded.enabled = sqlite3_column_int(statement, 12) != 0;
    const auto created = column_int64(statement, 13);
    const auto updated = column_int64(statement, 14);
    if (created <= 0 || updated < created) {
        if (error_message != nullptr) *error_message = "modbus_register_mappings 中存在非法时间戳";
        return StatusCode::kInvalidState;
    }
    loaded.created_at_ms = static_cast<TimestampMs>(created);
    loaded.updated_at_ms = static_cast<TimestampMs>(updated);
    normalize_modbus_register_mapping(&loaded);
    const auto status = validate_modbus_register_mapping(loaded, "modbus_register_mappings", error_message);
    if (!is_ok(status)) return StatusCode::kInvalidState;
    *mapping = std::move(loaded);
    return StatusCode::kOk;
}

}  // namespace

// 加载Modbus服务设置。
StatusCode ConfigStore::load_modbus_server_settings(
    ModbusServerSettings* settings,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_modbus_server_settings_locked(settings, error_message);
}

// 读取并初始化Modbus服务设置。
StatusCode ConfigStore::load_or_initialize_modbus_server_settings(
    ModbusServerSettings* settings,
    std::string* error_message)
{
    if (settings == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 Modbus Server 配置输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto status = load_modbus_server_settings_locked(settings, error_message);
    if (is_ok(status)) return status;
    if (status != StatusCode::kNotFound) return status;
    if (error_message != nullptr) error_message->clear();

    ModbusServerSettings defaults;
    normalize_modbus_server_settings(&defaults);
    status = validate_modbus_server_settings(defaults, "modbus_server_settings", error_message);
    if (!is_ok(status)) return status;
    status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) return status;
    status = save_modbus_server_settings_locked(defaults, error_message);
    if (is_ok(status)) status = set_meta_locked("modbus_server_settings_initialized", "true", error_message);
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    if (is_ok(status)) *settings = std::move(defaults);
    return status;
}

// 保存Modbus服务设置。
StatusCode ConfigStore::save_modbus_server_settings(
    const ModbusServerSettings& settings,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return save_modbus_server_settings_locked(settings, error_message);
}

// 加载Modbus服务设置。
StatusCode ConfigStore::load_modbus_server_settings_locked(
    ModbusServerSettings* settings,
    std::string* error_message) const
{
    if (settings == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 Modbus Server 配置输出参数";
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    Statement statement(database_,
        "SELECT enabled,listen_address,listen_port,unit_id,strict_unit_id,max_clients,"
        "idle_timeout_seconds,max_read_registers FROM modbus_server_settings WHERE id=1;");
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备读取 modbus_server_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "modbus_server_settings 尚未初始化";
        return StatusCode::kNotFound;
    }
    if (step != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "读取 modbus_server_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    const auto port = sqlite3_column_int(statement.get(), 2);
    const auto unit_id = sqlite3_column_int(statement.get(), 3);
    const auto max_clients = sqlite3_column_int64(statement.get(), 5);
    const auto idle_timeout = sqlite3_column_int64(statement.get(), 6);
    const auto max_read = sqlite3_column_int(statement.get(), 7);
    if (port < 1 || port > 65535 || unit_id < 0 || unit_id > 255 || max_clients < 0 ||
        max_clients > UINT32_MAX || idle_timeout < 0 || idle_timeout > UINT32_MAX ||
        max_read < 0 || max_read > 65535) {
        if (error_message != nullptr) *error_message = "modbus_server_settings 中存在越界字段";
        return StatusCode::kInvalidState;
    }
    ModbusServerSettings loaded;
    loaded.enabled = sqlite3_column_int(statement.get(), 0) != 0;
    loaded.listen_address = column_text(statement.get(), 1);
    loaded.listen_port = static_cast<std::uint16_t>(port);
    loaded.unit_id = static_cast<std::uint8_t>(unit_id);
    loaded.strict_unit_id = sqlite3_column_int(statement.get(), 4) != 0;
    loaded.max_clients = static_cast<std::uint32_t>(max_clients);
    loaded.idle_timeout_seconds = static_cast<std::uint32_t>(idle_timeout);
    loaded.max_read_registers = static_cast<std::uint16_t>(max_read);
    normalize_modbus_server_settings(&loaded);
    const auto status = validate_modbus_server_settings(loaded, "modbus_server_settings", error_message);
    if (!is_ok(status)) return StatusCode::kInvalidState;
    *settings = std::move(loaded);
    return StatusCode::kOk;
}

// 保存Modbus服务设置。
StatusCode ConfigStore::save_modbus_server_settings_locked(
    const ModbusServerSettings& settings,
    std::string* error_message)
{
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    auto normalized = settings;
    normalize_modbus_server_settings(&normalized);
    const auto validation = validate_modbus_server_settings(normalized, "modbus_server_settings", error_message);
    if (!is_ok(validation)) return validation;
    Statement statement(database_,
        "INSERT OR REPLACE INTO modbus_server_settings(id,enabled,listen_address,listen_port,unit_id,"
        "strict_unit_id,max_clients,idle_timeout_seconds,max_read_registers,updated_at_ms)"
        " VALUES(1,?,?,?,?,?,?,?,?,CAST(strftime('%s','now') AS INTEGER)*1000);");
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备写入 modbus_server_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (!bind_int(statement.get(), 1, normalized.enabled ? 1 : 0) ||
        !bind_text(statement.get(), 2, normalized.listen_address) ||
        !bind_int(statement.get(), 3, normalized.listen_port) ||
        !bind_int(statement.get(), 4, normalized.unit_id) ||
        !bind_int(statement.get(), 5, normalized.strict_unit_id ? 1 : 0) ||
        !bind_int(statement.get(), 6, static_cast<int>(normalized.max_clients)) ||
        !bind_int(statement.get(), 7, static_cast<int>(normalized.idle_timeout_seconds)) ||
        !bind_int(statement.get(), 8, normalized.max_read_registers)) {
        if (error_message != nullptr) *error_message = "绑定 modbus_server_settings 参数失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "写入 modbus_server_settings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 加载Modbus寄存器映射。
StatusCode ConfigStore::load_modbus_register_mappings(
    std::vector<ModbusRegisterMapping>* mappings,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return load_modbus_register_mappings_locked(mappings, error_message);
}

// 加载Modbus寄存器映射。
StatusCode ConfigStore::load_modbus_register_mappings_locked(
    std::vector<ModbusRegisterMapping>* mappings,
    std::string* error_message) const
{
    if (mappings == nullptr) {
        if (error_message != nullptr) *error_message = "缺少 Modbus 映射输出参数";
        return StatusCode::kInvalidArgument;
    }
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    Statement statement(database_,
        "SELECT mapping_id,device_id,point_key,device_name_snapshot,point_name_snapshot,start_address,"
        "data_type,value_multiplier,value_offset,byte_order,word_order,quality_address,enabled,"
        "created_at_ms,updated_at_ms FROM modbus_register_mappings "
        "ORDER BY start_address ASC,quality_address ASC,mapping_id ASC;");
    if (!statement.ok()) {
        if (error_message != nullptr) *error_message = "准备读取 modbus_register_mappings 失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    std::vector<ModbusRegisterMapping> loaded;
    for (;;) {
        const auto step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            if (error_message != nullptr) *error_message = "读取 modbus_register_mappings 失败：" + sqlite_error(database_);
            return StatusCode::kIoError;
        }
        ModbusRegisterMapping mapping;
        const auto status = decode_mapping(statement.get(), &mapping, error_message);
        if (!is_ok(status)) return status;
        loaded.push_back(std::move(mapping));
    }
    const auto validation = validate_modbus_register_mappings(loaded, error_message);
    if (!is_ok(validation)) return StatusCode::kInvalidState;
    *mappings = std::move(loaded);
    return StatusCode::kOk;
}

// 创建Modbus寄存器映射。
StatusCode ConfigStore::create_modbus_register_mapping(
    const ModbusRegisterMapping& mapping,
    std::string* error_message)
{
    auto normalized = mapping;
    normalize_modbus_register_mapping(&normalized);
    const auto single_status = validate_modbus_register_mapping(normalized, "modbus_register_mapping", error_message);
    if (!is_ok(single_status)) return single_status;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) return status;
    std::vector<ModbusRegisterMapping> mappings;
    status = load_modbus_register_mappings_locked(&mappings, error_message);
    if (is_ok(status) && std::any_of(mappings.begin(), mappings.end(), [&](const auto& item) {
            return item.mapping_id == normalized.mapping_id;
        })) {
        if (error_message != nullptr) *error_message = "Modbus mapping_id 已存在：" + normalized.mapping_id;
        status = StatusCode::kInvalidState;
    }
    if (is_ok(status)) {
        mappings.push_back(normalized);
        status = validate_modbus_register_mappings(mappings, error_message);
    }
    if (is_ok(status)) {
        Statement statement(database_,
            "INSERT INTO modbus_register_mappings(mapping_id,device_id,point_key,device_name_snapshot,"
            "point_name_snapshot,start_address,data_type,value_multiplier,value_offset,byte_order,word_order,"
            "quality_address,enabled,created_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
        if (!statement.ok() || !bind_mapping(statement.get(), normalized)) {
            if (error_message != nullptr) *error_message = "准备或绑定 Modbus 映射写入失败：" + sqlite_error(database_);
            status = StatusCode::kIoError;
        } else if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) *error_message = "创建 Modbus 映射失败：" + sqlite_error(database_);
            status = StatusCode::kIoError;
        }
    }
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    return status;
}

// 更新Modbus寄存器映射。
StatusCode ConfigStore::update_modbus_register_mapping(
    const ModbusRegisterMapping& mapping,
    std::string* error_message)
{
    auto normalized = mapping;
    normalize_modbus_register_mapping(&normalized);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    auto status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) return status;
    std::vector<ModbusRegisterMapping> mappings;
    status = load_modbus_register_mappings_locked(&mappings, error_message);
    auto found = mappings.end();
    if (is_ok(status)) found = std::find_if(mappings.begin(), mappings.end(), [&](const auto& item) {
        return item.mapping_id == normalized.mapping_id;
    });
    if (is_ok(status) && found == mappings.end()) {
        if (error_message != nullptr) *error_message = "Modbus 映射不存在：" + normalized.mapping_id;
        status = StatusCode::kNotFound;
    }
    if (is_ok(status)) {
        normalized.created_at_ms = found->created_at_ms;
        const auto single_status = validate_modbus_register_mapping(normalized, "modbus_register_mapping", error_message);
        if (!is_ok(single_status)) status = single_status;
    }
    if (is_ok(status)) {
        *found = normalized;
        status = validate_modbus_register_mappings(mappings, error_message);
    }
    if (is_ok(status)) {
        Statement statement(database_,
            "UPDATE modbus_register_mappings SET device_id=?,point_key=?,device_name_snapshot=?,"
            "point_name_snapshot=?,start_address=?,data_type=?,value_multiplier=?,value_offset=?,byte_order=?,"
            "word_order=?,quality_address=?,enabled=?,updated_at_ms=? WHERE mapping_id=?;");
        if (!statement.ok() ||
            !bind_text(statement.get(), 1, normalized.device_id) ||
            !bind_text(statement.get(), 2, normalized.point_key) ||
            !bind_text(statement.get(), 3, normalized.device_name_snapshot) ||
            !bind_text(statement.get(), 4, normalized.point_name_snapshot) ||
            !bind_int(statement.get(), 5, normalized.start_address) ||
            !bind_text(statement.get(), 6, to_string(normalized.data_type)) ||
            sqlite3_bind_double(statement.get(), 7, normalized.value_multiplier) != SQLITE_OK ||
            sqlite3_bind_double(statement.get(), 8, normalized.value_offset) != SQLITE_OK ||
            !bind_text(statement.get(), 9, to_string(normalized.byte_order)) ||
            !bind_text(statement.get(), 10, to_string(normalized.word_order)) ||
            !bind_int(statement.get(), 11, normalized.quality_address) ||
            !bind_int(statement.get(), 12, normalized.enabled ? 1 : 0) ||
            !bind_int64(statement.get(), 13, static_cast<sqlite3_int64>(normalized.updated_at_ms)) ||
            !bind_text(statement.get(), 14, normalized.mapping_id)) {
            if (error_message != nullptr) *error_message = "准备或绑定 Modbus 映射更新失败：" + sqlite_error(database_);
            status = StatusCode::kIoError;
        } else if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) *error_message = "更新 Modbus 映射失败：" + sqlite_error(database_);
            status = StatusCode::kIoError;
        }
    }
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    return status;
}

// 删除Modbus寄存器映射。
StatusCode ConfigStore::delete_modbus_register_mapping(
    const std::string& mapping_id,
    std::string* error_message)
{
    if (mapping_id.empty()) {
        if (error_message != nullptr) *error_message = "mapping_id 不能为空";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    Statement statement(database_, "DELETE FROM modbus_register_mappings WHERE mapping_id=?;");
    if (!statement.ok() || !bind_text(statement.get(), 1, mapping_id)) {
        if (error_message != nullptr) *error_message = "准备删除 Modbus 映射失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "删除 Modbus 映射失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_changes(database_) == 0) {
        if (error_message != nullptr) *error_message = "Modbus 映射不存在：" + mapping_id;
        return StatusCode::kNotFound;
    }
    return StatusCode::kOk;
}

// 替换Modbus寄存器映射。
StatusCode ConfigStore::replace_modbus_register_mappings(
    const std::vector<ModbusRegisterMapping>& mappings,
    std::string* error_message)
{
    auto normalized = mappings;
    for (auto& mapping : normalized) normalize_modbus_register_mapping(&mapping);
    auto status = validate_modbus_register_mappings(normalized, error_message);
    if (!is_ok(status)) return status;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_ready_locked(error_message)) return StatusCode::kInvalidState;
    status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(status)) return status;
    status = execute_sql_locked("DELETE FROM modbus_register_mappings;", error_message);
    if (is_ok(status) && !normalized.empty()) {
        Statement statement(database_,
            "INSERT INTO modbus_register_mappings(mapping_id,device_id,point_key,device_name_snapshot,"
            "point_name_snapshot,start_address,data_type,value_multiplier,value_offset,byte_order,word_order,"
            "quality_address,enabled,created_at_ms,updated_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
        if (!statement.ok()) {
            if (error_message != nullptr) *error_message = "准备替换 Modbus 映射失败：" + sqlite_error(database_);
            status = StatusCode::kIoError;
        }
        for (const auto& mapping : normalized) {
            if (!is_ok(status)) break;
            sqlite3_reset(statement.get());
            sqlite3_clear_bindings(statement.get());
            if (!bind_mapping(statement.get(), mapping) || sqlite3_step(statement.get()) != SQLITE_DONE) {
                if (error_message != nullptr) *error_message = "替换 Modbus 映射失败：" + sqlite_error(database_);
                status = StatusCode::kIoError;
            }
        }
    }
    if (is_ok(status)) status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(status)) execute_sql_locked("ROLLBACK;", nullptr);
    return status;
}

}  // namespace edge_controller
