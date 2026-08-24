// 组装采集配置、实时状态和设备显示名称查询。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "service/backend_service.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "common/string_utils.h"
#include "common/time_utils.h"
#include "service/backend_service_internal.h"

namespace edge_controller {

// 获取当前配置摘要。
ConfigSummary BackendService::get_config_summary() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    ConfigSummary summary;
    summary.project_name = system_config_.project_name;
    summary.project_version = system_config_.project_version;
    summary.site_id = system_config_.site_id;
    summary.default_poll_interval_ms = system_config_.default_poll_interval_ms;
    summary.channel_count = system_config_.channels.size();
    summary.master_count = system_config_.master_nodes.size();
    summary.device_count = system_config_.devices.size();
    summary.device_templates = device_templates();
    return summary;
}

// 获取最近配置重载提示。
std::vector<std::string> BackendService::get_config_reload_notes() const
{
    return {
        "系统基础设置来自 SQLite system_settings，重新加载配置不会读取 JSON 配置文件。",
        "通道配置来自 SQLite channels，保存后会重建通道并重新打开启用通道。",
        "主控配置来自 SQLite masters，保存后会重建采集目标和拓扑。",
        "设备列表由 SQLite masters 和设备模板运行时推导，重新加载不会读取 JSON 配置文件。",
    };
}

// 获取系统显示设置。
SystemSettings BackendService::get_system_settings() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return system_config_.settings;
}

// 获取网络配置。
NetworkSettings BackendService::get_network_settings() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return system_config_.network_settings;
}

// 获取通道配置列表。
std::vector<ChannelConfig> BackendService::get_channels() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return system_config_.channels;
}

// 枚举系统串口候选项。
StatusCode BackendService::list_serial_ports(
    std::vector<SerialPortInfo>* ports,
    std::string* error_message) const
{
    return serial_port_enumerator_.enumerate(ports, error_message);
}

// 获取主站配置列表。
std::vector<MasterNodeConfig> BackendService::get_masters() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return system_config_.master_nodes;
}

// 获取自动推导设备列表。
std::vector<DeviceConfig> BackendService::get_devices() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return system_config_.devices;
}

// 更新设备显示名称。
StatusCode BackendService::update_device_display_name(
    const DeviceId& device_id,
    const std::string& display_name,
    DeviceConfig* result,
    std::string* error_message)
{
    if (result == nullptr || device_id.empty()) {
        if (error_message != nullptr) *error_message = "设备名称更新参数不完整";
        return StatusCode::kInvalidArgument;
    }
    const auto normalized_name = backend_internal::trim_copy(display_name);
    if (utf8_character_count(normalized_name) > 40) {
        if (error_message != nullptr) *error_message = "自定义设备名称不能超过 40 个字符";
        return StatusCode::kInvalidArgument;
    }

    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto found = std::find_if(
        system_config_.devices.begin(), system_config_.devices.end(),
        [&](const DeviceConfig& device) { return device.device_id == device_id; });
    if (found == system_config_.devices.end()) {
        if (error_message != nullptr) *error_message = "未找到设备，请刷新设备列表后重试";
        return StatusCode::kNotFound;
    }

    std::string save_error;
    const auto save_status = config_store_.save_device_alias(
        device_id, normalized_name, time_utils::system_now_ms(), &save_error);
    if (!is_ok(save_status)) {
        if (error_message != nullptr) {
            *error_message = save_error.empty() ? "保存设备名称失败" : save_error;
        }
        return save_status;
    }

    found->device_name = normalized_name.empty() ? found->generated_name : normalized_name;
    topology_manager_.rebind_system_config(system_config_);
    if (auto status = data_store_.get_device_status(device_id); status.has_value()) {
        status->device_name = found->device_name;
        data_store_.update_device_status(*status);
    }
    synchronize_alarm_topology_locked("设备名称更新");
    *result = *found;
    append_event(
        "info", "device_name", device_id,
        normalized_name.empty() ? "设备名称已恢复为系统推导名称" : "设备显示名称已更新",
        found->device_name, time_utils::system_now_ms());
    return StatusCode::kOk;
}

