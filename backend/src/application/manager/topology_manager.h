// 建立通道、主站和自动推导设备之间的索引关系。
// 边界：只维护资源所有权和拓扑索引，不复制协议或业务规则。

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "shared/common/status_code.h"
#include "data/model/system_config.h"

namespace edge_controller {

// 为固定的“通道 -> 主控 -> 设备”拓扑建立轻量索引。
// 构建阶段先提交稳定 id，绑定生效配置后再一次性建立指针缓存；
// 配置重载失败不会污染当前拓扑，稳态查询也不再重复扫描或分配。
class TopologyManager {
public:
    // 禁止复制拓扑管理器。
    TopologyManager() = default;
    // 构造 TopologyManager 实例。
    TopologyManager(const TopologyManager&) = delete;
    // 禁止复制赋值拓扑管理器。
    TopologyManager& operator=(const TopologyManager&) = delete;
    // 移动构造拓扑管理器并转移索引所有权。
    TopologyManager(TopologyManager&& other) noexcept;
    // 移动赋值拓扑管理器并转移索引所有权。
    TopologyManager& operator=(TopologyManager&& other) noexcept;

    // 根据系统配置建立通道、主站和设备之间的拓扑索引。
    StatusCode build(const SystemConfig& system_config, std::string* error_message = nullptr);
    // 绑定当前生效配置对象，供索引查找返回稳定指针。
    void rebind_system_config(const SystemConfig& system_config);

    // 获取当前拓扑中的全部通道。
    const std::vector<const ChannelConfig*>& get_all_channels() const;
    // 获取指定通道下挂载的主站列表。
    const std::vector<const MasterNodeConfig*>& get_masters_by_channel(const ChannelId& channel_id) const;
    // 获取指定主站下推导出的设备列表。
    const std::vector<const DeviceConfig*>& get_devices_by_master(const MasterNodeId& master_id) const;
    // 根据设备 ID 反查所属主站。
    const MasterNodeConfig* get_master_by_device(const DeviceId& device_id) const;

private:
    // 移动来源。
    void move_from(TopologyManager&& other) noexcept;
    // 根据当前生效配置一次性重建 ID 到配置对象的指针索引。
    void rebuild_pointer_indexes();
    // 从当前配置中查找通道配置。
    const ChannelConfig* find_channel(const ChannelId& channel_id) const;
    // 从当前配置中查找主站配置。
    const MasterNodeConfig* find_master(const MasterNodeId& master_id) const;
    // 从当前配置中查找设备配置。
    const DeviceConfig* find_device(const DeviceId& device_id) const;

    const SystemConfig* system_config_{nullptr};
    std::vector<ChannelId> channel_ids_{};
    std::unordered_map<ChannelId, std::vector<MasterNodeId>> master_ids_by_channel_{};
    std::unordered_map<MasterNodeId, std::vector<DeviceId>> device_ids_by_master_{};
    std::unordered_map<DeviceId, MasterNodeId> master_id_by_device_{};
    std::unordered_map<ChannelId, const ChannelConfig*> channels_by_id_{};
    std::unordered_map<MasterNodeId, const MasterNodeConfig*> masters_by_id_{};
    std::unordered_map<DeviceId, const DeviceConfig*> devices_by_id_{};
    std::unordered_map<ChannelId, std::vector<const MasterNodeConfig*>> masters_by_channel_{};
    std::unordered_map<MasterNodeId, std::vector<const DeviceConfig*>> devices_by_master_{};
    std::vector<const ChannelConfig*> channels_{};
};

}  // namespace edge_controller
