// 持久化设备模板及字段，并初始化内置模板。
// 边界：一致性由类内锁或 SQLite 事务保证，错误通过 StatusCode 返回。

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "model/device_template.h"

struct sqlite3;

namespace edge_controller {

class DeviceTemplateStore {
public:
    // 销毁 DeviceTemplateStore 实例并释放相关资源。
    ~DeviceTemplateStore();

    // 初始化设备模板 SQLite 存储并补齐内置模板。
    StatusCode initialize(
        const std::string& database_path,
        std::string* error_message = nullptr);
    // 读取全部设备模板定义。
    StatusCode load_templates(
        std::vector<DeviceTemplateDefinition>* templates,
        std::string* error_message = nullptr) const;
    // 仅执行结构校验，不写数据库；配置导入用它完成全包预校验。
    StatusCode validate_template_definition(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message = nullptr) const;
    // 在一个事务内创建或完整覆盖指定自定义设备类型，不删除未列出的类型。
    StatusCode upsert_custom_templates(
        const std::vector<DeviceTemplateDefinition>& device_templates,
        std::string* error_message = nullptr);
    // 在一个事务内删除未列于 keep_template_ids 的自定义设备类型。
    StatusCode prune_custom_templates(
        const std::vector<std::string>& keep_template_ids,
        std::string* error_message = nullptr);
    // 新增设备模板定义。
    StatusCode create_template(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message = nullptr);
    // 更新已有设备模板定义。
    StatusCode update_template(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message = nullptr);
    // 更新设备类型字段的实时显示偏好。
    StatusCode update_realtime_display_preference(
        const std::string& template_id,
        const std::string& field_key,
        bool show_in_realtime,
        std::string* error_message = nullptr);
    // 更新设备类型的历史记录偏好。
    StatusCode update_history_enabled_preference(
        const std::string& template_id,
        const std::string& field_key,
        bool history_enabled,
        std::string* error_message = nullptr);
    // 删除设备模板定义。
    StatusCode delete_template(
        const std::string& template_id,
        std::string* error_message = nullptr);
    // 返回设备模板数据库文件路径。
    std::string database_path() const;

private:
    // 在持锁状态下打开数据库。
    StatusCode open_database_locked(const std::string& database_path, std::string* error_message);
    // 在持锁状态下打开并配置设备类型数据库。
    StatusCode configure_database_locked(std::string* error_message);
    // 在持锁状态下初始化数据库结构。
    StatusCode initialize_schema_locked(std::string* error_message);
    // 在持锁状态下初始化内置设备类型。
    StatusCode seed_builtin_templates_locked(std::string* error_message);
    // 在持锁状态下加载模板。
    StatusCode load_templates_locked(
        std::vector<DeviceTemplateDefinition>* templates,
        std::string* error_message) const;
    // 在持锁状态下写入模板。
    StatusCode insert_template_locked(
        const DeviceTemplateDefinition& device_template,
        bool builtin,
        std::string* error_message);
    // 在持锁且已有模板主记录的事务中写入读取区块。
    StatusCode insert_template_read_blocks_locked(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message);
    // 在持锁状态下写入模板字段。
    StatusCode insert_template_fields_locked(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message);
    // 在持锁状态下新增或更新自定义设备类型。
    StatusCode upsert_custom_template_locked(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message);
    // 在当前事务内持久化字段的实时数据展示偏好。
    StatusCode persist_realtime_display_preferences_locked(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message);
    // 在持锁状态下持久化历史记录偏好。
    StatusCode persist_history_enabled_preferences_locked(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message);
    // 在持锁状态下持久化实时分组偏好。
    StatusCode persist_realtime_grouping_preferences_locked(
        const DeviceTemplateDefinition& device_template,
        std::string* error_message);
    // 在持锁状态下读取指定内置设备类型。
    StatusCode get_template_builtin_locked(
        const std::string& template_id,
        bool* builtin,
        std::string* error_message) const;
    // 在持锁状态下校验模板。
    StatusCode validate_template_locked(
        const DeviceTemplateDefinition& device_template,
        bool creating,
        std::string* error_message) const;
    // 在持锁状态下执行SQL。
    StatusCode execute_sql_locked(const char* sql, std::string* error_message) const;
    // 在持锁状态下检查数据库是否可用。
    bool database_available_locked(std::string* error_message) const;
    // 在持锁状态下关闭数据库。
    void close_database_locked();

    // 模板定义包含多张关联表，读写期间独占连接以避免观察到半套字段配置。
    mutable std::mutex mutex_;
    ::sqlite3* database_{nullptr};
    std::string database_path_;
    bool initialized_{false};
};

}  // namespace edge_controller
