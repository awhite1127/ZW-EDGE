// 设备模板仓库：维护模板、字段定义和内置模板初始化，并保证多表写入的事务一致性。
#include "datastore/device_template_store.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/filesystem_compat.h"
#include "common/logger.h"
#include "common/sqlite_compat.h"
#include "common/time_utils.h"
#include "datastore/database_paths.h"
#include "datastore/sqlite_helpers.h"
#include "model/builtin_device_templates.h"
#include "protocol/modbus_rtu_protocol.h"

namespace edge_controller {

namespace {

using sqlite_helpers::Statement;
using sqlite_helpers::bind_double;
using sqlite_helpers::bind_int;
using sqlite_helpers::bind_int64;
using sqlite_helpers::bind_text;
using sqlite_helpers::column_text;
using sqlite_helpers::schema_migration_required_message;
using sqlite_helpers::sqlite_error;

// 返回去除首尾空白的字符串副本。
std::string trim_copy(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// 追加错误信息。
void append_error(std::string* target, const std::string& message)
{
    if (target == nullptr || message.empty()) {
        return;
    }
    if (!target->empty()) {
        *target += "；";
    }
    *target += message;
}

// 判断两组枚举项是否一致。
bool same_enum_items(
    const std::vector<DeviceTemplateEnumItemDefinition>& left,
    const std::vector<DeviceTemplateEnumItemDefinition>& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& first, const auto& second) {
               return first.value == second.value && first.label == second.label &&
                      first.sort_order == second.sort_order;
           });
}

// 判断两组设备类型字段是否一致。
bool same_template_fields(
    const std::vector<DeviceTemplateFieldDefinition>& left,
    const std::vector<DeviceTemplateFieldDefinition>& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& first, const auto& second) {
               return first.field_key == second.field_key &&
                      first.display_name == second.display_name &&
                      first.parser_id == second.parser_id && first.unit == second.unit &&
                      first.data_type == second.data_type &&
                      first.register_offset == second.register_offset &&
                      first.register_count == second.register_count &&
                      first.scale == second.scale && first.value_offset == second.value_offset &&
                       first.precision == second.precision && first.summary == second.summary &&
                       device_template_field_history_enabled(first) ==
                           device_template_field_history_enabled(second) &&
                      first.display_order == second.display_order &&
                      first.invalid_rule_type == second.invalid_rule_type &&
                      first.invalid_rule_value == second.invalid_rule_value &&
                      first.invalid_rule_min == second.invalid_rule_min &&
                      first.invalid_rule_max == second.invalid_rule_max &&
                      first.byte_order == second.byte_order &&
                      first.word_order == second.word_order &&
                      first.read_block_key == second.read_block_key &&
                      first.bit_index == second.bit_index &&
                      device_template_field_show_in_realtime(first) ==
                          device_template_field_show_in_realtime(second) &&
                      first.realtime_group_id == second.realtime_group_id &&
                      same_enum_items(first.enum_items, second.enum_items);
           });
}

// 判断两组读取区块是否一致。
bool same_read_blocks(
    const std::vector<DeviceTemplateReadBlockDefinition>& left,
    const std::vector<DeviceTemplateReadBlockDefinition>& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& first, const auto& second) {
               return first.block_key == second.block_key &&
                      first.display_name == second.display_name &&
                      first.function_code == second.function_code &&
                      first.start_offset == second.start_offset &&
                      first.register_count == second.register_count &&
                      first.sort_order == second.sort_order;
           });
}

// 判断两组实时分组是否一致。
bool same_realtime_groups(
    const std::vector<DeviceTemplateRealtimeGroupDefinition>& left,
    const std::vector<DeviceTemplateRealtimeGroupDefinition>& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& first, const auto& second) {
               return first.group_id == second.group_id &&
                      first.display_name == second.display_name &&
                      first.sort_order == second.sort_order;
           });
}

bool same_write_commands(
    const std::vector<DeviceTemplateWriteCommandDefinition>& left,
    const std::vector<DeviceTemplateWriteCommandDefinition>& right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& first, const auto& second) {
               return first.key == second.key && first.name == second.name &&
                      first.description == second.description && first.group == second.group &&
                      first.function_code == second.function_code &&
                      first.register_offset == second.register_offset &&
                      first.register_count == second.register_count &&
                      first.has_absolute_register == second.has_absolute_register &&
                      first.absolute_register == second.absolute_register &&
                      first.fixed_values == second.fixed_values &&
                      first.value_fields.empty() == second.value_fields.empty() &&
                      first.warnings == second.warnings &&
                      first.require_confirm == second.require_confirm &&
                      first.confirm_text == second.confirm_text &&
                      first.success_hint == second.success_hint;
           });
}

// 判断两份自定义设备类型定义是否一致。
bool same_custom_template_definition(
    const DeviceTemplateDefinition& stored,
    const DeviceTemplateDefinition& requested)
{
    return !stored.builtin && stored.template_id == requested.template_id &&
           stored.display_name == requested.display_name &&
           stored.description == requested.description &&
           stored.default_start_register == requested.default_start_register &&
           stored.device_address_stride == requested.device_address_stride &&
           stored.realtime_grouping_enabled == requested.realtime_grouping_enabled &&
           same_realtime_groups(stored.realtime_groups, requested.realtime_groups) &&
           same_read_blocks(stored.read_blocks, requested.read_blocks) &&
           same_template_fields(stored.fields, requested.fields) &&
           same_write_commands(stored.write_commands, requested.write_commands);
}