// 更新设备显示名称批量。
StatusCode BackendService::update_device_display_names_batch(
    const std::vector<DeviceDisplayNameBatchItem>& items,
    DeviceDisplayNameBatchResult* result,
    std::string* error_message)
{
    if (result == nullptr || items.empty()) {
        if (error_message != nullptr) *error_message = "批量设备名称更新参数不完整";
        return StatusCode::kInvalidArgument;
    }
    *result = DeviceDisplayNameBatchResult{};

    struct PendingUpdate {
        std::size_t device_index{0};
        std::string display_name;
    };

    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    std::vector<PendingUpdate> pending;
    std::vector<DeviceAlias> aliases;
    std::unordered_set<std::string> seen_device_ids;
    const auto updated_at_ms = time_utils::system_now_ms();
    bool all_restore = true;

    for (const auto& item : items) {
        const auto device_id = backend_internal::trim_copy(item.device_id);
        const auto display_name = backend_internal::trim_copy(item.display_name);
        auto fail = [&](const std::string& message) {
            result->failures.push_back({device_id, message});
        };
        if (device_id.empty()) {
            fail("缺少设备 ID");
            continue;
        }
        if (!seen_device_ids.insert(device_id).second) {
            fail("批量请求中设备 ID 重复");
            continue;
        }
        if (utf8_character_count(display_name) > 40) {
            fail("自定义设备名称不能超过 40 个字符");
            continue;
        }
        const auto found = std::find_if(
            system_config_.devices.begin(), system_config_.devices.end(),
            [&](const DeviceConfig& device) { return device.device_id == device_id; });
        if (found == system_config_.devices.end()) {
            fail("未找到设备，请刷新设备列表后重试");
            continue;
        }
        const auto index = static_cast<std::size_t>(
            std::distance(system_config_.devices.begin(), found));
        pending.push_back({index, display_name});
        aliases.push_back({device_id, display_name, updated_at_ms});
        if (!display_name.empty()) all_restore = false;
    }

    result->failure_count = result->failures.size();
    if (pending.empty()) return StatusCode::kOk;

    std::string save_error;
    const auto save_status = config_store_.save_device_aliases_batch(aliases, &save_error);
    if (!is_ok(save_status)) {
        if (error_message != nullptr) {
            *error_message = save_error.empty() ? "批量保存设备名称失败" : save_error;
        }
        return save_status;
    }

    for (const auto& update : pending) {
        auto& device = system_config_.devices[update.device_index];
        device.device_name = update.display_name.empty() ? device.generated_name : update.display_name;
        if (auto status = data_store_.get_device_status(device.device_id); status.has_value()) {
            status->device_name = device.device_name;
            data_store_.update_device_status(*status);
        }
    }
    topology_manager_.rebind_system_config(system_config_);
    synchronize_alarm_topology_locked("批量设备名称更新");

    result->success_count = pending.size();
    result->failure_count = result->failures.size();
    const auto summary = all_restore
        ? "已批量恢复 " + std::to_string(result->success_count) + " 个设备系统推导名称"
        : "已批量更新 " + std::to_string(result->success_count) + " 个设备名称";
    const auto detail = result->failure_count == 0
        ? std::string{}
        : "另有 " + std::to_string(result->failure_count) + " 个设备更新失败";
    append_event("info", "device_name", "batch", summary, detail, updated_at_ms);
    return StatusCode::kOk;
}

// 读取主站按通道。
std::vector<MasterNodeConfig> BackendService::get_masters_by_channel(const ChannelId& channel_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    std::vector<MasterNodeConfig> masters;
    for (const auto* master : topology_manager_.get_masters_by_channel(channel_id)) {
        if (master != nullptr) {
            masters.push_back(*master);
        }
    }
    return masters;
}

// 读取设备按主站。
std::vector<DeviceConfig> BackendService::get_devices_by_master(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    std::vector<DeviceConfig> devices;
    for (const auto* device : topology_manager_.get_devices_by_master(master_id)) {
        if (device != nullptr) {
            devices.push_back(*device);
        }
    }
    return devices;
}

// 获取系统整体运行状态。
SystemStatus BackendService::get_system_status() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_system_status();
}

// 读取通道状态。
std::vector<ChannelStatus> BackendService::get_channel_statuses() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_all_channel_statuses();
}

// 读取主站状态。
std::vector<MasterNodeStatus> BackendService::get_master_statuses() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_all_master_statuses();
}

// 读取设备状态。
std::vector<DeviceStatus> BackendService::get_device_statuses() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_all_device_statuses();
}

// 读取设备状态按主站。
std::vector<DeviceStatus> BackendService::get_device_statuses_by_master(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_devices_by_master(master_id);
}

// 读取通道状态。
std::optional<ChannelStatus> BackendService::get_channel_status(const ChannelId& channel_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_channel_status(channel_id);
}

// 读取主站状态。
std::optional<MasterNodeStatus> BackendService::get_master_status(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_master_status(master_id);
}

// 读取设备状态。
std::optional<DeviceStatus> BackendService::get_device_status(const DeviceId& device_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_device_status(device_id);
}

// 读取设备实时数据。
std::optional<DeviceRealtimeSnapshot> BackendService::get_device_realtime(const DeviceId& device_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_device_realtime(device_id);
}

// 读取全部设备实时数据。
std::vector<DeviceRealtimeSnapshot> BackendService::get_all_device_realtime() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_all_device_realtime_snapshots();
}

// 读取设备实时数据按主站。
std::vector<DeviceRealtimeSnapshot> BackendService::get_device_realtime_by_master(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return data_store_.get_device_realtime_snapshots_by_master(master_id);
}

// 获取实时监控页面聚合视图。
RealtimeViewSnapshot BackendService::get_realtime_view_snapshot() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);

    // 实时页需要“配置清单 + 最新运行态”同时存在，才能展示未采集、离线和正常设备的完整列表。
    auto snapshot = data_store_.get_realtime_page_snapshot();
    snapshot.device_template_generation = device_template_registry_generation();
    snapshot.devices = system_config_.devices;
    snapshot.channels = system_config_.channels;
    snapshot.masters = system_config_.master_nodes;
    return snapshot;
}

}  // namespace edge_controller
