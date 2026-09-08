// 内存运行态仓库：在采集线程与 IPC 查询之间提供带锁快照，不负责持久化历史数据。
#include "data/datastore/data_store.h"
#include <algorithm>
#include "shared/common/time_utils.h"

#include "data/model/data_item_keys.h"

namespace edge_controller {

namespace {

// 构造点位值。
PointValue build_point_value(
    const std::string& point_suffix,
    const std::string& point_name,
    double value,
    TimestampMs sample_time_ms)
{
    PointValue point;
    point.key = point_suffix;
    point.name = point_name;
    point.value = value;
    point.raw_value = value;
    // has_resistance 已明确表示该兼容值存在；字段质量不能复用设备通讯质量。
    point.quality = DataQuality::kGood;
    point.valid = true;
    point.sample_time_ms = sample_time_ms;
    return point;
}

// 构造实时数据快照。
DeviceRealtimeSnapshot build_realtime_snapshot(const DeviceStatus& status)
{
    DeviceRealtimeSnapshot snapshot;
    snapshot.device_id = status.device_id;
    snapshot.device_name = status.device_name;
    snapshot.master_id = status.master_id;
    snapshot.template_id = status.template_id;
    snapshot.template_name = status.template_name;
    snapshot.sample_time_ms = status.updated_at_ms;
    snapshot.communication_quality = status.communication_quality;
    snapshot.points = status.points;

    if (status.has_resistance) {
        snapshot.has_resistance = true;
        for (const auto& point : snapshot.points) {
            if (point.key == kResistanceFieldKey) {
                snapshot.resistance = point;
                break;
            }
        }
        if (snapshot.resistance.sample_time_ms == 0) {
            snapshot.resistance = build_point_value(
                kResistanceFieldKey,
                "接地电阻",
                status.resistance_value,
                status.updated_at_ms);
        }
    }

    if (snapshot.points.empty() && snapshot.has_resistance) {
        snapshot.points.push_back(snapshot.resistance);
    }

    return snapshot;
}

bool has_realtime_sample(const DeviceStatus& status)
{
    return status.updated_at_ms > 0 && (!status.points.empty() || status.has_resistance);
}

// 实时页面会从 DeviceRealtimeSnapshot 取得点位；健康投影只复制诊断与状态字段，
// 避免同一批 PointValue 在 SystemStatus 中再次深拷贝。
DeviceStatus build_device_health_status(const DeviceStatus& source)
{
    DeviceStatus result;
    result.device_id = source.device_id;
    result.device_name = source.device_name;
    result.master_id = source.master_id;
    result.template_id = source.template_id;
    result.template_name = source.template_name;
    result.online = source.online;
    result.last_collect_success = source.last_collect_success;
    result.communication_quality = source.communication_quality;
    result.updated_at_ms = source.updated_at_ms;
    result.last_success_time_ms = source.last_success_time_ms;
    result.last_failure_time_ms = source.last_failure_time_ms;
    result.has_resistance = source.has_resistance;
    result.resistance_value = source.resistance_value;
    result.diagnosis = source.diagnosis;
    result.last_error_message = source.last_error_message;
    return result;
}

SystemStatus build_realtime_system_status(const SystemStatus& source)
{
    SystemStatus result;
    result.config_loaded = source.config_loaded;
    result.service_ready = source.service_ready;
    result.running = source.running;
    result.polling_running = source.polling_running;
    result.polling_state = source.polling_state;
    result.started_at_ms = source.started_at_ms;
    result.stopped_at_ms = source.stopped_at_ms;
    result.last_heartbeat_ms = source.last_heartbeat_ms;
    result.last_poll_cycle_started_at_ms = source.last_poll_cycle_started_at_ms;
    result.last_poll_cycle_finished_at_ms = source.last_poll_cycle_finished_at_ms;
    result.online_channel_count = source.online_channel_count;
    result.online_master_count = source.online_master_count;
    result.online_device_count = source.online_device_count;
    result.last_poll_cycle_master_count = source.last_poll_cycle_master_count;
    result.last_poll_cycle_success_master_count = source.last_poll_cycle_success_master_count;
    result.last_poll_cycle_failed_master_count = source.last_poll_cycle_failed_master_count;
    result.last_poll_cycle_success_device_count = source.last_poll_cycle_success_device_count;
    result.last_poll_cycle_failed_device_count = source.last_poll_cycle_failed_device_count;
    result.last_poll_cycle_has_error = source.last_poll_cycle_has_error;
    result.diagnosis = source.diagnosis;
    result.last_status_message = source.last_status_message;
    result.last_poll_cycle_error_message = source.last_poll_cycle_error_message;
    result.channel_status_list = source.channel_status_list;
    result.master_status_list = source.master_status_list;
    result.device_status_list.reserve(source.device_status_list.size());
    for (const auto& status : source.device_status_list) {
        result.device_status_list.push_back(build_device_health_status(status));
    }
    return result;
}

}  // namespace

// 使用系统配置初始化运行状态存储。
void DataStore::initialize(const SystemConfig& system_config)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    channel_status_by_id_.clear();
    master_status_by_id_.clear();
    device_status_by_id_.clear();
    device_names_.clear();
    device_ids_by_master_.clear();
    channel_status_index_by_id_.clear();
    master_status_index_by_id_.clear();
    device_status_index_by_id_.clear();
    channel_order_.clear();
    master_order_.clear();
    device_order_.clear();
    system_status_ = {};
    system_status_.diagnosis = make_normal_diagnosis(DiagnosisLevel::kSystem, "system", "系统", 0);

