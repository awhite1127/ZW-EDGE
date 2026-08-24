// 管理配置 SQLite 的连接、事务、结构版本以及各配置域读写。
// 边界：一致性由类内锁或 SQLite 事务保证，错误通过 StatusCode 返回。

#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "common/status_code.h"
#include "model/channel_config.h"
#include "model/device_config.h"
#include "model/master_node_config.h"
#include "model/modbus_server.h"
#include "model/mqtt_settings.h"
#include "model/network_settings.h"
#include "model/system_settings.h"
#include "model/time_settings.h"
#include "model/web_auth.h"

struct sqlite3;

namespace edge_controller {

class ConfigImportTransaction;

class ConfigStore {
public:
    // 销毁 ConfigStore 实例并释放相关资源。
    ~ConfigStore();

    // 初始化配置 SQLite 存储并创建必要表结构。
    StatusCode initialize(
        const std::string& database_path,
        std::string* error_message = nullptr);
    // 读取系统显示设置。
    StatusCode load_system_settings(
        SystemSettings* settings,
        std::string* error_message = nullptr) const;
    // 读取系统显示设置，不存在时写入默认值。
    StatusCode load_or_initialize_system_settings(
        SystemSettings* settings,
        std::string* error_message = nullptr);
    // 保存系统显示设置。
    StatusCode save_system_settings(
        const SystemSettings& settings,
        std::string* error_message = nullptr);
    // 读取时间配置。
    StatusCode load_time_settings(TimeSettings* settings, std::string* error_message = nullptr) const;
    // 干净数据库首次初始化固定使用产品默认 Asia/Shanghai。
    StatusCode load_or_initialize_time_settings(
        TimeSettings* settings,
        std::string* error_message = nullptr);
    // 保存时间设置。
    StatusCode save_time_settings(const TimeSettings& settings, std::string* error_message = nullptr);
    // 读取通道配置列表。
    StatusCode load_channels(
        std::vector<ChannelConfig>* channels,
        std::string* error_message = nullptr) const;
    // 保存通道配置列表。
    StatusCode save_channels(
        const std::vector<ChannelConfig>& channels,
        std::string* error_message = nullptr);
    // 读取主站配置列表。
    StatusCode load_masters(
        std::vector<MasterNodeConfig>* masters,
        std::string* error_message = nullptr) const;
    // 保存主站配置列表。
    StatusCode save_masters(
        const std::vector<MasterNodeConfig>& masters,
        std::string* error_message = nullptr);
    // 加载设备显示名称别名。
    StatusCode load_device_aliases(
        std::vector<DeviceAlias>* aliases,
        std::string* error_message = nullptr) const;
    // display_name 为空时删除别名，恢复系统推导名称。
    StatusCode save_device_alias(
        const DeviceId& device_id,
        const std::string& display_name,
        TimestampMs updated_at_ms,
        std::string* error_message = nullptr);
    // 在一个事务内保存或清除多条设备别名。
    StatusCode save_device_aliases_batch(
        const std::vector<DeviceAlias>& aliases,
        std::string* error_message = nullptr);
    // 读取网络配置。
    StatusCode load_network_settings(
        NetworkSettings* settings,
        std::string* error_message = nullptr) const;
    // 读取网络配置，不存在时写入默认值。
    StatusCode load_or_initialize_network_settings(
        NetworkSettings* settings,
        std::string* error_message = nullptr);
    // 读取网络配置及其显式确认状态；旧库已有网络行但缺少标记时按已配置迁移。
    StatusCode load_or_initialize_network_settings_state(
        NetworkSettings* settings,
        bool* explicitly_configured,
        std::string* error_message = nullptr);
    // 保存网络配置。
    StatusCode save_network_settings(
        const NetworkSettings& settings,
        std::string* error_message = nullptr);
    // 在同一 SQLite 事务内保存网络配置和显式确认状态。
    StatusCode save_network_settings_state(
        const NetworkSettings& settings,
        bool explicitly_configured,
        std::string* error_message = nullptr);
    // 读取 MQTT 北向配置。
    StatusCode load_mqtt_settings(
        MqttSettings* settings,
        std::string* error_message = nullptr) const;
    // 读取 MQTT 北向配置，不存在时写入默认值。
    StatusCode load_or_initialize_mqtt_settings(
        MqttSettings* settings,
        std::string* error_message = nullptr);
    // 保存 MQTT 北向配置。密码沿用现有内网部署兼容存储方式，禁止通过查询接口或日志回显。
    StatusCode save_mqtt_settings(
        const MqttSettings& settings,
        std::string* error_message = nullptr);
    // 读取北向 Modbus Server 持久化配置；监听生命周期由 ModbusTcpServer 管理。
    StatusCode load_modbus_server_settings(
        ModbusServerSettings* settings,
        std::string* error_message = nullptr) const;
    // 首次访问时持久化默认关闭的北向 Modbus Server 配置。
    StatusCode load_or_initialize_modbus_server_settings(
        ModbusServerSettings* settings,
        std::string* error_message = nullptr);
    // 保存 Modbus 服务设置。
    StatusCode save_modbus_server_settings(
        const ModbusServerSettings& settings,
        std::string* error_message = nullptr);
    // 加载全部 Modbus 寄存器映射。
    StatusCode load_modbus_register_mappings(
        std::vector<ModbusRegisterMapping>* mappings,
        std::string* error_message = nullptr) const;
    // 创建 Modbus 寄存器映射。
    StatusCode create_modbus_register_mapping(
        const ModbusRegisterMapping& mapping,
        std::string* error_message = nullptr);
    // 更新 Modbus 寄存器映射。
    StatusCode update_modbus_register_mapping(
        const ModbusRegisterMapping& mapping,
        std::string* error_message = nullptr);
    // 删除 Modbus 寄存器映射。
    StatusCode delete_modbus_register_mapping(
        const std::string& mapping_id,
        std::string* error_message = nullptr);
    // 在一个 SQLite 事务内完整替换映射集合，用于热应用失败回滚、配置导入和恢复出厂。
    StatusCode replace_modbus_register_mappings(
        const std::vector<ModbusRegisterMapping>& mappings,
        std::string* error_message = nullptr);
    // 读取或初始化默认 Web 管理员账号。
    StatusCode load_or_initialize_web_admin(
        WebUser* user,
        std::string* error_message = nullptr);
    // 初始化内置 Web 账户，已存在账户不会被覆盖。
    StatusCode load_or_initialize_web_users(std::string* error_message = nullptr);
    // 读取 Web 管理员认证状态。
    StatusCode get_web_auth_status(
        WebAuthStatus* status,
        std::string* error_message = nullptr);
    // 校验 Web 登录用户名和密码。
    StatusCode verify_web_login(
        const WebLoginRequest& request,
        WebLoginResult* result,
        std::string* error_message = nullptr);
    // 修改固定 Web 账户自己的密码。
    StatusCode change_web_password(
        const WebPasswordChangeRequest& request,
        WebPasswordChangeResult* result,
        std::string* error_message = nullptr);
    // 管理员验证自身密码后设置固定只读账户密码。
    StatusCode set_web_viewer_password(
        const WebViewerPasswordSetRequest& request,
        WebPasswordChangeResult* result,
        std::string* error_message = nullptr);
    // 列出 Web 用户。
    StatusCode list_web_users(
        std::vector<WebUserView>* users,
        std::string* error_message = nullptr) const;
    // 创建 Web 用户。
    StatusCode create_web_user(
        const WebUserCreateRequest& request,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 更新 Web 用户显示名、角色和启用状态。
    StatusCode update_web_user(
        const WebUserUpdateRequest& request,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 重置 Web 用户密码。
    StatusCode reset_web_user_password(
        const WebUserPasswordResetRequest& request,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 仅供板端 root 维护入口恢复已有超级管理员密码；调用方负责执行身份校验和审计。
    StatusCode recover_super_admin_password(
        const std::string& username,
        const std::string& new_password,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 恢复内置 admin 和 user 账户的出厂密码与角色，并清理额外账号。
    StatusCode reset_web_user_passwords(std::string* error_message = nullptr);
    // 读取首次部署管理员免密入口是否仍可使用；旧库缺少标记时按不可用处理。
    StatusCode load_first_boot_admin_entry_available(
        bool* available,
        std::string* error_message = nullptr) const;
    // 原子消费首次部署管理员免密入口，仅首次调用可返回 consumed=true。
    StatusCode consume_first_boot_admin_entry(
        bool* consumed,
        std::string* error_message = nullptr);
    // 恢复出厂后重新开放首次部署管理员免密入口。
    StatusCode reset_first_boot_admin_entry(std::string* error_message = nullptr);
    // 写入配置存储元信息。
    StatusCode set_meta(
        const std::string& key,
        const std::string& value,
        std::string* error_message = nullptr);
    // 返回配置数据库文件路径。
    std::string database_path() const;

private:
    // 在持锁状态下打开数据库。
    StatusCode open_database_locked(const std::string& database_path, std::string* error_message);
    // 在持锁状态下打开并配置 SQLite 数据库。
    StatusCode configure_database_locked(std::string* error_message);
    // 在持锁状态下初始化数据库结构。
    StatusCode initialize_schema_locked(std::string* error_message);
    // 在持锁状态下加载系统设置。
    StatusCode load_system_settings_locked(
        SystemSettings* settings,
        std::string* error_message) const;
    // 在持锁状态下保存系统设置。
    StatusCode save_system_settings_locked(
        const SystemSettings& settings,
        std::string* error_message);
    // 在持锁状态下加载时间设置。
    StatusCode load_time_settings_locked(TimeSettings* settings, std::string* error_message) const;
    // 在持锁状态下保存时间设置。
    StatusCode save_time_settings_locked(const TimeSettings& settings, std::string* error_message);
    // 在持锁状态下读取全部通道配置。
    StatusCode load_channels_locked(
        std::vector<ChannelConfig>* channels,
        std::string* error_message) const;
    // 在持锁状态下保存全部通道配置。
    StatusCode save_channels_locked(
        const std::vector<ChannelConfig>& channels,
        std::string* error_message);
    // 在持锁状态下读取全部主站配置。
    StatusCode load_masters_locked(
        std::vector<MasterNodeConfig>* masters,
        std::string* error_message) const;
    // 在持锁状态下保存全部主站配置。
    StatusCode save_masters_locked(
        const std::vector<MasterNodeConfig>& masters,
        std::string* error_message);
    // 在持锁状态下加载设备显示名称别名。
    StatusCode load_device_aliases_locked(
        std::vector<DeviceAlias>* aliases,
        std::string* error_message) const;
    // 在持锁状态下保存设备显示名称别名。
    StatusCode save_device_alias_locked(
        const DeviceId& device_id,
        const std::string& display_name,
        TimestampMs updated_at_ms,
        std::string* error_message);
    // 在持锁状态下加载网络设置。
    StatusCode load_network_settings_locked(
        NetworkSettings* settings,
        std::string* error_message) const;
    // 在持锁状态下保存网络设置。
    StatusCode save_network_settings_locked(
        const NetworkSettings& settings,
        std::string* error_message);
    // 在持锁状态下加载MQTT设置。
    StatusCode load_mqtt_settings_locked(
        MqttSettings* settings,
        std::string* error_message) const;
    // 在持锁状态下保存MQTT设置。
    StatusCode save_mqtt_settings_locked(
        const MqttSettings& settings,
        std::string* error_message);
    // 在持锁状态下加载 Modbus 服务设置。
    StatusCode load_modbus_server_settings_locked(
        ModbusServerSettings* settings,
        std::string* error_message) const;
    // 在持锁状态下保存 Modbus 服务设置。
    StatusCode save_modbus_server_settings_locked(
        const ModbusServerSettings& settings,
        std::string* error_message);
    // 在持锁状态下加载 Modbus 寄存器映射。
    StatusCode load_modbus_register_mappings_locked(
        std::vector<ModbusRegisterMapping>* mappings,
        std::string* error_message) const;
    // 在持锁状态下加载 Web 用户。
    StatusCode load_web_user_locked(
        const std::string& username,
        WebUser* user,
        std::string* error_message) const;
    // 在持锁状态下保存 Web 用户。
    StatusCode save_web_user_locked(
        const WebUser& user,
        std::string* error_message);
    // 在持锁状态下初始化默认 Web 用户。
    StatusCode initialize_default_web_user_locked(
        const std::string& username,
        const std::string& role,
        const std::string& default_password,
        WebUser* user,
        std::string* error_message);
    // 在持锁状态下写入配置数据库元数据。
    StatusCode set_meta_locked(
        const std::string& key,
        const std::string& value,
        std::string* error_message);
    // 在持锁状态下执行SQL。
    StatusCode execute_sql_locked(const char* sql, std::string* error_message) const;
    // 在持锁状态下判断数据库句柄是否已打开。
    bool database_open_locked(std::string* error_message) const;
    // 在持锁状态下判断数据库是否可正常读写。
    bool database_ready_locked(std::string* error_message) const;
    // 在持锁状态下关闭数据库。
    void close_database_locked();

    friend class ConfigImportTransaction;

    // 单个 SQLite 连接由本锁串行使用；公开方法不得返回依赖 statement 或连接生命周期的引用。
    mutable std::mutex mutex_;
    ::sqlite3* database_{nullptr};
    std::string database_path_;
    bool initialized_{false};
};

}  // namespace edge_controller