// 确保数据库文件的父目录存在。
StatusCode ensure_parent_directory(const edge::fs::path& path, std::string* error_message)
{
    if (!path.has_parent_path()) {
        return StatusCode::kOk;
    }

    std::error_code create_error;
    edge::fs::create_directories(path.parent_path(), create_error);
    if (create_error) {
        if (error_message != nullptr) {
            *error_message = "创建设备模板数据库目录失败：" + path.parent_path().string() +
                             "，原因=" + create_error.message();
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

bool delete_template_preferences(sqlite3* database, const std::string& template_id)
{
    const auto realtime_prefix = "device_template.realtime_display." + template_id + ".";
    const auto history_prefix = "device_template.history_enabled." + template_id + ".";
    const auto group_field_prefix = "device_template.realtime_group_id." + template_id + ".";
    Statement statement(
        database,
        "DELETE FROM config_meta WHERE key IN (?,?,?) OR substr(key,1,?)=? "
        "OR substr(key,1,?)=? OR substr(key,1,?)=?;");
    return statement.ok() &&
           bind_text(statement.get(), 1, "device_template.realtime_grouping_enabled." + template_id) &&
           bind_text(statement.get(), 2, "device_template.realtime_groups." + template_id) &&
           bind_text(statement.get(), 3, "device_template.write_commands." + template_id) &&
           bind_int(statement.get(), 4, static_cast<int>(realtime_prefix.size())) &&
           bind_text(statement.get(), 5, realtime_prefix) &&
           bind_int(statement.get(), 6, static_cast<int>(history_prefix.size())) &&
           bind_text(statement.get(), 7, history_prefix) &&
           bind_int(statement.get(), 8, static_cast<int>(group_field_prefix.size())) &&
           bind_text(statement.get(), 9, group_field_prefix) &&
           sqlite3_step(statement.get()) == SQLITE_DONE;
}

// 校验自定义设备类型标识是否安全。
bool is_safe_custom_id(const std::string& value)
{
    if (value.empty()) {
        return false;
    }
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' ||
            ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

// 判断设备类型字段的数据类型是否受支持。
bool is_supported_data_type(const std::string& value)
{
    return value == "bool" || value == "uint16" || value == "int16" ||
           value == "uint32" || value == "int32" || value == "float32";
}

// 判断设备类型字段解析器是否受支持。
bool is_supported_parser_id(const std::string& value)
{
    return value == "scaled_uint16" ||
           value == "scaled_int16" ||
           value == "scaled_uint32" ||
           value == "scaled_int32" ||
           value == "scaled_float32" ||
           value == "scaled_high_uint8" ||
           value == "scaled_low_uint8" ||
           value == "bit_uint16";
}

// 判断字段内字节排列是否为稳定协议值。
bool is_supported_byte_order(const std::string& value)
{
    return value == "big_endian" || value == "little_endian";
}

// 判断 32 位字段的字排列是否为稳定协议值。
bool is_supported_word_order(const std::string& value)
{
    return value == "high_word_first" || value == "low_word_first";
}

// 返回指定数据类型占用的寄存器数量。
std::uint16_t required_register_count(const std::string& data_type)
{
    if (data_type == "bool" || data_type == "uint16" || data_type == "int16") {
        return 1;
    }
    if (data_type == "uint32" || data_type == "int32" || data_type == "float32") {
        return 2;
    }
    return 0;
}

// 返回指定解析器要求的数据类型。
std::string required_data_type_for_parser_id(const std::string& parser_id)
{
    if (parser_id == "scaled_uint16") {
        return "uint16";
    }
    if (parser_id == "bit_uint16") {
        return "bool";
    }
    if (parser_id == "scaled_int16") {
        return "int16";
    }
    if (parser_id == "scaled_uint32") {
        return "uint32";
    }
    if (parser_id == "scaled_int32") {
        return "int32";
    }
    if (parser_id == "scaled_float32") {
        return "float32";
    }
    if (parser_id == "scaled_high_uint8" || parser_id == "scaled_low_uint8") {
        return "uint16";
    }
    return {};
}

}  // namespace

// 关闭数据库连接并释放存储资源；内部加锁保证析构安全。
DeviceTemplateStore::~DeviceTemplateStore()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_database_locked();
}

// 初始化设备模板数据库并补齐内置模板。
StatusCode DeviceTemplateStore::initialize(
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
    if (is_ok(status)) status = initialize_schema_locked(&error);
    if (is_ok(status)) status = seed_builtin_templates_locked(&error);

    std::vector<DeviceTemplateDefinition> templates;
    if (is_ok(status)) status = load_templates_locked(&templates, &error);
    if (!is_ok(status)) {
        const auto message = error.empty()
                                 ? "设备模板 SQLite 存储初始化失败"
                                 : error;
        Logger::warn(message);
        close_database_locked();
        if (error_message != nullptr) *error_message = message;
        return status;
    }

    set_device_templates(std::move(templates));
    initialized_ = true;
    Logger::info("设备模板 SQLite 存储初始化完成：" + database_path_);
    return StatusCode::kOk;
}

// 读取全部设备模板定义。
StatusCode DeviceTemplateStore::load_templates(
    std::vector<DeviceTemplateDefinition>* templates,
    std::string* error_message) const
{
    if (templates == nullptr) {
        if (error_message != nullptr) {
            *error_message = "设备模板列表输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    return load_templates_locked(templates, error_message);
}

// 校验类型。
StatusCode DeviceTemplateStore::validate_template_definition(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    return validate_template_locked(device_template, false, error_message);
}

// 批量新增或更新自定义设备类型。
StatusCode DeviceTemplateStore::upsert_custom_templates(
    const std::vector<DeviceTemplateDefinition>& device_templates,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    std::set<std::string> template_ids;
    for (const auto& device_template : device_templates) {
        std::string validation_error;
        const auto validation_status =
            validate_template_locked(device_template, false, &validation_error);
        if (!is_ok(validation_status)) {
            if (error_message != nullptr) {
                *error_message = "设备类型“" + device_template.display_name + "”（" +
                                 device_template.template_id + "）无效：" + validation_error;
            }
            return validation_status;
        }
        if (!template_ids.insert(trim_copy(device_template.template_id)).second) {
            if (error_message != nullptr) {
                *error_message = "自定义设备类型标识重复：" + device_template.template_id;
            }
            return StatusCode::kInvalidArgument;
        }
    }

    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(begin_status)) return begin_status;

    for (const auto& device_template : device_templates) {
        std::string write_error;
        const auto write_status = upsert_custom_template_locked(device_template, &write_error);
        if (!is_ok(write_status)) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = write_error.empty() ? "写入自定义设备类型失败" : write_error;
            }
            return write_status;
        }
    }

    std::vector<DeviceTemplateDefinition> templates;
    const auto load_status = load_templates_locked(&templates, error_message);
    if (!is_ok(load_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return load_status;
    }
    const auto commit_status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(commit_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commit_status;
    }
    set_device_templates(std::move(templates));
    return StatusCode::kOk;
}

// 删除不在保留集合中的自定义设备类型。
StatusCode DeviceTemplateStore::prune_custom_templates(
    const std::vector<std::string>& keep_template_ids,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }
    const std::set<std::string> keep(keep_template_ids.begin(), keep_template_ids.end());

    std::vector<std::string> remove_ids;
    Statement list_statement(
        database_, "SELECT template_id FROM device_templates WHERE builtin=0 ORDER BY template_id;");
    if (!list_statement.ok()) {
        if (error_message != nullptr) *error_message = "准备读取自定义设备类型失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    int step_status = SQLITE_ROW;
    while ((step_status = sqlite3_step(list_statement.get())) == SQLITE_ROW) {
        const auto template_id = column_text(list_statement.get(), 0);
        if (keep.find(template_id) == keep.end()) remove_ids.push_back(template_id);
    }
    if (step_status != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "读取自定义设备类型失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (remove_ids.empty()) return StatusCode::kOk;

    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(begin_status)) return begin_status;
    for (const auto& template_id : remove_ids) {
        Statement fields_statement(
            database_, "DELETE FROM device_template_fields WHERE template_id=?;");
        Statement blocks_statement(
            database_, "DELETE FROM device_template_read_blocks WHERE template_id=?;");
        Statement template_statement(
            database_, "DELETE FROM device_templates WHERE template_id=? AND builtin=0;");
        if (!delete_template_preferences(database_, template_id) ||
            !fields_statement.ok() || !bind_text(fields_statement.get(), 1, template_id) ||
            sqlite3_step(fields_statement.get()) != SQLITE_DONE ||
            !blocks_statement.ok() || !bind_text(blocks_statement.get(), 1, template_id) ||
            sqlite3_step(blocks_statement.get()) != SQLITE_DONE ||
            !template_statement.ok() || !bind_text(template_statement.get(), 1, template_id) ||
            sqlite3_step(template_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "删除配置包外的自定义设备类型“" + template_id + "”失败：" +
                                 sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    std::vector<DeviceTemplateDefinition> templates;
    const auto load_status = load_templates_locked(&templates, error_message);
    if (!is_ok(load_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return load_status;
    }
    const auto commit_status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(commit_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commit_status;
    }
    set_device_templates(std::move(templates));
    return StatusCode::kOk;
}

// 在持锁状态下加载模板。
StatusCode DeviceTemplateStore::load_templates_locked(
    std::vector<DeviceTemplateDefinition>* templates,
    std::string* error_message) const
{
    if (templates == nullptr) {
        if (error_message != nullptr) {
            *error_message = "设备模板列表输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }

    const char* template_sql =
        "SELECT template_id, template_name, description, default_start_register, "
        "device_address_stride, builtin, "
        "COALESCE((SELECT CAST(value AS INTEGER) FROM config_meta p WHERE p.key = "
        "'device_template.realtime_grouping_enabled.' || device_templates.template_id), 0), "
        "COALESCE((SELECT value FROM config_meta p WHERE p.key = "
        "'device_template.realtime_groups.' || device_templates.template_id), '[]') "
        "FROM device_templates ORDER BY builtin DESC, template_id ASC;";
    Statement template_statement(database_, template_sql);
    if (!template_statement.ok()) {
        if (error_message != nullptr) {
            *error_message = "准备设备模板查询失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    std::vector<DeviceTemplateDefinition> result;
    int template_step_status = SQLITE_ROW;
    while ((template_step_status = sqlite3_step(template_statement.get())) == SQLITE_ROW) {
        DeviceTemplateDefinition item;
        item.template_id = column_text(template_statement.get(), 0);
        item.display_name = column_text(template_statement.get(), 1);
        item.description = column_text(template_statement.get(), 2);
        item.default_start_register = static_cast<RegisterAddress>(sqlite3_column_int(template_statement.get(), 3));
        item.device_address_stride = static_cast<std::uint32_t>(sqlite3_column_int64(template_statement.get(), 4));
        item.builtin = sqlite3_column_int(template_statement.get(), 5) != 0;
        item.realtime_grouping_enabled = sqlite3_column_int(template_statement.get(), 6) != 0;
        const auto groups_text = column_text(template_statement.get(), 7);
        const auto groups_json = nlohmann::json::parse(groups_text, nullptr, false);
        if (!groups_json.is_array()) {
            if (error_message != nullptr) {
                *error_message = "设备模板实时展示分组配置损坏：" + item.template_id;
            }
            return StatusCode::kInvalidState;
        }
        for (const auto& group_json : groups_json) {
            if (!group_json.is_object() || !group_json.contains("id") || !group_json["id"].is_string() ||
                !group_json.contains("name") || !group_json["name"].is_string() ||
                !group_json.contains("order") || !group_json["order"].is_number_integer()) {
                if (error_message != nullptr) {
                    *error_message = "设备模板实时展示分组条目损坏：" + item.template_id;
                }
                return StatusCode::kInvalidState;
            }
            const auto group_order = group_json["order"].get<std::int64_t>();
            if (group_order < 0 || group_order > std::numeric_limits<int>::max()) {
                if (error_message != nullptr) {
                    *error_message = "设备模板实时展示分组顺序损坏：" + item.template_id;
                }
                return StatusCode::kInvalidState;
            }
            item.realtime_groups.push_back({
                group_json["id"].get<std::string>(),
                group_json["name"].get<std::string>(),
                static_cast<int>(group_order),
            });
        }
        // 自定义类型只用于 FC03/FC04 数据采集。旧开发期保存的 write_commands
        // 不再加载到运行时；内置类型在下方统一从代码定义恢复控制能力。
        result.push_back(std::move(item));
    }
    if (template_step_status != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "读取设备模板基本信息失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const char* read_block_sql =
        "SELECT block_key,display_name,function_code,start_offset,register_count,sort_order "
        "FROM device_template_read_blocks WHERE template_id=? "
        "ORDER BY sort_order ASC,block_key ASC;";
    for (auto& device_template : result) {
        Statement read_block_statement(database_, read_block_sql);
        if (!read_block_statement.ok() ||
            !bind_text(read_block_statement.get(), 1, device_template.template_id)) {
            if (error_message != nullptr) {
                *error_message = "准备设备模板读取区块查询失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        int step_status = SQLITE_ROW;
        while ((step_status = sqlite3_step(read_block_statement.get())) == SQLITE_ROW) {
            DeviceTemplateReadBlockDefinition block;
            block.block_key = column_text(read_block_statement.get(), 0);
            block.display_name = column_text(read_block_statement.get(), 1);
            block.function_code = static_cast<std::uint32_t>(sqlite3_column_int(read_block_statement.get(), 2));
            block.start_offset = static_cast<std::uint32_t>(sqlite3_column_int64(read_block_statement.get(), 3));
            block.register_count = static_cast<std::uint32_t>(sqlite3_column_int(read_block_statement.get(), 4));
            block.sort_order = sqlite3_column_int(read_block_statement.get(), 5);
            device_template.read_blocks.push_back(std::move(block));
        }
        if (step_status != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "读取设备模板读取区块失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    const char* field_sql =
        "SELECT field_key, field_name, unit, data_type, parser_id, read_block_key, register_offset, register_count, scale, "
        "value_offset, precision, summary, display_order, invalid_rule_type, invalid_rule_value, "
        "invalid_rule_min, invalid_rule_max, "
        "byte_order, word_order, bit_index, "
        "COALESCE((SELECT CAST(value AS INTEGER) FROM config_meta p WHERE p.key = "
        "'device_template.realtime_display.' || device_template_fields.template_id || '.' || device_template_fields.field_key), 0), "
        "COALESCE((SELECT CAST(value AS INTEGER) FROM config_meta p WHERE p.key = "
        "'device_template.history_enabled.' || device_template_fields.template_id || '.' || device_template_fields.field_key), 0), "
        "COALESCE((SELECT value FROM config_meta p WHERE p.key = "
        "'device_template.realtime_group_id.' || device_template_fields.template_id || '.' || device_template_fields.field_key), '') "
        "FROM device_template_fields WHERE template_id = ? ORDER BY display_order ASC, field_key ASC;";
    for (auto& device_template : result) {
        Statement field_statement(database_, field_sql);
        if (!field_statement.ok() ||
            !bind_text(field_statement.get(), 1, device_template.template_id)) {
            if (error_message != nullptr) {
                *error_message = "准备设备模板字段查询失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }

        int field_step_status = SQLITE_ROW;
        while ((field_step_status = sqlite3_step(field_statement.get())) == SQLITE_ROW) {
            DeviceTemplateFieldDefinition field;
            field.field_key = column_text(field_statement.get(), 0);
            field.display_name = column_text(field_statement.get(), 1);
            field.unit = column_text(field_statement.get(), 2);
            field.data_type = column_text(field_statement.get(), 3);
            field.parser_id = column_text(field_statement.get(), 4);
            if (field.parser_id.empty()) {
                field.parser_id = "scaled_uint16";
            }
            field.read_block_key = column_text(field_statement.get(), 5);
            field.register_offset = static_cast<RegisterAddress>(sqlite3_column_int(field_statement.get(), 6));
            field.register_count = static_cast<RegisterCount>(sqlite3_column_int(field_statement.get(), 7));
            field.scale = sqlite3_column_double(field_statement.get(), 8);
            field.value_offset = sqlite3_column_double(field_statement.get(), 9);
            field.precision = static_cast<std::uint32_t>(sqlite3_column_int(field_statement.get(), 10));
            field.summary = sqlite3_column_int(field_statement.get(), 11) != 0;
            field.display_order = static_cast<std::uint32_t>(sqlite3_column_int(field_statement.get(), 12));
            field.invalid_rule_type = column_text(field_statement.get(), 13);
            field.invalid_rule_value = sqlite3_column_double(field_statement.get(), 14);
            field.invalid_rule_min = sqlite3_column_double(field_statement.get(), 15);
            field.invalid_rule_max = sqlite3_column_double(field_statement.get(), 16);
            field.byte_order = column_text(field_statement.get(), 17);
            field.word_order = column_text(field_statement.get(), 18);
            if (field.byte_order.empty()) field.byte_order = "big_endian";
            if (field.word_order.empty()) field.word_order = "high_word_first";
            field.bit_index = sqlite3_column_int(field_statement.get(), 19);
            field.show_in_realtime = sqlite3_column_int(field_statement.get(), 20) != 0;
            field.history_enabled = sqlite3_column_int(field_statement.get(), 21) != 0;
            field.realtime_group_id = column_text(field_statement.get(), 22);
            device_template.fields.push_back(std::move(field));
        }
        if (field_step_status != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "读取设备模板字段失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    const char* enum_sql =
        "SELECT field_key,value,label,sort_order FROM device_template_enum_items "
        "WHERE template_id=? ORDER BY field_key ASC,sort_order ASC,value ASC;";
    for (auto& device_template : result) {
        Statement enum_statement(database_, enum_sql);
        if (!enum_statement.ok() ||
            !bind_text(enum_statement.get(), 1, device_template.template_id)) {
            if (error_message != nullptr) {
                *error_message = "准备设备模板枚举项查询失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        int enum_step_status = SQLITE_ROW;
        while ((enum_step_status = sqlite3_step(enum_statement.get())) == SQLITE_ROW) {
            const auto field_key = column_text(enum_statement.get(), 0);
            const auto field = std::find_if(
                device_template.fields.begin(), device_template.fields.end(),
                [&](const auto& candidate) { return candidate.field_key == field_key; });
            if (field == device_template.fields.end()) {
                if (error_message != nullptr) {
                    *error_message = "枚举项引用不存在的字段：" + field_key;
                }
                return StatusCode::kInvalidState;
            }
            field->enum_items.push_back({
                sqlite3_column_int64(enum_statement.get(), 1),
                column_text(enum_statement.get(), 2),
                sqlite3_column_int(enum_statement.get(), 3),
            });
        }
        if (enum_step_status != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "读取设备模板枚举项失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    // 每个持久化设备类型都必须直接满足当前读取模型。
    for (auto& device_template : result) {
        std::string read_model_error;
        const auto read_model_status =
            validate_device_template_read_model(device_template, &read_model_error);
        if (!is_ok(read_model_status)) {
            if (error_message != nullptr) {
                *error_message = "设备模板 " + device_template.template_id +
                                 " 的读取区块配置无效：" + read_model_error;
            }
            return StatusCode::kInvalidState;
        }
    }

    if (result.empty()) {
        if (error_message != nullptr) {
            *error_message = "设备模板表为空";
        }
        return StatusCode::kInvalidState;
    }

    const auto builtins = builtin_device_templates();
    for (auto& device_template : result) {
        if (!device_template.builtin) {
            continue;
        }
        for (const auto& builtin_template : builtins) {
            if (builtin_template.template_id == device_template.template_id) {
                device_template.write_commands = builtin_template.write_commands;
                break;
            }
        }
    }

    for (const auto& device_template : result) {
        std::string command_error;
        if (!is_ok(validate_device_template_write_commands(device_template, &command_error))) {
            if (error_message != nullptr) {
                *error_message = command_error;
            }
            return StatusCode::kInvalidState;
        }
    }

    *templates = std::move(result);
    return StatusCode::kOk;
}

// 新增设备模板定义。
StatusCode DeviceTemplateStore::create_template(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    // 旧版本可能在模板已提交后因无关的轮询恢复错误向页面报告失败。
    // 对完全相同的重试按幂等成功处理；同 ID 不同内容仍由后续校验明确拒绝。
    std::vector<DeviceTemplateDefinition> stored_templates;
    const auto existing_load_status = load_templates_locked(&stored_templates, error_message);
    if (!is_ok(existing_load_status)) {
        return existing_load_status;
    }
    const auto stored = std::find_if(
        stored_templates.begin(), stored_templates.end(), [&](const auto& candidate) {
            return candidate.template_id == device_template.template_id;
        });
    if (stored != stored_templates.end() &&
        same_custom_template_definition(*stored, device_template)) {
        if (error_message != nullptr) error_message->clear();
        return StatusCode::kOk;
    }

    const auto validation_status = validate_template_locked(device_template, true, error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(begin_status)) {
        return begin_status;
    }

    std::string insert_error;
    const auto insert_status = insert_template_locked(device_template, false, &insert_error);
    if (!is_ok(insert_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = insert_error.empty() ? "写入设备模板失败" : insert_error;
        }
        return insert_status;
    }

    std::string preference_error;
    const auto preference_status = persist_realtime_display_preferences_locked(device_template, &preference_error);
    if (!is_ok(preference_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = preference_error.empty() ? "写入设备模板实时展示配置失败" : preference_error;
        }
        return preference_status;
    }
    const auto history_preference_status = persist_history_enabled_preferences_locked(device_template, &preference_error);
    if (!is_ok(history_preference_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = preference_error.empty() ? "更新设备模板历史记录配置失败" : preference_error;
        }
        return history_preference_status;
    }
    const auto grouping_status = persist_realtime_grouping_preferences_locked(device_template, &preference_error);
    if (!is_ok(grouping_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = preference_error.empty() ? "写入设备模板实时展示分组失败" : preference_error;
        }
        return grouping_status;
    }
    std::vector<DeviceTemplateDefinition> templates;
    const auto load_status = load_templates_locked(&templates, error_message);
    if (!is_ok(load_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return load_status;
    }

    const auto commit_status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(commit_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commit_status;
    }

    set_device_templates(std::move(templates));
    return StatusCode::kOk;
}

// 更新设备模板定义。
StatusCode DeviceTemplateStore::update_template(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    // 在锁内确认数据库可用、目标存在且不是内置类型。
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    bool builtin = false;
    const auto exists_status = get_template_builtin_locked(device_template.template_id, &builtin, error_message);
    if (!is_ok(exists_status)) {
        return exists_status;
    }
    if (builtin) {
        if (error_message != nullptr) {
            *error_message = "内置模板不能编辑";
        }
        return StatusCode::kInvalidArgument;
    }

    // 校验新定义并开启事务。
    const auto validation_status = validate_template_locked(device_template, false, error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(begin_status)) {
        return begin_status;
    }

    // 更新设备类型主记录，并清理待重建的字段和区块。
    const auto now = time_utils::local_time_string();
    const char* update_sql =
        "UPDATE device_templates SET template_name = ?, description = ?, "
        "default_start_register = ?, device_address_stride = ?, updated_at = ? "
        "WHERE template_id = ? AND builtin = 0;";
    {
        Statement update_statement(database_, update_sql);
        if (!update_statement.ok() ||
            !bind_text(update_statement.get(), 1, device_template.display_name) ||
            !bind_text(update_statement.get(), 2, device_template.description) ||
            !bind_int(update_statement.get(), 3, static_cast<int>(device_template.default_start_register)) ||
            !bind_int(update_statement.get(), 4, static_cast<int>(device_template.device_address_stride)) ||
            !bind_text(update_statement.get(), 5, now) ||
            !bind_text(update_statement.get(), 6, device_template.template_id)) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "准备设备模板更新失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_step(update_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "更新设备模板基本信息失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_changes(database_) != 1) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "设备模板不存在或不可编辑：" + device_template.template_id;
            }
            return StatusCode::kNotFound;
        }
    }

    {
        Statement delete_fields_statement(database_, "DELETE FROM device_template_fields WHERE template_id = ?;");
        if (!delete_fields_statement.ok() ||
            !bind_text(delete_fields_statement.get(), 1, device_template.template_id) ||
            sqlite3_step(delete_fields_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "清空设备模板字段失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    {
        Statement delete_blocks_statement(
            database_, "DELETE FROM device_template_read_blocks WHERE template_id = ?;");
        if (!delete_blocks_statement.ok() ||
            !bind_text(delete_blocks_statement.get(), 1, device_template.template_id) ||
            sqlite3_step(delete_blocks_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "删除设备模板旧读取区块失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    // 重建读取区块、字段和各类展示偏好。
    std::string blocks_error;
    const auto blocks_status = insert_template_read_blocks_locked(device_template, &blocks_error);
    if (!is_ok(blocks_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = blocks_error.empty() ? "写入设备模板读取区块失败" : blocks_error;
        }
        return blocks_status;
    }

    std::string fields_error;
    const auto fields_status = insert_template_fields_locked(device_template, &fields_error);
    if (!is_ok(fields_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = fields_error.empty() ? "写入设备模板字段失败" : fields_error;
        }
        return fields_status;
    }

    std::string preference_error;
    const auto preference_status = persist_realtime_display_preferences_locked(device_template, &preference_error);
    if (!is_ok(preference_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = preference_error.empty() ? "更新设备模板实时展示配置失败" : preference_error;
        }
        return preference_status;
    }
    const auto history_preference_status = persist_history_enabled_preferences_locked(device_template, &preference_error);
    if (!is_ok(history_preference_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = preference_error.empty() ? "更新设备模板历史记录配置失败" : preference_error;
        }
        return history_preference_status;
    }
    const auto grouping_status = persist_realtime_grouping_preferences_locked(device_template, &preference_error);
    if (!is_ok(grouping_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = preference_error.empty() ? "更新设备模板实时展示分组失败" : preference_error;
        }
        return grouping_status;
    }
    // 重新加载完整类型清单，提交后再刷新内存注册表。
    std::vector<DeviceTemplateDefinition> templates;
    const auto load_status = load_templates_locked(&templates, error_message);
    if (!is_ok(load_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return load_status;
    }

    const auto commit_status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(commit_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commit_status;
    }

    set_device_templates(std::move(templates));
    return StatusCode::kOk;
}

// 更新指定设备类型字段的实时显示偏好。
StatusCode DeviceTemplateStore::update_realtime_display_preference(
    const std::string& template_id,
    const std::string& field_key,
    bool show_in_realtime,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) return StatusCode::kInvalidState;
    const auto normalized_template_id = trim_copy(template_id);
    const auto normalized_field_key = trim_copy(field_key);
    Statement field_statement(database_,
        "SELECT 1 FROM device_template_fields WHERE template_id = ? AND field_key = ? LIMIT 1;");
    if (!field_statement.ok() || !bind_text(field_statement.get(), 1, normalized_template_id) ||
        !bind_text(field_statement.get(), 2, normalized_field_key)) {
        if (error_message != nullptr) *error_message = "检查模板数据项失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(field_statement.get()) != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "设备模板或数据项不存在";
        return StatusCode::kNotFound;
    }
    if (show_in_realtime) {
        Statement grouping_statement(
            database_,
            "SELECT COALESCE((SELECT CAST(value AS INTEGER) FROM config_meta WHERE key=?),0),"
            "COALESCE((SELECT value FROM config_meta WHERE key=?),'');");
        const auto enabled_key = "device_template.realtime_grouping_enabled." + normalized_template_id;
        const auto group_key = "device_template.realtime_group_id." + normalized_template_id + "." + normalized_field_key;
        if (!grouping_statement.ok() || !bind_text(grouping_statement.get(), 1, enabled_key) ||
            !bind_text(grouping_statement.get(), 2, group_key) ||
            sqlite3_step(grouping_statement.get()) != SQLITE_ROW) {
            if (error_message != nullptr) *error_message = "检查实时展示分组配置失败：" + sqlite_error(database_);
            return StatusCode::kIoError;
        }
        if (sqlite3_column_int(grouping_statement.get(), 0) != 0 &&
            column_text(grouping_statement.get(), 1).empty()) {
            if (error_message != nullptr) *error_message = "该数据项尚未分配实时展示分组，请在设备类型配置页中设置后再启用";
            return StatusCode::kInvalidArgument;
        }
    }
    Statement upsert(database_,
        "INSERT INTO config_meta(key,value,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at;");
    const auto preference_key = "device_template.realtime_display." + normalized_template_id + "." + normalized_field_key;
    if (!upsert.ok() || !bind_text(upsert.get(), 1, preference_key) ||
        !bind_text(upsert.get(), 2, show_in_realtime ? "1" : "0") ||
        !bind_text(upsert.get(), 3, time_utils::local_time_string()) || sqlite3_step(upsert.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "保存实时展示偏好失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    std::vector<DeviceTemplateDefinition> templates;
    const auto status = load_templates_locked(&templates, error_message);
    if (is_ok(status)) set_device_templates(std::move(templates));
    return status;
}

// 更新历史数据启用状态偏好。
StatusCode DeviceTemplateStore::update_history_enabled_preference(
    const std::string& template_id,
    const std::string& field_key,
    bool history_enabled,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) return StatusCode::kInvalidState;
    const auto normalized_template_id = trim_copy(template_id);
    const auto normalized_field_key = trim_copy(field_key);
    Statement field_statement(database_,
        "SELECT 1 FROM device_template_fields WHERE template_id = ? AND field_key = ? LIMIT 1;");
    if (!field_statement.ok() || !bind_text(field_statement.get(), 1, normalized_template_id) ||
        !bind_text(field_statement.get(), 2, normalized_field_key)) {
        if (error_message != nullptr) *error_message = "检查模板数据项失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_step(field_statement.get()) != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "设备模板或数据项不存在";
        return StatusCode::kNotFound;
    }
    Statement upsert(database_,
        "INSERT INTO config_meta(key,value,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at;");
    const auto preference_key = "device_template.history_enabled." + normalized_template_id + "." + normalized_field_key;
    if (!upsert.ok() || !bind_text(upsert.get(), 1, preference_key) ||
        !bind_text(upsert.get(), 2, history_enabled ? "1" : "0") ||
        !bind_text(upsert.get(), 3, time_utils::local_time_string()) || sqlite3_step(upsert.get()) != SQLITE_DONE) {
        if (error_message != nullptr) *error_message = "保存历史记录配置失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    std::vector<DeviceTemplateDefinition> templates;
    const auto status = load_templates_locked(&templates, error_message);
    if (is_ok(status)) set_device_templates(std::move(templates));
    return status;
}

StatusCode DeviceTemplateStore::delete_template(
    const std::string& template_id,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!database_available_locked(error_message)) {
        return StatusCode::kInvalidState;
    }

    const auto trimmed_template_id = trim_copy(template_id);
    if (trimmed_template_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "模板 ID 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    bool builtin = false;
    const auto exists_status = get_template_builtin_locked(trimmed_template_id, &builtin, error_message);
    if (!is_ok(exists_status)) {
        return exists_status;
    }
    if (builtin) {
        if (error_message != nullptr) {
            *error_message = "内置模板不能删除";
        }
        return StatusCode::kInvalidArgument;
    }

    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(begin_status)) {
        return begin_status;
    }

    if (!delete_template_preferences(database_, trimmed_template_id)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
            *error_message = "删除设备模板展示偏好失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    {
        Statement delete_fields_statement(database_, "DELETE FROM device_template_fields WHERE template_id = ?;");
        if (!delete_fields_statement.ok() ||
            !bind_text(delete_fields_statement.get(), 1, trimmed_template_id) ||
            sqlite3_step(delete_fields_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "删除设备模板字段失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    {
        Statement delete_blocks_statement(
            database_, "DELETE FROM device_template_read_blocks WHERE template_id = ?;");
        if (!delete_blocks_statement.ok() ||
            !bind_text(delete_blocks_statement.get(), 1, trimmed_template_id) ||
            sqlite3_step(delete_blocks_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "删除设备模板读取区块失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    {
        Statement delete_template_statement(
            database_,
            "DELETE FROM device_templates WHERE template_id = ? AND builtin = 0;");
        if (!delete_template_statement.ok() ||
            !bind_text(delete_template_statement.get(), 1, trimmed_template_id) ||
            sqlite3_step(delete_template_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "删除设备模板失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_changes(database_) != 1) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "设备模板不存在或不可删除：" + trimmed_template_id;
            }
            return StatusCode::kNotFound;
        }
    }

    std::vector<DeviceTemplateDefinition> templates;
    const auto load_status = load_templates_locked(&templates, error_message);
    if (!is_ok(load_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return load_status;
    }

    const auto commit_status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(commit_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commit_status;
    }

    set_device_templates(std::move(templates));
    return StatusCode::kOk;
}

// 返回设备类型数据库文件路径。
std::string DeviceTemplateStore::database_path() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return database_path_;
}

StatusCode DeviceTemplateStore::open_database_locked(const std::string& database_path, std::string* error_message)
{
    const edge::fs::path path(database_path);
    const auto directory_status = ensure_parent_directory(path, error_message);
    if (!is_ok(directory_status)) {
        return directory_status;
    }

    sqlite3* database = nullptr;
    if (sqlite3_open_v2(
            database_path.c_str(),
            &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        const auto detail = sqlite_error(database);
        if (database != nullptr) {
            sqlite3_close(database);
        }
        if (error_message != nullptr) {
            *error_message = "打开设备模板 SQLite 数据库失败：" + database_path + "，原因=" + detail;
        }
        return StatusCode::kIoError;
    }

    database_ = database;
    database_path_ = database_path;
    const auto configure_status = configure_database_locked(error_message);
    if (!is_ok(configure_status)) {
        close_database_locked();
        return configure_status;
    }
    return StatusCode::kOk;
}

// 在持锁状态下打开数据库并完成设备类型表结构初始化。
StatusCode DeviceTemplateStore::configure_database_locked(std::string* error_message)
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
            *error_message = "配置设备模板 SQLite 连接失败：" +
                             (detail.empty() ? sqlite_error(database_) : detail);
        }
        return configure_status;
    }
    return StatusCode::kOk;
}

// 在持锁状态下初始化数据库结构。
StatusCode DeviceTemplateStore::initialize_schema_locked(std::string* error_message)
{
    // edge-config.db 的结构和 user_version 仅由 ConfigStore 管理。
    Statement statement(
        database_,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name IN ('device_templates','device_template_fields','device_template_read_blocks','device_template_enum_items');");
    if (!statement.ok() || sqlite3_step(statement.get()) != SQLITE_ROW) {
        if (error_message != nullptr) *error_message = "检查设备模板表失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    if (sqlite3_column_int(statement.get(), 0) != 4) {
        if (error_message != nullptr) {
            *error_message = schema_migration_required_message(
                "edge-config.db 缺少设备模板表");
        }
        return StatusCode::kInvalidState;
    }
    return StatusCode::kOk;
}

// 在持锁状态下初始化内置设备类型。
StatusCode DeviceTemplateStore::seed_builtin_templates_locked(
    std::string* error_message)
{
    const auto seed_templates = builtin_device_templates();
    for (const auto& device_template : seed_templates) {
        std::string command_error;
        if (!is_ok(validate_device_template_write_commands(device_template, &command_error))) {
            if (error_message != nullptr) {
                *error_message = command_error;
            }
            return StatusCode::kInvalidState;
        }
    }

    const auto begin_status = execute_sql_locked("BEGIN IMMEDIATE TRANSACTION;", error_message);
    if (!is_ok(begin_status)) {
        return begin_status;
    }
    std::size_t inserted_count = 0;

    // 清理已经从代码中删除的内置模板，以及不再作为事实源的控制命令快照。
    std::vector<std::string> stale_builtin_ids;
    Statement builtin_statement(database_, "SELECT template_id FROM device_templates WHERE builtin=1;");
    if (!builtin_statement.ok()) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) *error_message = "读取已有内置设备模板失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    int builtin_step_status = SQLITE_ROW;
    while ((builtin_step_status = sqlite3_step(builtin_statement.get())) == SQLITE_ROW) {
        const auto template_id = column_text(builtin_statement.get(), 0);
        const auto current = std::find_if(seed_templates.begin(), seed_templates.end(), [&](const auto& item) {
            return item.template_id == template_id;
        });
        if (current == seed_templates.end()) stale_builtin_ids.push_back(template_id);
    }
    if (builtin_step_status != SQLITE_DONE) {
        execute_sql_locked("ROLLBACK;", nullptr);
        if (error_message != nullptr) *error_message = "读取已有内置设备模板失败：" + sqlite_error(database_);
        return StatusCode::kIoError;
    }
    for (const auto& template_id : stale_builtin_ids) {
        Statement delete_statement(database_, "DELETE FROM device_templates WHERE template_id=? AND builtin=1;");
        if (!delete_template_preferences(database_, template_id) || !delete_statement.ok() ||
            !bind_text(delete_statement.get(), 1, template_id) ||
            sqlite3_step(delete_statement.get()) != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) *error_message = "清理失效内置设备模板失败：" + sqlite_error(database_);
            return StatusCode::kIoError;
        }
    }
    const auto commands_cleanup_status = execute_sql_locked(
        "DELETE FROM config_meta WHERE key LIKE 'device_template.write_commands.%';",
        error_message);
    if (!is_ok(commands_cleanup_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commands_cleanup_status;
    }

    for (const auto& device_template : seed_templates) {
        Statement exists_statement(database_, "SELECT 1 FROM device_templates WHERE template_id = ? LIMIT 1;");
        if (!exists_statement.ok() ||
            !bind_text(exists_statement.get(), 1, device_template.template_id)) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "检查内置设备模板是否存在失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        const auto exists_status = sqlite3_step(exists_statement.get());
        if (exists_status == SQLITE_ROW) {
            const auto now = time_utils::local_time_string();
            Statement update_statement(
                database_,
                "UPDATE device_templates SET template_name = ?, description = ?, "
                "default_start_register = ?, device_address_stride = ?, "
                "builtin = 1, updated_at = ? "
                "WHERE template_id = ?;");
            if (!update_statement.ok() ||
                !bind_text(update_statement.get(), 1, device_template.display_name) ||
                !bind_text(update_statement.get(), 2, device_template.description) ||
                !bind_int(update_statement.get(), 3, static_cast<int>(device_template.default_start_register)) ||
                !bind_int(update_statement.get(), 4, static_cast<int>(device_template.device_address_stride)) ||
                !bind_text(update_statement.get(), 5, now) ||
                !bind_text(update_statement.get(), 6, device_template.template_id)) {
                execute_sql_locked("ROLLBACK;", nullptr);
                if (error_message != nullptr) {
                    *error_message = "同步内置设备模板失败：" + sqlite_error(database_);
                }
                return StatusCode::kIoError;
            }
            if (sqlite3_step(update_statement.get()) != SQLITE_DONE) {
                execute_sql_locked("ROLLBACK;", nullptr);
                if (error_message != nullptr) {
                    *error_message = "同步内置设备模板失败：" + sqlite_error(database_);
                }
                return StatusCode::kIoError;
            }

            Statement delete_blocks_statement(database_, "DELETE FROM device_template_read_blocks WHERE template_id = ?;");
            Statement delete_fields_statement(database_, "DELETE FROM device_template_fields WHERE template_id = ?;");
            if (!delete_blocks_statement.ok() ||
                !bind_text(delete_blocks_statement.get(), 1, device_template.template_id) ||
                sqlite3_step(delete_blocks_statement.get()) != SQLITE_DONE ||
                !delete_fields_statement.ok() ||
                !bind_text(delete_fields_statement.get(), 1, device_template.template_id) ||
                sqlite3_step(delete_fields_statement.get()) != SQLITE_DONE) {
                execute_sql_locked("ROLLBACK;", nullptr);
                if (error_message != nullptr) {
                    *error_message = "同步内置设备模板字段失败：" + sqlite_error(database_);
                }
                return StatusCode::kIoError;
            }

            std::string fields_error;
            const auto blocks_status = insert_template_read_blocks_locked(device_template, &fields_error);
            const auto fields_status = is_ok(blocks_status)
                                           ? insert_template_fields_locked(device_template, &fields_error)
                                           : blocks_status;
            if (!is_ok(fields_status)) {
                execute_sql_locked("ROLLBACK;", nullptr);
                if (error_message != nullptr) {
                    *error_message = fields_error.empty() ? "同步内置设备模板字段失败" : fields_error;
                }
                return fields_status;
            }

            // 分组是内置结构；字段显示与历史开关只清理已失效字段的孤立偏好。
            const auto grouping_status = persist_realtime_grouping_preferences_locked(device_template, &fields_error);
            if (!is_ok(grouping_status)) {
                execute_sql_locked("ROLLBACK;", nullptr);
                if (error_message != nullptr) *error_message = fields_error;
                return grouping_status;
            }
            const auto updated_at = time_utils::local_time_string();
            const char* insert_preference_sql =
                "INSERT OR IGNORE INTO config_meta(key,value,updated_at) VALUES(?,?,?);";
            for (const auto& field : device_template.fields) {
                for (const auto& preference : std::vector<std::pair<std::string, bool>>{
                         {"device_template.realtime_display." + device_template.template_id + "." + field.field_key,
                          device_template_field_show_in_realtime(field)},
                         {"device_template.history_enabled." + device_template.template_id + "." + field.field_key,
                          device_template_field_history_enabled(field)},
                     }) {
                    Statement preference_statement(database_, insert_preference_sql);
                    if (!preference_statement.ok() ||
                        !bind_text(preference_statement.get(), 1, preference.first) ||
                        !bind_text(preference_statement.get(), 2, preference.second ? "1" : "0") ||
                        !bind_text(preference_statement.get(), 3, updated_at) ||
                        sqlite3_step(preference_statement.get()) != SQLITE_DONE) {
                        execute_sql_locked("ROLLBACK;", nullptr);
                        if (error_message != nullptr) {
                            *error_message = "补齐内置设备字段偏好失败：" + sqlite_error(database_);
                        }
                        return StatusCode::kIoError;
                    }
                }
            }
            for (const auto& prefix : {
                     "device_template.realtime_display." + device_template.template_id + ".",
                     "device_template.history_enabled." + device_template.template_id + ".",
                 }) {
                Statement cleanup_statement(
                    database_,
                    "DELETE FROM config_meta WHERE substr(key,1,?)=? AND NOT EXISTS ("
                    "SELECT 1 FROM device_template_fields f WHERE f.template_id=? "
                    "AND config_meta.key=? || f.field_key);"
                );
                if (!cleanup_statement.ok() ||
                    !bind_int(cleanup_statement.get(), 1, static_cast<int>(prefix.size())) ||
                    !bind_text(cleanup_statement.get(), 2, prefix) ||
                    !bind_text(cleanup_statement.get(), 3, device_template.template_id) ||
                    !bind_text(cleanup_statement.get(), 4, prefix) ||
                    sqlite3_step(cleanup_statement.get()) != SQLITE_DONE) {
                    execute_sql_locked("ROLLBACK;", nullptr);
                    if (error_message != nullptr) *error_message = "清理内置设备字段孤立偏好失败：" + sqlite_error(database_);
                    return StatusCode::kIoError;
                }
            }
            continue;
        }
        if (exists_status != SQLITE_DONE) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "检查内置设备模板是否存在失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }

        std::string insert_error;
        const auto insert_status = insert_template_locked(device_template, true, &insert_error);
        if (!is_ok(insert_status)) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = "导入设备模板种子失败：" + insert_error;
            }
            return insert_status;
        }
        const auto realtime_status = persist_realtime_display_preferences_locked(device_template, &insert_error);
        const auto history_status = is_ok(realtime_status)
                                        ? persist_history_enabled_preferences_locked(device_template, &insert_error)
                                        : realtime_status;
        const auto grouping_status = is_ok(history_status)
                                         ? persist_realtime_grouping_preferences_locked(device_template, &insert_error)
                                         : history_status;
        if (!is_ok(grouping_status)) {
            execute_sql_locked("ROLLBACK;", nullptr);
            if (error_message != nullptr) {
                *error_message = insert_error.empty()
                                     ? "写入内置设备模板展示配置失败"
                                     : insert_error;
            }
            return grouping_status;
        }
        ++inserted_count;
    }

    const auto commit_status = execute_sql_locked("COMMIT;", error_message);
    if (!is_ok(commit_status)) {
        execute_sql_locked("ROLLBACK;", nullptr);
        return commit_status;
    }
    if (inserted_count > 0) {
        Logger::info("已补齐 C++ 内置设备模板，新增数量=" + std::to_string(inserted_count));
    }
    return StatusCode::kOk;
}

// 在持锁状态下写入模板。
StatusCode DeviceTemplateStore::insert_template_locked(
    const DeviceTemplateDefinition& device_template,
    bool builtin,
    std::string* error_message)
{
    const auto now = time_utils::local_time_string();
    const char* template_sql =
        "INSERT INTO device_templates "
        "(template_id, template_name, description, default_start_register, "
        "device_address_stride, builtin, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    Statement template_statement(database_, template_sql);
    if (!template_statement.ok() ||
        !bind_text(template_statement.get(), 1, device_template.template_id) ||
        !bind_text(template_statement.get(), 2, device_template.display_name) ||
        !bind_text(template_statement.get(), 3, device_template.description) ||
        !bind_int(template_statement.get(), 4, static_cast<int>(device_template.default_start_register)) ||
        !bind_int(template_statement.get(), 5, static_cast<int>(device_template.device_address_stride)) ||
        !bind_int(template_statement.get(), 6, builtin ? 1 : 0) ||
        !bind_text(template_statement.get(), 7, now) ||
        !bind_text(template_statement.get(), 8, now)) {
        if (error_message != nullptr) {
            *error_message = "准备设备模板写入失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    if (sqlite3_step(template_statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "写入设备模板基本信息失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto blocks_status = insert_template_read_blocks_locked(device_template, error_message);
    if (!is_ok(blocks_status)) {
        return blocks_status;
    }
    return insert_template_fields_locked(device_template, error_message);
}

// 在事务中新增或更新单个自定义设备类型。
StatusCode DeviceTemplateStore::upsert_custom_template_locked(
    const DeviceTemplateDefinition& raw_template,
    std::string* error_message)
{
    auto device_template = raw_template;

    bool exists = false;
    bool builtin = false;
    Statement exists_statement(
        database_, "SELECT builtin FROM device_templates WHERE template_id=? LIMIT 1;");
    if (!exists_statement.ok() ||
        !bind_text(exists_statement.get(), 1, device_template.template_id)) {
        if (error_message != nullptr) {
            *error_message = "检查设备类型标识失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    const auto exists_status = sqlite3_step(exists_statement.get());
    if (exists_status == SQLITE_ROW) {
        exists = true;
        builtin = sqlite3_column_int(exists_statement.get(), 0) != 0;
    } else if (exists_status != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "检查设备类型标识失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    if (builtin) {
        if (error_message != nullptr) {
            *error_message = "配置包不能覆盖内置设备类型：" + device_template.template_id;
        }
        return StatusCode::kInvalidArgument;
    }
    if (!exists) {
        const auto insert_status = insert_template_locked(device_template, false, error_message);
        if (!is_ok(insert_status)) return insert_status;
        const auto realtime_status = persist_realtime_display_preferences_locked(device_template, error_message);
        if (!is_ok(realtime_status)) return realtime_status;
        const auto history_status = persist_history_enabled_preferences_locked(device_template, error_message);
        if (!is_ok(history_status)) return history_status;
        const auto grouping_status = persist_realtime_grouping_preferences_locked(device_template, error_message);
        return grouping_status;
    }

    const auto now = time_utils::local_time_string();
    Statement update_statement(
        database_,
        "UPDATE device_templates SET template_name=?,description=?,"
        "default_start_register=?,device_address_stride=?,updated_at=? "
        "WHERE template_id=? AND builtin=0;");
    if (!update_statement.ok() ||
        !bind_text(update_statement.get(), 1, device_template.display_name) ||
        !bind_text(update_statement.get(), 2, device_template.description) ||
        !bind_int(update_statement.get(), 3, static_cast<int>(device_template.default_start_register)) ||
        !bind_int(update_statement.get(), 4, static_cast<int>(device_template.device_address_stride)) ||
        !bind_text(update_statement.get(), 5, now) ||
        !bind_text(update_statement.get(), 6, device_template.template_id) ||
        sqlite3_step(update_statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "覆盖设备类型基本信息失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const std::string preference_prefix =
        "device_template.realtime_display." + device_template.template_id + ".";
    const std::string history_preference_prefix =
        "device_template.history_enabled." + device_template.template_id + ".";
    Statement preference_statement(database_, "DELETE FROM config_meta WHERE substr(key,1,?)=?;");
    Statement history_preference_statement(database_, "DELETE FROM config_meta WHERE substr(key,1,?)=?;");
    Statement fields_statement(database_, "DELETE FROM device_template_fields WHERE template_id=?;");
    Statement blocks_statement(database_, "DELETE FROM device_template_read_blocks WHERE template_id=?;");
    if (!preference_statement.ok() ||
        !bind_int(preference_statement.get(), 1, static_cast<int>(preference_prefix.size())) ||
        !bind_text(preference_statement.get(), 2, preference_prefix) ||
        sqlite3_step(preference_statement.get()) != SQLITE_DONE ||
        !history_preference_statement.ok() ||
        !bind_int(history_preference_statement.get(), 1, static_cast<int>(history_preference_prefix.size())) ||
        !bind_text(history_preference_statement.get(), 2, history_preference_prefix) ||
        sqlite3_step(history_preference_statement.get()) != SQLITE_DONE ||
        !fields_statement.ok() ||
        !bind_text(fields_statement.get(), 1, device_template.template_id) ||
        sqlite3_step(fields_statement.get()) != SQLITE_DONE ||
        !blocks_statement.ok() ||
        !bind_text(blocks_statement.get(), 1, device_template.template_id) ||
        sqlite3_step(blocks_statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "清理设备类型旧区块、字段或枚举失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    const auto blocks_status = insert_template_read_blocks_locked(device_template, error_message);
    if (!is_ok(blocks_status)) return blocks_status;
    const auto fields_status = insert_template_fields_locked(device_template, error_message);
    if (!is_ok(fields_status)) return fields_status;
    const auto realtime_status = persist_realtime_display_preferences_locked(device_template, error_message);
    if (!is_ok(realtime_status)) return realtime_status;
    const auto history_status = persist_history_enabled_preferences_locked(device_template, error_message);
    if (!is_ok(history_status)) return history_status;
    const auto grouping_status = persist_realtime_grouping_preferences_locked(device_template, error_message);
    return grouping_status;
}

// 在当前模板事务中写入稳定读取区块定义。
StatusCode DeviceTemplateStore::insert_template_read_blocks_locked(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    const char* block_sql =
        "INSERT INTO device_template_read_blocks "
        "(template_id,block_key,display_name,function_code,start_offset,register_count,sort_order) "
        "VALUES(?,?,?,?,?,?,?);";
    for (const auto& block : device_template.read_blocks) {
        Statement block_statement(database_, block_sql);
        if (!block_statement.ok() ||
            !bind_text(block_statement.get(), 1, device_template.template_id) ||
            !bind_text(block_statement.get(), 2, block.block_key) ||
            !bind_text(block_statement.get(), 3, block.display_name) ||
            !bind_int(block_statement.get(), 4, static_cast<int>(block.function_code)) ||
            !bind_int(block_statement.get(), 5, static_cast<int>(block.start_offset)) ||
            !bind_int(block_statement.get(), 6, static_cast<int>(block.register_count)) ||
            !bind_int(block_statement.get(), 7, block.sort_order)) {
            if (error_message != nullptr) {
                *error_message = "准备设备模板读取区块写入失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_step(block_statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入设备模板读取区块失败：" + block.block_key +
                                 "，原因=" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }
    return StatusCode::kOk;
}

// 在持锁状态下写入模板字段。
StatusCode DeviceTemplateStore::insert_template_fields_locked(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    const char* field_sql =
        "INSERT INTO device_template_fields "
        "(template_id, field_key, field_name, unit, data_type, parser_id, read_block_key, register_offset, register_count, "
        "scale, value_offset, precision, summary, display_order, invalid_rule_type, invalid_rule_value, "
        "invalid_rule_min, invalid_rule_max, "
        "byte_order, word_order, bit_index) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    for (const auto& field : device_template.fields) {
        const auto parser_id = field.parser_id.empty() ? std::string("scaled_uint16") : field.parser_id;
        Statement field_statement(database_, field_sql);
        if (!field_statement.ok() ||
            !bind_text(field_statement.get(), 1, device_template.template_id) ||
            !bind_text(field_statement.get(), 2, field.field_key) ||
            !bind_text(field_statement.get(), 3, field.display_name) ||
            !bind_text(field_statement.get(), 4, field.unit) ||
            !bind_text(field_statement.get(), 5, field.data_type) ||
            !bind_text(field_statement.get(), 6, parser_id) ||
            !bind_text(field_statement.get(), 7, field.read_block_key) ||
            !bind_int(field_statement.get(), 8, static_cast<int>(field.register_offset)) ||
            !bind_int(field_statement.get(), 9, static_cast<int>(field.register_count)) ||
            !bind_double(field_statement.get(), 10, field.scale) ||
            !bind_double(field_statement.get(), 11, field.value_offset) ||
            !bind_int(field_statement.get(), 12, static_cast<int>(field.precision)) ||
            !bind_int(field_statement.get(), 13, field.summary ? 1 : 0) ||
            !bind_int(field_statement.get(), 14, static_cast<int>(field.display_order)) ||
            !bind_text(field_statement.get(), 15, field.invalid_rule_type) ||
            !bind_double(field_statement.get(), 16, field.invalid_rule_value) ||
            !bind_double(field_statement.get(), 17, field.invalid_rule_min) ||
            !bind_double(field_statement.get(), 18, field.invalid_rule_max) ||
            !bind_text(field_statement.get(), 19, trim_copy(field.byte_order)) ||
            !bind_text(field_statement.get(), 20, trim_copy(field.word_order)) ||
            !bind_int(field_statement.get(), 21, field.bit_index)) {
            if (error_message != nullptr) {
                *error_message = "准备设备模板字段写入失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_step(field_statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入设备模板字段失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    const char* enum_sql =
        "INSERT INTO device_template_enum_items "
        "(template_id,field_key,value,label,sort_order) VALUES(?,?,?,?,?);";
    for (const auto& field : device_template.fields) {
        for (const auto& item : field.enum_items) {
            Statement enum_statement(database_, enum_sql);
            if (!enum_statement.ok() ||
                !bind_text(enum_statement.get(), 1, device_template.template_id) ||
                !bind_text(enum_statement.get(), 2, field.field_key) ||
                !bind_int64(enum_statement.get(), 3, item.value) ||
                !bind_text(enum_statement.get(), 4, trim_copy(item.label)) ||
                !bind_int(enum_statement.get(), 5, item.sort_order) ||
                sqlite3_step(enum_statement.get()) != SQLITE_DONE) {
                if (error_message != nullptr) {
                    *error_message = "写入设备模板枚举项失败：" + field.field_key +
                                     "，原因=" + sqlite_error(database_);
                }
                return StatusCode::kIoError;
            }
        }
    }

    return StatusCode::kOk;
}

// 在模板保存事务内同步每个字段的实时数据展示偏好。
StatusCode DeviceTemplateStore::persist_realtime_display_preferences_locked(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    const char* upsert_sql =
        "INSERT INTO config_meta(key,value,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at;";
    const auto updated_at = time_utils::local_time_string();
    for (const auto& field : device_template.fields) {
        Statement statement(database_, upsert_sql);
        const auto preference_key = "device_template.realtime_display." +
                                    device_template.template_id + "." + field.field_key;
        const auto show_in_realtime = device_template_field_show_in_realtime(field);
        if (!statement.ok() ||
            !bind_text(statement.get(), 1, preference_key) ||
            !bind_text(statement.get(), 2, show_in_realtime ? "1" : "0") ||
            !bind_text(statement.get(), 3, updated_at) ||
            sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入字段实时展示配置失败：" + field.field_key + "，原因=" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }
    return StatusCode::kOk;
}

// 持久化设备类型字段的历史记录偏好。
StatusCode DeviceTemplateStore::persist_history_enabled_preferences_locked(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    const char* upsert_sql =
        "INSERT INTO config_meta(key,value,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at;";
    const auto updated_at = time_utils::local_time_string();
    for (const auto& field : device_template.fields) {
        Statement statement(database_, upsert_sql);
        const auto preference_key = "device_template.history_enabled." +
                                    device_template.template_id + "." + field.field_key;
        const auto enabled = device_template_field_history_enabled(field);
        if (!statement.ok() || !bind_text(statement.get(), 1, preference_key) ||
            !bind_text(statement.get(), 2, enabled ? "1" : "0") ||
            !bind_text(statement.get(), 3, updated_at) || sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入字段历史记录配置失败：" + field.field_key + "，原因=" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }
    return StatusCode::kOk;
}

// 持久化设备类型的实时展示分组偏好。
StatusCode DeviceTemplateStore::persist_realtime_grouping_preferences_locked(
    const DeviceTemplateDefinition& device_template,
    std::string* error_message)
{
    const auto updated_at = time_utils::local_time_string();
    const char* upsert_sql =
        "INSERT INTO config_meta(key,value,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at;";

    nlohmann::json groups = nlohmann::json::array();
    for (const auto& group : device_template.realtime_groups) {
        groups.push_back({
            {"id", group.group_id},
            {"name", group.display_name},
            {"order", group.sort_order},
        });
    }

    const auto enabled_key = "device_template.realtime_grouping_enabled." + device_template.template_id;
    const auto groups_key = "device_template.realtime_groups." + device_template.template_id;
    for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
             {enabled_key, device_template.realtime_grouping_enabled ? "1" : "0"},
             {groups_key, groups.dump()},
         }) {
        Statement statement(database_, upsert_sql);
        if (!statement.ok() || !bind_text(statement.get(), 1, entry.first) ||
            !bind_text(statement.get(), 2, entry.second) || !bind_text(statement.get(), 3, updated_at) ||
            sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入实时展示分组配置失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }

    const auto field_prefix = "device_template.realtime_group_id." + device_template.template_id + ".";
    Statement delete_statement(database_, "DELETE FROM config_meta WHERE substr(key,1,?)=?;");
    if (!delete_statement.ok() ||
        !bind_int(delete_statement.get(), 1, static_cast<int>(field_prefix.size())) ||
        !bind_text(delete_statement.get(), 2, field_prefix) ||
        sqlite3_step(delete_statement.get()) != SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "清理字段实时展示分组引用失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }

    for (const auto& field : device_template.fields) {
        if (field.realtime_group_id.empty()) continue;
        Statement statement(database_, upsert_sql);
        const auto key = field_prefix + field.field_key;
        if (!statement.ok() || !bind_text(statement.get(), 1, key) ||
            !bind_text(statement.get(), 2, field.realtime_group_id) ||
            !bind_text(statement.get(), 3, updated_at) ||
            sqlite3_step(statement.get()) != SQLITE_DONE) {
            if (error_message != nullptr) {
                *error_message = "写入字段实时展示分组失败：" + field.field_key + "，原因=" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
    }
    return StatusCode::kOk;
}

// 在持锁状态下读取指定内置设备类型。
StatusCode DeviceTemplateStore::get_template_builtin_locked(
    const std::string& template_id,
    bool* builtin,
    std::string* error_message) const
{
    if (builtin == nullptr) {
        if (error_message != nullptr) {
            *error_message = "设备模板内置标记输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    const auto trimmed_template_id = trim_copy(template_id);
    if (trimmed_template_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "模板 ID 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    Statement statement(database_, "SELECT builtin FROM device_templates WHERE template_id = ? LIMIT 1;");
    if (!statement.ok() ||
        !bind_text(statement.get(), 1, trimmed_template_id)) {
        if (error_message != nullptr) {
            *error_message = "查询设备模板失败：" + sqlite_error(database_);
        }
        return StatusCode::kIoError;
    }
    const auto step_status = sqlite3_step(statement.get());
    if (step_status == SQLITE_ROW) {
        *builtin = sqlite3_column_int(statement.get(), 0) != 0;
        return StatusCode::kOk;
    }
    if (step_status == SQLITE_DONE) {
        if (error_message != nullptr) {
            *error_message = "设备模板不存在：" + trimmed_template_id;
        }
        return StatusCode::kNotFound;
    }
    if (error_message != nullptr) {
        *error_message = "查询设备模板失败：" + sqlite_error(database_);
    }
    return StatusCode::kIoError;
}

// 在持锁状态下校验模板。
StatusCode DeviceTemplateStore::validate_template_locked(
    const DeviceTemplateDefinition& raw_template,
    bool creating,
    std::string* error_message) const
{
    // 规范化基础字段并累计必填项错误。
    DeviceTemplateDefinition device_template = raw_template;
    device_template.template_id = trim_copy(device_template.template_id);
    device_template.display_name = trim_copy(device_template.display_name);

    std::string errors;
    if (device_template.template_id.empty()) {
        append_error(&errors, "模板 ID 不能为空");
    } else if (!is_safe_custom_id(device_template.template_id)) {
        append_error(&errors, "模板 ID 只能使用小写字母、数字、下划线和中划线");
    }
    if (device_template.display_name.empty()) {
        append_error(&errors, "模板名称不能为空");
    }
    if (device_template.fields.empty()) {
        append_error(&errors, "采集点列表不能为空");
    }

    // 创建时检查设备类型标识是否已存在。
    if (creating && !device_template.template_id.empty()) {
        Statement exists_statement(database_, "SELECT 1 FROM device_templates WHERE template_id = ? LIMIT 1;");
        if (!exists_statement.ok() ||
            !bind_text(exists_statement.get(), 1, device_template.template_id)) {
            if (error_message != nullptr) {
                *error_message = "检查模板 ID 是否重复失败：" + sqlite_error(database_);
            }
            return StatusCode::kIoError;
        }
        if (sqlite3_step(exists_statement.get()) == SQLITE_ROW) {
            append_error(&errors, "模板 ID 已存在：" + device_template.template_id);
        }
    }

    // 校验实时展示分组的标识、名称和顺序唯一性。
    std::set<std::string> realtime_group_ids;
    std::set<int> realtime_group_orders;
    for (std::size_t index = 0; index < device_template.realtime_groups.size(); ++index) {
        const auto& group = device_template.realtime_groups[index];
        const auto group_id = trim_copy(group.group_id);
        const auto group_name = trim_copy(group.display_name);
        const auto label = "实时展示分组[" + std::to_string(index + 1) + "]";
        if (group_id.empty() || !is_safe_custom_id(group_id)) {
            append_error(&errors, label + " ID 只能使用小写字母、数字、下划线和中划线");
        } else if (!realtime_group_ids.insert(group_id).second) {
            append_error(&errors, "实时展示分组 ID 重复：" + group_id);
        }
        if (group_name.empty()) {
            append_error(&errors, label + "名称不能为空");
        } else if (group_name.size() > 100) {
            append_error(&errors, label + "名称不能超过 100 个字节");
        }
        if (group.sort_order < 0 || !realtime_group_orders.insert(group.sort_order).second) {
            append_error(&errors, label + "顺序必须是互不重复的非负整数");
        }
    }
    if (device_template.realtime_grouping_enabled && device_template.realtime_groups.empty()) {
        append_error(&errors, "开启实时展示分组后至少需要一个分组");
    }

    if (!device_template.builtin && !device_template.write_commands.empty()) {
        append_error(&errors, "自定义设备类型仅支持 FC03 / FC04 数据采集，不能定义控制操作");
    }

    // 校验字段标识、解析器、排列、数值规则和展示属性。
    bool has_summary = false;
    std::set<std::string> field_keys;
    for (std::size_t index = 0; index < device_template.fields.size(); ++index) {
        auto field = device_template.fields[index];
        field.field_key = trim_copy(field.field_key);
        field.display_name = trim_copy(field.display_name);
        field.data_type = trim_copy(field.data_type);
        field.parser_id = trim_copy(field.parser_id.empty() ? "scaled_uint16" : field.parser_id);
        field.byte_order = trim_copy(field.byte_order);
        field.word_order = trim_copy(field.word_order);
        const auto label = "采集点[" + std::to_string(index + 1) + "]";

        if (field.field_key.empty()) {
            append_error(&errors, label + " key 不能为空");
        } else {
            if (!is_safe_custom_id(field.field_key)) {
                append_error(&errors, label + " key 只能使用小写字母、数字、下划线和中划线");
            }
            if (!field_keys.insert(field.field_key).second) {
                append_error(&errors, "字段 key 重复：" + field.field_key);
            }
        }
        if (field.display_name.empty()) {
            append_error(&errors, label + " 名称不能为空");
        }
        if (field.register_count == 0) {
            append_error(&errors, label + " 寄存器数量必须大于 0");
        }
        if (!is_supported_data_type(field.data_type)) {
            append_error(&errors, label + " data_type 不支持：" + field.data_type);
        } else if (field.register_count != required_register_count(field.data_type)) {
            append_error(
                &errors,
                label + " data_type 为 " + field.data_type + " 时 register_count 必须为 " +
                    std::to_string(required_register_count(field.data_type)));
        }
        if (!is_supported_parser_id(field.parser_id)) {
            append_error(&errors, label + " parser_id 不支持：" + field.parser_id);
        }
        const auto required_data_type = required_data_type_for_parser_id(field.parser_id);
        if (!required_data_type.empty() && field.data_type != required_data_type) {
            append_error(
                &errors,
                label + " parser_id 为 " + field.parser_id + " 时 data_type 必须为 " + required_data_type);
        }
        const auto byte_order_supported = is_supported_byte_order(field.byte_order);
        const auto word_order_supported = is_supported_word_order(field.word_order);
        if (!byte_order_supported) {
            append_error(&errors, label + " byte_order 不支持：" + field.byte_order);
        }
        if (!word_order_supported) {
            append_error(&errors, label + " word_order 不支持：" + field.word_order);
        }
        const auto is_32_bit_field = field.data_type == "uint32" ||
                                     field.data_type == "int32" ||
                                     field.data_type == "float32";
        if (!is_32_bit_field && byte_order_supported && word_order_supported &&
            (field.byte_order != "big_endian" || field.word_order != "high_word_first")) {
            append_error(
                &errors,
                label + " 16 位或 8 位字段必须使用 byte_order=big_endian 且 word_order=high_word_first");
        }
        if (!std::isfinite(field.scale) || field.scale == 0.0) {
            append_error(&errors, label + " 倍率 scale 必须为非 0 有效数字");
        }
        if (!std::isfinite(field.value_offset)) {
            append_error(&errors, label + " 偏移量必须为有效数字");
        }
        if (field.precision > 6) {
            append_error(&errors, label + " 精度不能超过 6");
        }
        if (field.display_order == 0) {
            append_error(&errors, label + " 显示顺序必须大于 0");
        }
        if (field.summary) {
            has_summary = true;
        }
        const auto realtime_group_id = trim_copy(field.realtime_group_id);
        if (!realtime_group_id.empty() && realtime_group_ids.find(realtime_group_id) == realtime_group_ids.end()) {
            append_error(&errors, label + "引用的实时展示分组不存在：" + realtime_group_id);
        }
        if (device_template.realtime_grouping_enabled &&
            device_template_field_show_in_realtime(field) && realtime_group_id.empty()) {
            append_error(&errors, label + "已启用实时展示，必须选择实时展示分组");
        }

    }
    if (!device_template.fields.empty() && !has_summary) {
        append_error(&errors, "至少需要一个采集点设置为关键数据");
    }

    // 复用读取模型校验，统一检查区块引用和寄存器范围。
    std::string read_model_error;
    const auto read_model_status =
        validate_device_template_read_model(device_template, &read_model_error);
    if (!is_ok(read_model_status)) {
        append_error(&errors, read_model_error);
    }

    // 一次性返回累计的全部配置错误。
    if (!errors.empty()) {
        if (error_message != nullptr) {
            *error_message = errors;
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 在持锁状态下执行SQL。
StatusCode DeviceTemplateStore::execute_sql_locked(const char* sql, std::string* error_message) const
{
    char* raw_error = nullptr;
    const auto status = sqlite3_exec(database_, sql, nullptr, nullptr, &raw_error);
    if (status != SQLITE_OK) {
        const std::string detail = raw_error != nullptr ? raw_error : sqlite_error(database_);
        sqlite3_free(raw_error);
        if (error_message != nullptr) {
            *error_message = detail;
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 在持锁状态下检查数据库是否可用。
bool DeviceTemplateStore::database_available_locked(std::string* error_message) const
{
    if (!initialized_ && database_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "设备模板 SQLite 数据库未初始化";
        }
        return false;
    }
    if (database_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "设备模板 SQLite 数据库未打开";
        }
        return false;
    }
    return true;
}

// 在持锁状态下关闭数据库。
void DeviceTemplateStore::close_database_locked()
{
    if (database_ != nullptr) {
        sqlite3_close(database_);
        database_ = nullptr;
    }
}

}  // namespace edge_controller