    for (const auto& channel : system_config.channels) {
        ChannelStatus status;
        status.channel_id = channel.channel_id;
        status.configured = true;
        status.enabled = channel.enabled;
        status.device_path = channel_target_description(channel);
        status.status = channel.enabled ? "closed" : "disabled";
        status.diagnosis = make_normal_diagnosis(
            DiagnosisLevel::kChannel,
            channel.channel_id,
            status.device_path,
            0);
        status.last_error_message = status.diagnosis.error_code == "NONE" ? "" : status.diagnosis.message;
        channel_status_by_id_[channel.channel_id] = status;
        channel_order_.push_back(channel.channel_id);
    }

    freshness_deadlines_.clear();
    freshness_ttl_by_master_.clear();
    for (const auto& master : system_config.master_nodes) {
        freshness_ttl_by_master_[master.master_id] = std::max<TimestampMs>(3000, 3ULL * master.poll_interval_ms);
        MasterNodeStatus status;
        status.master_id = master.master_id;
        status.diagnosis = make_normal_diagnosis(
            DiagnosisLevel::kMaster,
            master.master_id,
            master.master_name,
            0);
        status.last_error_message = status.diagnosis.error_code == "NONE" ? "" : status.diagnosis.message;
        master_status_by_id_[master.master_id] = status;
        master_order_.push_back(master.master_id);
        device_ids_by_master_[master.master_id] = {};
    }

    for (const auto& device : system_config.devices) {
        device_names_[device.device_id] = device.device_name;
        DeviceStatus status;
        status.device_id = device.device_id;
        status.device_name = device.device_name;
        status.master_id = device.master_id;
        status.diagnosis = make_normal_diagnosis(
            DiagnosisLevel::kDevice,
            device.device_id,
            device.device_name,
            0);
        status.last_error_message = status.diagnosis.error_code == "NONE" ? "" : status.diagnosis.message;
        device_status_by_id_[device.device_id] = status;
        device_order_.push_back(device.device_id);
        device_ids_by_master_[device.master_id].push_back(device.device_id);
    }

    rebuild_system_status_locked();
}

void DataStore::update_device_names(const std::vector<DeviceConfig>& devices)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (const auto& device : devices) {
        device_names_[device.device_id] = device.device_name;
        const auto current = device_status_by_id_.find(device.device_id);
        if (current != device_status_by_id_.end()) {
            auto status = current->second;
            status.device_name = device.device_name;
            update_device_status_locked(status);
        }
    }
}

