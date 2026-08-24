// 告警规则 SQLite 仓库接口。
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "model/alarm.h"

struct sqlite3;

namespace edge_controller {

class ConfigImportTransaction;

struct AlarmRuntimeStateKey {
    DeviceId device_id;
    std::string point_key;
};

class AlarmStore {
public:
    // 销毁 AlarmStore 实例并释放相关资源。
    ~AlarmStore();

    // 初始化。
    StatusCode initialize(const std::string& database_path, std::string* error_message = nullptr);
    // 新增或更新规则。
    StatusCode upsert_rule(const AlarmRule& rule, std::string* error_message = nullptr);
    // 获取规则。
    StatusCode get_rule(const DeviceId& device_id, const std::string& point_key, std::optional<AlarmRule>* rule, std::string* error_message = nullptr) const;
    // 列出规则。
    StatusCode list_rules(std::vector<AlarmRule>* rules, std::string* error_message = nullptr) const;
    // 列出规则按设备。
    StatusCode list_rules_by_device(const DeviceId& device_id, std::vector<AlarmRule>* rules, std::string* error_message = nullptr) const;
    // 删除规则。
    StatusCode delete_rule(const DeviceId& device_id, const std::string& point_key, std::string* error_message = nullptr);
    // 清空规则。
    StatusCode clear_rules(std::string* error_message = nullptr);

    // 新增或更新报警运行状态。
    StatusCode upsert_runtime_state(const AlarmRuntimeState& state, std::string* error_message = nullptr);
    // 获取指定点位的报警运行状态。
    StatusCode get_runtime_state(const DeviceId& device_id, const std::string& point_key, std::optional<AlarmRuntimeState>* state, std::string* error_message = nullptr) const;
    // 列出全部告警运行状态。
    StatusCode list_runtime_states(std::vector<AlarmRuntimeState>* states, std::string* error_message = nullptr) const;
    // 列出当前处于活动状态的告警。
    StatusCode list_active_runtime_states(std::vector<AlarmRuntimeState>* states, std::string* error_message = nullptr) const;
    // 删除指定点位的报警运行状态。
    StatusCode delete_runtime_state(const DeviceId& device_id, const std::string& point_key, std::string* error_message = nullptr);
    // 在单个 SQLite 事务中批量写入/删除运行态，同类操作复用同一 prepared statement。
    StatusCode apply_runtime_state_batch(
        const std::vector<AlarmRuntimeState>& upserts,
        const std::vector<AlarmRuntimeStateKey>& deletes,
        std::string* error_message = nullptr);
    // 清空全部告警运行状态。
    StatusCode clear_runtime_states(std::string* error_message = nullptr);
    // 在单个 SQLite 事务内完整替换告警运行状态，用于配置导入失败后的无损恢复。
    StatusCode replace_runtime_states(
        const std::vector<AlarmRuntimeState>& states,
        std::string* error_message = nullptr);

private:
    // 在调用方已开启的共享事务内完整替换导入携带的告警规则和运行状态。
    StatusCode replace_alarm_data_for_import_locked(
        const std::vector<AlarmRule>& rules,
        const std::vector<AlarmRuntimeState>& states,
        std::string* error_message);
    // 在持锁状态下执行。
    StatusCode execute_locked(const char* sql, std::string* error_message) const;
    // 在持锁状态下初始化数据库结构。
    StatusCode initialize_schema_locked(std::string* error_message);
    // 在持锁状态下检查数据库是否可用。
    bool available_locked(std::string* error_message) const;
    // 在持锁状态下关闭。
    void close_locked();

    friend class ConfigImportTransaction;

    // 告警规则和运行态共用数据库连接，各公开操作以本锁包围完整事务。
    mutable std::mutex mutex_;
    ::sqlite3* database_{nullptr};
    std::string database_path_;
};

}  // namespace edge_controller
