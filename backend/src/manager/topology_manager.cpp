// 拓扑管理器根据主站和模板推导设备清单，保证通道—主站—设备引用关系一致。
#include "manager/topology_manager.h"

#include <utility>

namespace edge_controller {

// 构造 TopologyManager 实例。
TopologyManager::TopologyManager(TopologyManager&& other) noexcept
{
    move_from(std::move(other));
}

// 移动赋值对象并转移其资源所有权。
TopologyManager& TopologyManager::operator=(TopologyManager&& other) noexcept
{
    if (this != &other) {
        move_from(std::move(other));
    }
    return *this;
}

// 根据系统配置构建通道、主站和设备索引。
StatusCode TopologyManager::build(const SystemConfig& system_config, std::string* error_message)
{
    std::vector<ChannelId> next_channel_ids;
    std::unordered_map<ChannelId, std::vector<MasterNodeId>> next_master_ids_by_channel;
    std::unordered_map<MasterNodeId, std::vector<DeviceId>> next_device_ids_by_master;
    std::unordered_map<DeviceId, MasterNodeId> next_master_id_by_device;
    std::unordered_map<ChannelId, const ChannelConfig*> channel_index;
    std::unordered_map<MasterNodeId, const MasterNodeConfig*> master_index;

    next_channel_ids.reserve(system_config.channels.size());
    channel_index.reserve(system_config.channels.size());
    master_index.reserve(system_config.master_nodes.size());

    for (const auto& channel : system_config.channels) {
        next_channel_ids.push_back(channel.channel_id);
        next_master_ids_by_channel[channel.channel_id] = {};
        channel_index[channel.channel_id] = &channel;
    }

    for (const auto& master : system_config.master_nodes) {
        master_index[master.master_id] = &master;
        next_device_ids_by_master[master.master_id] = {};

        if (master.protocol == MasterProtocol::kModbusRtu ||
            master.protocol == MasterProtocol::kModbusTcp) {
            const auto channel_iterator = next_master_ids_by_channel.find(master.channel_id);
            if (channel_iterator == next_master_ids_by_channel.end()) {
                if (error_message != nullptr) {
                    *error_message = "构建拓扑失败：主控 " + master.master_id + " 找不到所属通道";
                }
                return StatusCode::kInvalidArgument;
            }
            const auto configured_channel = channel_index.find(master.channel_id);
            const auto* channel =
                configured_channel == channel_index.end()
                    ? nullptr
                    : configured_channel->second;
            const bool channel_type_matched =
                channel != nullptr &&
                ((master.protocol == MasterProtocol::kModbusRtu &&
                  channel->channel_type == ChannelType::kModbusRtuSerial) ||
                 (master.protocol == MasterProtocol::kModbusTcp &&
                  channel->channel_type == ChannelType::kModbusTcp));
            if (!channel_type_matched) {
                if (error_message != nullptr) {
                    *error_message = "构建拓扑失败：主控 " + master.master_id + " 绑定的通道类型与协议不匹配";
                }
                return StatusCode::kInvalidArgument;
            }

            channel_iterator->second.push_back(master.master_id);
        }
    }

    for (const auto& device : system_config.devices) {
        const auto master_iterator = master_index.find(device.master_id);
        if (master_iterator == master_index.end()) {
            if (error_message != nullptr) {
                *error_message =
                    "构建拓扑失败：设备 " + device.device_id + " 引用了不存在的主控 " + device.master_id;
            }
            return StatusCode::kInvalidArgument;
        }

        next_device_ids_by_master[device.master_id].push_back(device.device_id);
        next_master_id_by_device[device.device_id] = master_iterator->second->master_id;
    }

    system_config_ = &system_config;
    channel_ids_ = std::move(next_channel_ids);
    master_ids_by_channel_ = std::move(next_master_ids_by_channel);
    device_ids_by_master_ = std::move(next_device_ids_by_master);
    master_id_by_device_ = std::move(next_master_id_by_device);
    rebuild_pointer_indexes();
    return StatusCode::kOk;
}

// 移动来源。
void TopologyManager::move_from(TopologyManager&& other) noexcept
{
    // SystemConfig 由 BackendService 持有，move 后必须由调用方显式 rebind，避免保留旧临时配置地址。
    system_config_ = nullptr;
    channel_ids_ = std::move(other.channel_ids_);
    master_ids_by_channel_ = std::move(other.master_ids_by_channel_);
    device_ids_by_master_ = std::move(other.device_ids_by_master_);
    master_id_by_device_ = std::move(other.master_id_by_device_);
    channels_by_id_.clear();
    masters_by_id_.clear();
    devices_by_id_.clear();
    masters_by_channel_.clear();
    devices_by_master_.clear();
    channels_.clear();

    other.system_config_ = nullptr;
    other.channels_by_id_.clear();
    other.masters_by_id_.clear();
    other.devices_by_id_.clear();
    other.masters_by_channel_.clear();
    other.devices_by_master_.clear();
    other.channels_.clear();
}

// 重新绑定当前生效系统配置并重建缓存。
void TopologyManager::rebind_system_config(const SystemConfig& system_config)
{
    system_config_ = &system_config;
    rebuild_pointer_indexes();
}

// 获取全部通道配置指针。
const std::vector<const ChannelConfig*>& TopologyManager::get_all_channels() const
{
    return channels_;
}

// 获取指定通道下的主站配置指针。
const std::vector<const MasterNodeConfig*>& TopologyManager::get_masters_by_channel(
    const ChannelId& channel_id) const
{
    static const std::vector<const MasterNodeConfig*> empty;
    const auto iterator = masters_by_channel_.find(channel_id);
    return iterator == masters_by_channel_.end() ? empty : iterator->second;
}

// 获取指定主站下的设备配置指针。
const std::vector<const DeviceConfig*>& TopologyManager::get_devices_by_master(
    const MasterNodeId& master_id) const
{
    static const std::vector<const DeviceConfig*> empty;
    const auto iterator = devices_by_master_.find(master_id);
    return iterator == devices_by_master_.end() ? empty : iterator->second;
}

// 根据设备 ID 查找所属主站。
const MasterNodeConfig* TopologyManager::get_master_by_device(const DeviceId& device_id) const
{
    const auto iterator = master_id_by_device_.find(device_id);
    if (iterator == master_id_by_device_.end()) {
        return nullptr;
    }
    return find_master(iterator->second);
}

// 一次性重建配置对象指针索引；稳态轮询只做哈希查找，不再线性扫描整个配置。
void TopologyManager::rebuild_pointer_indexes()
{
    channels_by_id_.clear();
    masters_by_id_.clear();
    devices_by_id_.clear();
    masters_by_channel_.clear();
    devices_by_master_.clear();
    channels_.clear();

    if (system_config_ == nullptr) {
        return;
    }

    channels_by_id_.reserve(system_config_->channels.size());
    masters_by_id_.reserve(system_config_->master_nodes.size());
    devices_by_id_.reserve(system_config_->devices.size());
    masters_by_channel_.reserve(master_ids_by_channel_.size());
    devices_by_master_.reserve(device_ids_by_master_.size());
    channels_.reserve(channel_ids_.size());

    for (const auto& channel : system_config_->channels) {
        channels_by_id_.emplace(channel.channel_id, &channel);
    }
    for (const auto& master : system_config_->master_nodes) {
        masters_by_id_.emplace(master.master_id, &master);
    }
    for (const auto& device : system_config_->devices) {
        devices_by_id_.emplace(device.device_id, &device);
    }
    for (const auto& entry : master_ids_by_channel_) {
        auto& masters = masters_by_channel_[entry.first];
        masters.reserve(entry.second.size());
        for (const auto& master_id : entry.second) {
            const auto* master = find_master(master_id);
            if (master != nullptr) {
                masters.push_back(master);
            }
        }
    }
    for (const auto& entry : device_ids_by_master_) {
        auto& devices = devices_by_master_[entry.first];
        devices.reserve(entry.second.size());
        for (const auto& device_id : entry.second) {
            const auto* device = find_device(device_id);
            if (device != nullptr) {
                devices.push_back(device);
            }
        }
    }

    for (const auto& channel_id : channel_ids_) {
        const auto* channel = find_channel(channel_id);
        if (channel != nullptr) {
            channels_.push_back(channel);
        }
    }
}

// 查找通道。
const ChannelConfig* TopologyManager::find_channel(const ChannelId& channel_id) const
{
    const auto iterator = channels_by_id_.find(channel_id);
    return iterator == channels_by_id_.end() ? nullptr : iterator->second;
}

// 查找主站。
const MasterNodeConfig* TopologyManager::find_master(const MasterNodeId& master_id) const
{
    const auto iterator = masters_by_id_.find(master_id);
    return iterator == masters_by_id_.end() ? nullptr : iterator->second;
}

// 查找设备。
const DeviceConfig* TopologyManager::find_device(const DeviceId& device_id) const
{
    const auto iterator = devices_by_id_.find(device_id);
    return iterator == devices_by_id_.end() ? nullptr : iterator->second;
}

}  // namespace edge_controller