void DataStore::reconcile_channels(const std::vector<ChannelConfig>& channels)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::unordered_map<ChannelId, ChannelStatus> retained;
    channel_order_.clear();
    for (const auto& channel : channels) {
        const auto previous = channel_status_by_id_.find(channel.channel_id);
        auto status = previous == channel_status_by_id_.end() ? ChannelStatus{} : previous->second;
        status.channel_id = channel.channel_id;
        status.configured = true;
        status.enabled = channel.enabled;
        status.device_path = channel_target_description(channel);
        retained.emplace(channel.channel_id, std::move(status));
        channel_order_.push_back(channel.channel_id);
    }
    channel_status_by_id_ = std::move(retained);
    refresh_channel_status_list_locked();
}

// 更新通道状态。
void DataStore::update_channel_status(const ChannelStatus& status)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // 正常采集路径直接覆盖既有槽位；只有索引不一致这一防御性分支才回退为全量重建。
    const auto previous = channel_status_by_id_.find(status.channel_id);
    if (previous == channel_status_by_id_.end()) {
        channel_order_.push_back(status.channel_id);
        channel_status_by_id_[status.channel_id] = status;
        channel_status_index_by_id_[status.channel_id] = system_status_.channel_status_list.size();
        system_status_.channel_status_list.push_back(status);
        if (status.opened) {
            ++system_status_.online_channel_count;
        }
        return;
    }

    const bool was_opened = previous->second.opened;
    previous->second = status;
    const auto index = channel_status_index_by_id_.find(status.channel_id);
    if (index == channel_status_index_by_id_.end() ||
        index->second >= system_status_.channel_status_list.size()) {
        refresh_channel_status_list_locked();
        return;
    }
    system_status_.channel_status_list[index->second] = status;
    if (was_opened != status.opened) {
        if (status.opened) {
            ++system_status_.online_channel_count;
        } else if (system_status_.online_channel_count > 0) {
            --system_status_.online_channel_count;
        }
    }
}

// 更新主站状态。
void DataStore::update_master_status(const MasterNodeStatus& status)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto previous = master_status_by_id_.find(status.master_id);
    if (previous == master_status_by_id_.end()) {
        master_order_.push_back(status.master_id);
        master_status_by_id_[status.master_id] = status;
        master_status_index_by_id_[status.master_id] = system_status_.master_status_list.size();
        system_status_.master_status_list.push_back(status);
        if (status.online) {
            ++system_status_.online_master_count;
        }
        return;
    }

    const bool was_online = previous->second.online;
    previous->second = status;
    const auto index = master_status_index_by_id_.find(status.master_id);
    if (index == master_status_index_by_id_.end() ||
        index->second >= system_status_.master_status_list.size()) {
        refresh_master_status_list_locked();
        return;
    }
    system_status_.master_status_list[index->second] = status;
    if (was_online != status.online) {
        if (status.online) {
            ++system_status_.online_master_count;
        } else if (system_status_.online_master_count > 0) {
            --system_status_.online_master_count;
        }
    }
}

// 更新设备状态。
void DataStore::update_device_status(const DeviceStatus& status)
{
    update_device_statuses({status});
}

// 更新一组设备状态。
void DataStore::update_device_statuses(const std::vector<DeviceStatus>& statuses)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto now = time_utils::steady_now_ms();
    for (const auto& status : statuses) {
        const auto ttl = freshness_ttl_by_master_.find(status.master_id);
        const auto lifetime = ttl == freshness_ttl_by_master_.end() ? 3000 : ttl->second;
        for (const auto& point : status.points) {
            if (point.quality == DataQuality::kGood && point.valid) {
                auto& stamp = freshness_deadlines_[{status.device_id, point.key}];
                if (stamp.second == 0 || stamp.first != point.sample_time_ms)
                    stamp = {point.sample_time_ms, now + lifetime};
            }
        }
        update_device_status_locked(status);
    }
}

