#include "application/service/runtime_config_compiler.h"
#include "application/service/backend_service_internal.h"
#include "data/model/device_derivation.h"
#include <cstdlib>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#ifndef EDGE_CONTROLLER_PACKAGE_VERSION
#error "EDGE_CONTROLLER_PACKAGE_VERSION must be provided by the controller build"
#endif
namespace edge_controller {
namespace {
// 优先读取集中式部署变量，空值时使用正式安装目录。
std::string environment_or_default(const char* name, const char* fallback)
{
    const auto* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : fallback;
}

// SQLite 只覆盖持久化业务设置；进程版本和目录遵循构建、systemd 部署契约。
SystemConfig make_default_runtime_config()
{
    SystemConfig config;
    config.project_name = "edge-controller";
    config.project_version = EDGE_CONTROLLER_PACKAGE_VERSION;
    config.site_id = "default-site";
    config.data_directory = environment_or_default("EDGE_CONTROLLER_DATA_DIR", "/opt/edge-controller/data");
    config.log_directory = environment_or_default("EDGE_CONTROLLER_LOG_DIR", "/opt/edge-controller/log");
    config.log_level = LogLevel::kInfo;
    config.default_poll_interval_ms = 1000;
    config.max_channel_reconnect_interval_ms = 5000;
    return config;
}

// 收集当前启用且参与采集的通道标识。
std::vector<ChannelId> enabled_collection_channel_ids(
    const SystemConfig& config,
    const TopologyManager& topology_manager)
{
    std::vector<ChannelId> channel_ids;
    std::unordered_set<ChannelId> seen;

    for (const auto& master : config.master_nodes) {
        if (!master.enabled ||
            (master.protocol != MasterProtocol::kModbusRtu &&
             master.protocol != MasterProtocol::kModbusTcp)) {
            continue;
        }

        const auto* channel = backend_internal::find_channel_config_in_list(config.channels, master.channel_id);
        if (channel == nullptr || !channel->enabled) {
            continue;
        }

        bool has_enabled_device = false;
        for (const auto* device : topology_manager.get_devices_by_master(master.master_id)) {
            if (device != nullptr && device->enabled) {
                has_enabled_device = true;
                break;
            }
        }
        if (!has_enabled_device || seen.find(master.channel_id) != seen.end()) {
            continue;
        }

        channel_ids.push_back(master.channel_id);
        seen.insert(master.channel_id);
    }

    return channel_ids;
}

// 按通道类型筛选通道标识列表。
std::vector<ChannelId> filter_channel_ids_by_type(
    const SystemConfig& config,
    const std::vector<ChannelId>& channel_ids,
    ChannelType channel_type)
{
    std::vector<ChannelId> filtered;
    for (const auto& channel_id : channel_ids) {
        const auto* channel = backend_internal::find_channel_config_in_list(config.channels, channel_id);
        if (channel != nullptr && channel->channel_type == channel_type) {
            filtered.push_back(channel_id);
        }
    }
    return filtered;
}

}  // namespace
StatusCode RuntimeConfigCompiler::prepare(
    PreparedRuntimeConfig* prepared,
    std::vector<std::string>* errors)
{
    if (prepared == nullptr) {
        if (errors != nullptr) {
            errors->push_back("运行配置准备参数为空");
        }
        return StatusCode::kInvalidArgument;
    }

    PreparedRuntimeConfig next;
    auto next_config = make_default_runtime_config();
    // 先在临时对象中完成 SQLite 加载、设备推导、拓扑构建和通道初始化；全部成功后再切换到运行态。
    std::vector<ChannelConfig> sqlite_channels;
    std::string channels_error;
    const auto channels_status = store_.load_channels(&sqlite_channels, &channels_error);
    if (!is_ok(channels_status)) {
        if (errors != nullptr) {
            errors->push_back(channels_error.empty() ? "加载 SQLite channels 失败" : channels_error);
        }
        return channels_status;
    }
    next_config.channels = std::move(sqlite_channels);

    std::vector<MasterNodeConfig> sqlite_masters;
    std::string masters_error;
    const auto masters_status = store_.load_masters(&sqlite_masters, &masters_error);
    if (!is_ok(masters_status)) {
        if (errors != nullptr) {
            errors->push_back(masters_error.empty() ? "加载 SQLite masters 失败" : masters_error);
        }
        return masters_status;
    }
    next_config.master_nodes = std::move(sqlite_masters);

    SystemSettings sqlite_settings;
    std::string settings_error;
    const auto settings_status = store_.load_or_initialize_system_settings(
        &sqlite_settings,
        &settings_error);
    if (!is_ok(settings_status)) {
        if (errors != nullptr) {
            errors->push_back(settings_error.empty() ? "加载 SQLite system_settings 失败" : settings_error);
        }
        return settings_status;
    }
    next_config.settings = std::move(sqlite_settings);

    TimeSettings sqlite_time_settings;
    std::string time_settings_error;
    const auto time_settings_status = store_.load_or_initialize_time_settings(
        &sqlite_time_settings,
        &time_settings_error);
    if (!is_ok(time_settings_status)) {
        if (errors != nullptr) {
            errors->push_back(time_settings_error.empty() ? "加载 SQLite time_settings 失败" : time_settings_error);
        }
        return time_settings_status;
    }
    next_config.time_settings = std::move(sqlite_time_settings);

    NetworkSettings sqlite_network_settings;
    bool network_settings_explicitly_configured = false;
    std::string network_settings_error;
    const auto network_settings_status = store_.load_or_initialize_network_settings_state(
        &sqlite_network_settings,
        &network_settings_explicitly_configured,
        &network_settings_error);
    if (!is_ok(network_settings_status)) {
        if (errors != nullptr) {
            errors->push_back(
                network_settings_error.empty() ? "加载 SQLite network_settings 失败" : network_settings_error);
        }
        return network_settings_status;
    }
    next_config.network_settings = std::move(sqlite_network_settings);
    next_config.network_settings_explicitly_configured = network_settings_explicitly_configured;

    MqttSettings sqlite_mqtt_settings;
    std::string mqtt_settings_error;
    const auto mqtt_settings_status = store_.load_or_initialize_mqtt_settings(
        &sqlite_mqtt_settings,
        &mqtt_settings_error);
    if (!is_ok(mqtt_settings_status)) {
        if (errors != nullptr) {
            errors->push_back(
                mqtt_settings_error.empty() ? "加载 SQLite mqtt_settings 失败" : mqtt_settings_error);
        }
        return mqtt_settings_status;
    }
    next_config.mqtt_settings = std::move(sqlite_mqtt_settings);

    std::vector<DeviceConfig> synced_devices;
    // 设备不再由独立配置维护，而是由主控采集块和设备模板自动推导。
    const auto sync_status = derive_devices_from_masters(next_config.master_nodes, synced_devices, errors);
    if (!is_ok(sync_status)) {
        return sync_status;
    }
    std::vector<DeviceAlias> aliases;
    std::string alias_error;
    const auto alias_status = store_.load_device_aliases(&aliases, &alias_error);
    if (!is_ok(alias_status)) {
        if (errors != nullptr) {
            errors->push_back(alias_error.empty() ? "加载设备显示名称失败" : alias_error);
        }
        return alias_status;
    }
    std::unordered_map<DeviceId, std::string> alias_by_device;
    for (const auto& alias : aliases) {
        if (!alias.device_id.empty() && !alias.display_name.empty()) {
            alias_by_device[alias.device_id] = alias.display_name;
        }
    }
    for (auto& device : synced_devices) {
        const auto alias = alias_by_device.find(device.device_id);
        if (alias != alias_by_device.end()) device.device_name = alias->second;
    }
    next_config.devices = std::move(synced_devices);
    std::string topology_error;
    const auto topology_status = next.topology_manager.build(next_config, &topology_error);
    if (!is_ok(topology_status)) {
        if (errors != nullptr && !topology_error.empty()) {
            errors->push_back(topology_error);
        }
        return topology_status;
    }

    std::string channel_error;
    const auto channel_init_status = next.channel_manager.initialize(next_config.channels, &channel_error);
    if (!is_ok(channel_init_status)) {
        if (errors != nullptr && !channel_error.empty()) {
            errors->push_back(channel_error);
        }
        return channel_init_status;
    }

    const auto active_collection_channel_ids =
        enabled_collection_channel_ids(next_config, next.topology_manager);
    const auto tcp_collection_channel_ids = filter_channel_ids_by_type(
        next_config,
        active_collection_channel_ids,
        ChannelType::kModbusTcp);
    const auto channel_open_status = next.channel_manager.open_enabled_channels(
        tcp_collection_channel_ids,
        &next.channel_open_summary);
    if (!is_ok(channel_open_status)) {
        if (errors != nullptr) {
            errors->push_back("通道管理器初始化失败");
        }
        return channel_open_status;
    }

    next.system_config = std::move(next_config);
    *prepared = std::move(next);
    prepared->topology_manager.rebind_system_config(prepared->system_config);
    return StatusCode::kOk;
}

}  // namespace edge_controller
