// 线程安全的当前运行态内存快照仓库接口。
#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "model/device_realtime.h"
#include "model/realtime_view_snapshot.h"
#include "model/service_summary.h"
#include "model/system_config.h"
#include "model/system_status.h"

namespace edge_controller {

class DataStore {
public:
    // 初始化。
    void initialize(const SystemConfig& system_config);

    // 更新通道状态。
    void update_channel_status(const ChannelStatus& status);
    // 更新主站状态。
    void update_master_status(const MasterNodeStatus& status);
    // 更新设备状态。
    void update_device_status(const DeviceStatus& status);
    // 批量更新设备状态。
    void update_device_statuses(const std::vector<DeviceStatus>& statuses);
    // 更新系统状态。
    void update_system_status(const SystemStatus& status);

    // 获取通道状态。
    std::optional<ChannelStatus> get_channel_status(const ChannelId& channel_id) const;
    // 获取主站状态。
    std::optional<MasterNodeStatus> get_master_status(const MasterNodeId& master_id) const;
    // 获取设备状态。
    std::optional<DeviceStatus> get_device_status(const DeviceId& device_id) const;
    // 一次共享锁读取指定设备状态，供同轮批量失败合并使用。
    std::vector<DeviceStatus> get_device_statuses(const std::vector<DeviceId>& device_ids) const;
    // 获取设备实时数据。
    std::optional<DeviceRealtimeSnapshot> get_device_realtime(const DeviceId& device_id) const;

    // 获取全部通道状态快照。
    std::vector<ChannelStatus> get_all_channel_statuses() const;
    // 获取全部主站状态快照。
    std::vector<MasterNodeStatus> get_all_master_statuses() const;
    // 获取全部设备状态快照。
    std::vector<DeviceStatus> get_all_device_statuses() const;
    // 获取设备按主站。
    std::vector<DeviceStatus> get_devices_by_master(const MasterNodeId& master_id) const;
    // 获取全部设备实时数据快照。
    std::vector<DeviceRealtimeSnapshot> get_all_device_realtime_snapshots() const;
    // 在一次共享锁内构造 MQTT 所需的轻量实时快照，避免复制无关系统列表和设备点位状态。
    RealtimeViewSnapshot get_mqtt_realtime_snapshot() const;
    // 按主站获取设备实时数据快照。
    std::vector<DeviceRealtimeSnapshot> get_device_realtime_snapshots_by_master(const MasterNodeId& master_id) const;

    // 获取轮询摘要。
    PollingCycleSummary get_polling_summary() const;
    // 只更新轮询相关标量字段，避免为一次摘要更新复制完整系统状态列表。
    void update_polling_summary(
        const PollingCycleSummary& summary,
        const std::string& status_message);
    // 在一次共享锁内选取当前最重要的对象诊断，避免构造三份状态列表快照。
    DiagnosisStatus get_current_object_diagnosis() const;
    // 获取系统状态。
    SystemStatus get_system_status() const;

private:
    // 在持锁状态下重建系统状态。
    void rebuild_system_status_locked();
    // 在持锁状态下刷新通道状态列表。
    void refresh_channel_status_list_locked();
    // 在持锁状态下刷新主站状态列表。
    void refresh_master_status_list_locked();
    // 在持锁状态下刷新设备状态列表。
    void refresh_device_status_list_locked();
    // 在持锁状态下更新设备实时数据。
    void update_device_realtime_locked(const DeviceStatus& status);

    // 采集线程写入、IPC 线程读取：写操作保持 map、顺序列表、索引表和在线计数在同一临界区同步更新。
    // 读取接口始终返回值快照，调用方不得持有指向仓库内部元素的引用或指针。
    mutable std::shared_mutex mutex_;
    std::unordered_map<ChannelId, ChannelStatus> channel_status_by_id_;
    std::unordered_map<MasterNodeId, MasterNodeStatus> master_status_by_id_;
    std::unordered_map<DeviceId, DeviceStatus> device_status_by_id_;
    std::unordered_map<DeviceId, DeviceRealtimeSnapshot> device_realtime_by_id_;
    std::unordered_map<MasterNodeId, std::vector<DeviceId>> device_ids_by_master_;
    // 三张索引表定位 SystemStatus 中的稳定顺序槽位，使单对象更新无需重建整张展示列表。
    std::unordered_map<ChannelId, std::size_t> channel_status_index_by_id_;
    std::unordered_map<MasterNodeId, std::size_t> master_status_index_by_id_;
    std::unordered_map<DeviceId, std::size_t> device_status_index_by_id_;
    std::vector<ChannelId> channel_order_;
    std::vector<MasterNodeId> master_order_;
    std::vector<DeviceId> device_order_;
    SystemStatus system_status_{};
};

}  // namespace edge_controller