void DataStore::update_device_status_locked(const DeviceStatus& input)
{
    const auto& status = input;
    const auto previous = device_status_by_id_.find(status.device_id);
    if (previous == device_status_by_id_.end()) {
        device_order_.push_back(status.device_id);
        device_ids_by_master_[status.master_id].push_back(status.device_id);
        device_status_by_id_[status.device_id] = status;
        device_status_index_by_id_[status.device_id] = system_status_.device_status_list.size();
        system_status_.device_status_list.push_back(build_device_health_status(status));
        if (status.online) {
            ++system_status_.online_device_count;
        }
    } else {
        const bool was_online = previous->second.online;
        previous->second = status;
        const auto index = device_status_index_by_id_.find(status.device_id);
        if (index == device_status_index_by_id_.end() ||
            index->second >= system_status_.device_status_list.size()) {
            refresh_device_status_list_locked();
        } else {
            system_status_.device_status_list[index->second] = build_device_health_status(status);
            if (was_online != status.online) {
                if (status.online) {
                    ++system_status_.online_device_count;
                } else if (system_status_.online_device_count > 0) {
                    --system_status_.online_device_count;
                }
            }
        }
    }
    const auto name = device_names_.find(status.device_id);
    if (name != device_names_.end()) {
        device_status_by_id_.at(status.device_id).device_name = name->second;
        system_status_.device_status_list.at(device_status_index_by_id_.at(status.device_id)).device_name = name->second;
    }

}

std::vector<DeviceStatus> DataStore::expire_device_values(bool stopped, const std::vector<MasterNodeId>* masters)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto now = time_utils::steady_now_ms();
    std::vector<DeviceStatus> changed;
    for (const auto& entry : device_status_by_id_) {
        auto status = entry.second;
        if (masters && std::find(masters->begin(), masters->end(), status.master_id) == masters->end()) continue;
        bool expired = false;
        for (auto& point : status.points) {
            const auto deadline = freshness_deadlines_.find({status.device_id, point.key});
            if (point.quality == DataQuality::kGood &&
                (stopped || (deadline != freshness_deadlines_.end() && now >= deadline->second.second))) {
                point.quality = DataQuality::kStale;
                point.valid = false;
                point.message = stopped ? "采集已停止，保留最后采样值" : "采样已超期，等待新数据";
                expired = true;
            }
        }
        if (!expired) continue;
        status.communication_quality = DataQuality::kStale;
        status.last_collect_success = false;
        status.online = false;
        status.has_resistance = false;
        status.last_error_message = stopped ? "采集已停止" : "采样已超期";
        status.diagnosis = make_diagnosis(DiagnosisLevel::kDevice, status.device_id, status.device_name,
            DiagnosisRunStatus::kWarning, stopped ? DiagnosisErrorCode::kPollingNotRunning : DiagnosisErrorCode::kDataInvalid,
            status.last_success_time_ms, time_utils::system_now_ms(), 0);
        status.diagnosis.message = status.last_error_message;
        // 保留采样时间与数值；质量变化不能伪造一次新采样。
        changed.push_back(std::move(status));
    }
    for (const auto& status : changed) update_device_status_locked(status);
    return changed;
}

// 更新系统状态。
void DataStore::update_system_status(const SystemStatus& status)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    system_status_.config_loaded = status.config_loaded;
    system_status_.service_ready = status.service_ready;
    system_status_.running = status.running;
    system_status_.polling_running = status.polling_running;
    system_status_.polling_state = status.polling_state;
    system_status_.started_at_ms = status.started_at_ms;
    system_status_.stopped_at_ms = status.stopped_at_ms;
    system_status_.last_heartbeat_ms = status.last_heartbeat_ms;
    system_status_.last_poll_cycle_started_at_ms = status.last_poll_cycle_started_at_ms;
    system_status_.last_poll_cycle_finished_at_ms = status.last_poll_cycle_finished_at_ms;
    system_status_.last_poll_cycle_master_count = status.last_poll_cycle_master_count;
    system_status_.last_poll_cycle_success_master_count = status.last_poll_cycle_success_master_count;
    system_status_.last_poll_cycle_failed_master_count = status.last_poll_cycle_failed_master_count;
    system_status_.last_poll_cycle_success_device_count = status.last_poll_cycle_success_device_count;
    system_status_.last_poll_cycle_failed_device_count = status.last_poll_cycle_failed_device_count;
    system_status_.last_poll_cycle_has_error = status.last_poll_cycle_has_error;
    system_status_.diagnosis = status.diagnosis;
    system_status_.last_status_message = status.last_status_message;
    system_status_.last_poll_cycle_error_message = status.last_poll_cycle_error_message;
}

