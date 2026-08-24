// 配置包持久化事务：让配置、设备模板和告警仓储在同一 SQLite 连接/事务中提交。
#pragma once

#include <string>
#include <vector>

#include "common/status_code.h"
#include "model/alarm.h"
#include "model/channel_config.h"
#include "model/device_template.h"
#include "model/master_node_config.h"
#include "model/modbus_server.h"
#include "model/mqtt_settings.h"
#include "model/network_settings.h"
#include "model/system_settings.h"
#include "model/time_settings.h"

namespace edge_controller {

class AlarmStore;
class ConfigStore;
class DeviceTemplateStore;

// 已完成业务校验的配置导入持久化载荷。
struct ConfigImportPersistencePayload {
    SystemSettings system_settings;
    TimeSettings time_settings;
    NetworkSettings network_settings;
    bool network_settings_explicitly_configured{true};
    MqttSettings mqtt_settings;
    std::vector<DeviceTemplateDefinition> custom_device_types;
    std::vector<ChannelConfig> channels;
    std::vector<MasterNodeConfig> masters;
    bool replace_alarm_data{false};
    std::vector<AlarmRule> alarm_rules;
    std::vector<AlarmRuntimeState> alarm_runtime_states;
    ModbusServerSettings modbus_server_settings;
    std::vector<ModbusRegisterMapping> modbus_register_mappings;
};

class ConfigImportTransaction {
public:
    // 三个仓储必须指向同一 edge-config.db。成功时整套提交；任一步失败时整套回滚。
    static StatusCode apply(
        ConfigStore& config_store,
        DeviceTemplateStore& device_template_store,
        AlarmStore& alarm_store,
        const ConfigImportPersistencePayload& payload,
        std::string* error_message = nullptr);
};

}  // namespace edge_controller