// 读取通道状态。
std::optional<ChannelStatus> DataStore::get_channel_status(const ChannelId& channel_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto iterator = channel_status_by_id_.find(channel_id);
    if (iterator == channel_status_by_id_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

// 读取主站状态。
std::optional<MasterNodeStatus> DataStore::get_master_status(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto iterator = master_status_by_id_.find(master_id);
    if (iterator == master_status_by_id_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

// 读取设备状态。
std::optional<DeviceStatus> DataStore::get_device_status(const DeviceId& device_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto iterator = device_status_by_id_.find(device_id);
    if (iterator == device_status_by_id_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

std::vector<DeviceStatus> DataStore::get_device_statuses(const std::vector<DeviceId>& device_ids) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<DeviceStatus> statuses;
    statuses.reserve(device_ids.size());
    for (const auto& device_id : device_ids) {
        const auto iterator = device_status_by_id_.find(device_id);
        if (iterator != device_status_by_id_.end()) {
            statuses.push_back(iterator->second);
        }
    }
    return statuses;
}

// 读取设备实时数据。
std::optional<DeviceRealtimeSnapshot> DataStore::get_device_realtime(const DeviceId& device_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto iterator = device_status_by_id_.find(device_id);
    if (iterator == device_status_by_id_.end() || !has_realtime_sample(iterator->second)) {
        return std::nullopt;
    }
    return build_realtime_snapshot(iterator->second);
}

// 读取全部通道状态。
std::vector<ChannelStatus> DataStore::get_all_channel_statuses() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<ChannelStatus> statuses;
    statuses.reserve(channel_order_.size());
    for (const auto& channel_id : channel_order_) {
        const auto iterator = channel_status_by_id_.find(channel_id);
        if (iterator != channel_status_by_id_.end()) {
            statuses.push_back(iterator->second);
        }
    }
    return statuses;
}

// 读取全部主站状态。
std::vector<MasterNodeStatus> DataStore::get_all_master_statuses() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<MasterNodeStatus> statuses;
    statuses.reserve(master_order_.size());
    for (const auto& master_id : master_order_) {
        const auto iterator = master_status_by_id_.find(master_id);
        if (iterator != master_status_by_id_.end()) {
            statuses.push_back(iterator->second);
        }
    }
    return statuses;
}

// 读取全部设备状态。
std::vector<DeviceStatus> DataStore::get_all_device_statuses() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<DeviceStatus> statuses;
    statuses.reserve(device_order_.size());
    for (const auto& device_id : device_order_) {
        const auto iterator = device_status_by_id_.find(device_id);
        if (iterator != device_status_by_id_.end()) {
            statuses.push_back(iterator->second);
        }
    }
    return statuses;
}

// 读取设备按主站。
std::vector<DeviceStatus> DataStore::get_devices_by_master(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<DeviceStatus> result;

    const auto device_iterator = device_ids_by_master_.find(master_id);
    if (device_iterator == device_ids_by_master_.end()) {
        return result;
    }

    result.reserve(device_iterator->second.size());
    for (const auto& device_id : device_iterator->second) {
        const auto status_iterator = device_status_by_id_.find(device_id);
        if (status_iterator != device_status_by_id_.end()) {
            result.push_back(status_iterator->second);
        }
    }
    return result;
}

// 读取全部设备实时数据快照。
std::vector<DeviceRealtimeSnapshot> DataStore::get_all_device_realtime_snapshots() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<DeviceRealtimeSnapshot> snapshots;
    snapshots.reserve(device_order_.size());
    for (const auto& device_id : device_order_) {
        const auto iterator = device_status_by_id_.find(device_id);
        if (iterator != device_status_by_id_.end() && has_realtime_sample(iterator->second)) {
            snapshots.push_back(build_realtime_snapshot(iterator->second));
        }
    }
    return snapshots;
}

// 在单次共享锁内生成 Web 实时页需要的健康投影和点位快照，保证二者来自同一采集时刻。
RealtimeViewSnapshot DataStore::get_realtime_page_snapshot() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    RealtimeViewSnapshot snapshot;
    snapshot.system_status = build_realtime_system_status(system_status_);
    snapshot.device_realtime_snapshots.reserve(device_order_.size());
    for (const auto& device_id : device_order_) {
        const auto realtime = device_status_by_id_.find(device_id);
        if (realtime != device_status_by_id_.end() &&
            has_realtime_sample(realtime->second)) {
            snapshot.device_realtime_snapshots.push_back(build_realtime_snapshot(realtime->second));
        }
    }
    return snapshot;
}

// 在单次共享锁内读取 MQTT 发布需要的设备在线摘要和实时快照。
RealtimeViewSnapshot DataStore::get_mqtt_realtime_snapshot() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    RealtimeViewSnapshot snapshot;
    auto& statuses = snapshot.system_status.device_status_list;
    statuses.reserve(device_order_.size());
    snapshot.device_realtime_snapshots.reserve(device_order_.size());

    for (const auto& device_id : device_order_) {
        const auto status = device_status_by_id_.find(device_id);
        if (status != device_status_by_id_.end()) {
            // MQTT 载荷只读取这三个状态字段；不要把 points/diagnosis 和其它系统列表复制到发布线程。
            DeviceStatus lightweight_status;
            lightweight_status.device_id = status->second.device_id;
            lightweight_status.online = status->second.online;
            lightweight_status.updated_at_ms = status->second.updated_at_ms;
            statuses.push_back(std::move(lightweight_status));
        }

        const auto realtime = device_status_by_id_.find(device_id);
        if (realtime != device_status_by_id_.end() &&
            has_realtime_sample(realtime->second)) {
            snapshot.device_realtime_snapshots.push_back(build_realtime_snapshot(realtime->second));
        }
    }
    return snapshot;
}

// 读取设备实时数据快照按主站。
std::vector<DeviceRealtimeSnapshot> DataStore::get_device_realtime_snapshots_by_master(const MasterNodeId& master_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<DeviceRealtimeSnapshot> result;

    const auto device_iterator = device_ids_by_master_.find(master_id);
    if (device_iterator == device_ids_by_master_.end()) {
        return result;
    }

    result.reserve(device_iterator->second.size());
    for (const auto& device_id : device_iterator->second) {
        const auto realtime_iterator = device_status_by_id_.find(device_id);
        if (realtime_iterator != device_status_by_id_.end() && has_realtime_sample(realtime_iterator->second)) {
            result.push_back(build_realtime_snapshot(realtime_iterator->second));
        }
    }
    return result;
}

// 读取轮询摘要。
PollingCycleSummary DataStore::get_polling_summary() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    PollingCycleSummary summary;
    summary.polling_running = system_status_.polling_running;
    summary.polling_state = system_status_.polling_state;
    summary.last_cycle_started_at_ms = system_status_.last_poll_cycle_started_at_ms;
    summary.last_cycle_finished_at_ms = system_status_.last_poll_cycle_finished_at_ms;
    summary.last_cycle_master_count = system_status_.last_poll_cycle_master_count;
    summary.last_cycle_success_master_count = system_status_.last_poll_cycle_success_master_count;
    summary.last_cycle_failed_master_count = system_status_.last_poll_cycle_failed_master_count;
    summary.last_cycle_success_device_count = system_status_.last_poll_cycle_success_device_count;
    summary.last_cycle_failed_device_count = system_status_.last_poll_cycle_failed_device_count;
    summary.last_cycle_has_error = system_status_.last_poll_cycle_has_error;
    summary.diagnosis = system_status_.diagnosis;
    summary.last_cycle_error_message = system_status_.last_poll_cycle_error_message;
    summary.last_heartbeat_ms = system_status_.last_heartbeat_ms;
    return summary;
}

// 更新摘要。
void DataStore::update_polling_summary(
    const PollingCycleSummary& summary,
    const std::string& status_message)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    system_status_.polling_running = summary.polling_running;
    system_status_.polling_state = summary.polling_state;
    system_status_.last_poll_cycle_started_at_ms = summary.last_cycle_started_at_ms;
    system_status_.last_poll_cycle_finished_at_ms = summary.last_cycle_finished_at_ms;
    system_status_.last_poll_cycle_master_count = summary.last_cycle_master_count;
    system_status_.last_poll_cycle_success_master_count = summary.last_cycle_success_master_count;
    system_status_.last_poll_cycle_failed_master_count = summary.last_cycle_failed_master_count;
    system_status_.last_poll_cycle_success_device_count = summary.last_cycle_success_device_count;
    system_status_.last_poll_cycle_failed_device_count = summary.last_cycle_failed_device_count;
    system_status_.last_poll_cycle_has_error = summary.last_cycle_has_error;
    system_status_.diagnosis = summary.diagnosis;
    system_status_.last_poll_cycle_error_message = summary.last_cycle_error_message;
    system_status_.last_heartbeat_ms = summary.last_heartbeat_ms;
    if (!status_message.empty()) {
        system_status_.last_status_message = status_message;
    }
}

DiagnosisStatus DataStore::get_current_object_diagnosis() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    DiagnosisStatus selected;
    for (const auto& channel_id : channel_order_) {
        const auto iterator = channel_status_by_id_.find(channel_id);
        if (iterator != channel_status_by_id_.end()) {
            consider_diagnosis(iterator->second.diagnosis, &selected);
        }
    }
    if (diagnosis_has_issue(selected)) {
        return selected;
    }
    for (const auto& master_id : master_order_) {
        const auto iterator = master_status_by_id_.find(master_id);
        if (iterator != master_status_by_id_.end()) {
            consider_diagnosis(iterator->second.diagnosis, &selected);
        }
    }
    if (diagnosis_has_issue(selected)) {
        return selected;
    }
    for (const auto& device_id : device_order_) {
        const auto iterator = device_status_by_id_.find(device_id);
        if (iterator != device_status_by_id_.end()) {
            consider_diagnosis(iterator->second.diagnosis, &selected);
        }
    }
    return selected;
}

// 读取系统状态。
SystemStatus DataStore::get_system_status() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto snapshot = system_status_;
    for (auto& status : snapshot.device_status_list) status = device_status_by_id_.at(status.device_id);
    return snapshot;
}

// 在持锁状态下重建系统状态。
void DataStore::rebuild_system_status_locked()
{
    refresh_channel_status_list_locked();
    refresh_master_status_list_locked();
    refresh_device_status_list_locked();
}

// 在持锁状态下刷新通道状态列表。
void DataStore::refresh_channel_status_list_locked()
{
    system_status_.channel_status_list.clear();
    channel_status_index_by_id_.clear();
    system_status_.online_channel_count = 0;
    for (const auto& channel_id : channel_order_) {
        const auto iterator = channel_status_by_id_.find(channel_id);
        if (iterator == channel_status_by_id_.end()) {
            continue;
        }
        channel_status_index_by_id_[channel_id] = system_status_.channel_status_list.size();
        system_status_.channel_status_list.push_back(iterator->second);
        if (iterator->second.opened) {
            ++system_status_.online_channel_count;
        }
    }
}

// 在持锁状态下刷新主站状态列表。
void DataStore::refresh_master_status_list_locked()
{
    system_status_.master_status_list.clear();
    master_status_index_by_id_.clear();
    system_status_.online_master_count = 0;
    for (const auto& master_id : master_order_) {
        const auto iterator = master_status_by_id_.find(master_id);
        if (iterator == master_status_by_id_.end()) {
            continue;
        }
        master_status_index_by_id_[master_id] = system_status_.master_status_list.size();
        system_status_.master_status_list.push_back(iterator->second);
        if (iterator->second.online) {
            ++system_status_.online_master_count;
        }
    }
}

// 在持锁状态下刷新设备状态列表。
void DataStore::refresh_device_status_list_locked()
{
    system_status_.device_status_list.clear();
    device_status_index_by_id_.clear();
    system_status_.online_device_count = 0;
    for (const auto& device_id : device_order_) {
        const auto iterator = device_status_by_id_.find(device_id);
        if (iterator == device_status_by_id_.end()) {
            continue;
        }
        device_status_index_by_id_[device_id] = system_status_.device_status_list.size();
        system_status_.device_status_list.push_back(build_device_health_status(iterator->second));
        if (iterator->second.online) {
            ++system_status_.online_device_count;
        }
    }
}

}  // namespace edge_controller
