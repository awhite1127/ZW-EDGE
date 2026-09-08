// 通道管理器拥有串口/TCP 通道实例，统一控制初始化、按需打开、状态快照和批量关闭。
#include "application/manager/channel_manager.h"

#include "communication/channel/serial_channel.h"
#include "communication/channel/tcp_channel.h"
#include "shared/common/logger.h"
#include "shared/common/time_utils.h"

#include <algorithm>
#include <map>

namespace edge_controller {

namespace {

std::string communication_resource_key(const ChannelConfig& config)
{
return config.channel_type == ChannelType::kModbusRtuSerial
            ? (!config.device_path.empty()
                   ? "serial:" + config.device_path
                   : (!config.port_name.empty() ? "serial:" + config.port_name : "channel:" + config.channel_id))
            : "channel:" + config.channel_id;
}

// 计算指定时间点至今的毫秒数。
std::uint32_t elapsed_ms_since(TimestampMs started_at_ms)
{
    const auto finished_at_ms = time_utils::steady_now_ms();
    return finished_at_ms > started_at_ms ? static_cast<std::uint32_t>(finished_at_ms - started_at_ms) : 0U;
}

// 生成串口配置显示文本。
std::string serial_config_text(const ChannelConfig& config)
{
    return " baud_rate=" + std::to_string(config.baud_rate) +
           " data_bits=" + std::to_string(config.data_bits) +
           " stop_bits=" + std::to_string(config.stop_bits) +
           " parity=" + std::string(to_string(config.parity));
}

}  // namespace

// 根据配置初始化通道实例集合。
StatusCode ChannelManager::initialize(const std::vector<ChannelConfig>& channels, std::string* error_message)
{
    std::unordered_map<ChannelId, std::shared_ptr<IChannel>> next_channels;
    std::unordered_map<ChannelId, std::shared_ptr<std::mutex>> next_communication_mutexes;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> mutexes_by_resource;
    next_channels.reserve(channels.size());
    next_communication_mutexes.reserve(channels.size());

    for (const auto& config : channels) {
        if (next_channels.find(config.channel_id) != next_channels.end()) {
            if (error_message != nullptr) {
                *error_message = "通道管理器中存在重复通道 ID: " + config.channel_id;
            }
            return StatusCode::kInvalidArgument;
        }

        if (config.channel_type == ChannelType::kModbusRtuSerial) {
            next_channels[config.channel_id] = std::make_unique<SerialChannel>(config);
        } else if (config.channel_type == ChannelType::kModbusTcp) {
            next_channels[config.channel_id] = std::make_unique<TcpChannel>(config);
        } else {
            if (error_message != nullptr) {
                *error_message = "不支持的通道类型: " + config.channel_id;
            }
            return StatusCode::kInvalidArgument;
        }

        const auto resource_key = communication_resource_key(config);
        auto& resource_mutex = mutexes_by_resource[resource_key];
        if (resource_mutex == nullptr) {
            resource_mutex = std::make_shared<std::mutex>();
        }
        next_communication_mutexes[config.channel_id] = resource_mutex;
    }

    generations_.clear();
    for (const auto& config : channels) generations_[config.channel_id] = 1;
    resource_mutexes_ = std::move(mutexes_by_resource);
    channels_ = std::move(next_channels);
    communication_mutexes_ = std::move(next_communication_mutexes);
    return StatusCode::kOk;
}

// 打开全部启用通道。
StatusCode ChannelManager::open_enabled_channels(ChannelOpenSummary* summary)
{
    return open_enabled_channels_filtered(nullptr, summary);
}

// 打开指定集合中的启用通道。
StatusCode ChannelManager::open_enabled_channels(
    const std::vector<ChannelId>& channel_ids,
    ChannelOpenSummary* summary)
{
    return open_enabled_channels_filtered(&channel_ids, summary);
}

// 按过滤条件执行启用通道打开流程。
StatusCode ChannelManager::open_enabled_channels_filtered(
    const std::vector<ChannelId>* channel_ids,
    ChannelOpenSummary* summary)
{
    ChannelOpenSummary local_summary;
    if (summary == nullptr) {
        summary = &local_summary;
    }

    *summary = {};

    for (auto& entry : channels_) {
        auto& channel = entry.second;
        const auto& config = channel->config();
        if (!config.enabled) {
            continue;
        }
        if (channel_ids != nullptr &&
            std::find(channel_ids->begin(), channel_ids->end(), config.channel_id) == channel_ids->end()) {
            if (Logger::debug_enabled()) {
                Logger::debug(
                    "通道 " + config.channel_id +
                    " 未绑定启用采集目标，启动阶段不预打开，目标=" +
                    channel_target_description(config));
            }
            continue;
        }

        ++summary->enabled_count;
        const auto status = channel->open();
        if (is_ok(status)) {
            ++summary->opened_count;
            continue;
        }

        ++summary->failed_count;
        ChannelOpenFailure failure;
        failure.channel_id = config.channel_id;
        failure.device_path = channel_target_description(config);
        failure.status = status;
        failure.error_message = channel->status().last_error_message;
        failure.attempt_time_ms = time_utils::system_now_ms();
        summary->failures.push_back(failure);

        Logger::warn(
            "通道 " + failure.channel_id +
            " 目标=" + failure.device_path +
            " 启动时打开失败，已标记为不可用: " + failure.error_message);
    }

    return StatusCode::kOk;
}

// 关闭所有通道。
void ChannelManager::close_all()
{
    for (auto& entry : channels_) {
        entry.second->close();
    }
}

// 为 RTU 采集准备目标通道；已打开的串口只刷新缓冲区，不再按轮询周期重开。
StatusCode ChannelManager::prepare_rtu_channel_for_collection(
    const ChannelId& channel_id,
    std::string* error_message)
{
    const auto started_at_ms = time_utils::steady_now_ms();
    auto* channel = get_channel(channel_id);
    if (channel == nullptr) {
        if (error_message != nullptr) {
            *error_message = "未找到 RTU 通道: " + channel_id;
        }
        return StatusCode::kNotFound;
    }

    const auto& config = channel->config();
    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_channel_prepare_start channel_id=" + config.channel_id +
            " channel_name=" + config.channel_name +
            " serial_device=" + channel_target_description(config) +
            serial_config_text(config));
    }

    if (config.channel_type != ChannelType::kModbusRtuSerial) {
        if (error_message != nullptr) {
            *error_message = "目标通道不是 RTU 串口通道: " + channel_id;
        }
        return StatusCode::kInvalidArgument;
    }
    if (!config.enabled) {
        if (error_message != nullptr) {
            *error_message = "RTU 通道未启用: " + channel_id;
        }
        return StatusCode::kInvalidState;
    }

    const bool reused_open_channel = channel->is_open();
    if (!reused_open_channel) {
        if (Logger::debug_enabled()) {
            Logger::debug(
                "rtu_channel_open_start channel_id=" + config.channel_id +
                " channel_name=" + config.channel_name +
                " serial_device=" + channel_target_description(config) +
                serial_config_text(config));
        }
        const auto open_status = channel->open();
        if (!is_ok(open_status)) {
            const auto status = channel->status();
            if (error_message != nullptr) {
                *error_message = status.last_error_message.empty()
                                     ? "打开 RTU 通道失败: " + channel_id
                                     : status.last_error_message;
            }
            if (Logger::debug_enabled()) {
                Logger::debug(
                    "rtu_channel_open_done channel_id=" + config.channel_id +
                    " serial_device=" + channel_target_description(config) +
                    " fd=" + std::to_string(channel->native_handle()) +
                    " success=false error=\"" +
                    (error_message == nullptr ? std::string{} : *error_message) +
                    "\" elapsed_ms=" +
                    std::to_string(elapsed_ms_since(started_at_ms)));
            }
            return open_status;
        }
    }

    const auto flush_status = channel->flush();
    if (!is_ok(flush_status)) {
        const auto status = channel->status();
        if (error_message != nullptr) {
            *error_message = status.last_error_message.empty()
                                 ? "刷新 RTU 通道缓冲区失败: " + channel_id
                                 : status.last_error_message;
        }
        if (Logger::debug_enabled()) {
            Logger::debug(
                "rtu_channel_flush_done channel_id=" + config.channel_id +
                " serial_device=" + channel_target_description(config) +
                " fd=" + std::to_string(channel->native_handle()) +
                " success=false error=\"" +
                (error_message == nullptr ? std::string{} : *error_message) +
                "\" elapsed_ms=" +
                std::to_string(elapsed_ms_since(started_at_ms)));
        }
        channel->close();
        return flush_status;
    }

    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_channel_prepare_done channel_id=" + config.channel_id +
            " serial_device=" + channel_target_description(config) +
            " fd=" + std::to_string(channel->native_handle()) +
            " reused=" + std::string(reused_open_channel ? "true" : "false") +
            " success=true elapsed_ms=" + std::to_string(elapsed_ms_since(started_at_ms)));
    }
    return StatusCode::kOk;
}

ChannelManager::CommunicationLease ChannelManager::acquire_communication_lease(
    const ChannelId& channel_id)
{
    for (;;) {
        std::shared_ptr<std::mutex> resource;
        {
            std::shared_lock<std::shared_mutex> index_lock(*index_mutex_);
            const auto iterator = communication_mutexes_.find(channel_id);
            if (iterator == communication_mutexes_.end()) return {};
            resource = iterator->second;
        }
        CommunicationLease lease(resource);
        // 等待期间配置可能已将通道重绑定到另一物理资源。
        std::shared_lock<std::shared_mutex> index_lock(*index_mutex_);
        const auto current = communication_mutexes_.find(channel_id);
        if (current == communication_mutexes_.end()) return {};
        if (current->second == resource) return lease;
    }
}

StatusCode ChannelManager::apply_channels(const std::vector<ChannelConfig>& configs,
    const std::function<StatusCode()>& persist, std::string* error)
{
    decltype(channels_) next_channels;
    decltype(communication_mutexes_) next_mutexes;
    decltype(resource_mutexes_) next_resources;
    decltype(generations_) next_generations;
    std::map<std::mutex*, std::shared_ptr<std::mutex>> affected_resources;
    std::vector<std::shared_ptr<IChannel>> retired;
    {
        std::shared_lock<std::shared_mutex> index_lock(*index_mutex_);
        for (const auto& config : configs) {
            const auto key = communication_resource_key(config);
            auto& resource = next_resources[key];
            if (!resource) {
                const auto old = resource_mutexes_.find(key);
                resource = old == resource_mutexes_.end() ? std::make_shared<std::mutex>() : old->second;
            }
            next_mutexes[config.channel_id] = resource;
            const auto previous = channels_.find(config.channel_id);
            if (previous != channels_.end() && channel_transport_key(previous->second->config()) == channel_transport_key(config)) {
                next_channels[config.channel_id] = previous->second;
                next_generations[config.channel_id] = generations_.at(config.channel_id);
                continue;
            }
            if (config.channel_type == ChannelType::kModbusRtuSerial)
                next_channels[config.channel_id] = std::make_shared<SerialChannel>(config);
            else if (config.channel_type == ChannelType::kModbusTcp)
                next_channels[config.channel_id] = std::make_shared<TcpChannel>(config);
            else { if (error) *error = "不支持的通道类型"; return StatusCode::kInvalidArgument; }
            next_generations[config.channel_id] = previous == channels_.end() ? 1 : generations_.at(config.channel_id) + 1;
            affected_resources[resource.get()] = resource;
        }
        for (const auto& old : channels_) {
            const auto current = next_channels.find(old.first);
            if (current != next_channels.end() && current->second == old.second) continue;
            const auto resource = communication_mutexes_.at(old.first);
            affected_resources[resource.get()] = resource;
            retired.push_back(old.second);
        }
    }
    // 所有分配均在提交前完成；只等待改变涉及的旧/新物理资源。
    std::vector<CommunicationLease> leases;
    leases.reserve(affected_resources.size());
    for (const auto& resource : affected_resources) leases.emplace_back(resource.second);
    const auto status = persist();
    if (!is_ok(status)) return status;
    {
        std::unique_lock<std::shared_mutex> index_lock(*index_mutex_);
        channels_.swap(next_channels);
        communication_mutexes_.swap(next_mutexes);
        resource_mutexes_.swap(next_resources);
        generations_.swap(next_generations);
    }
    for (const auto& old : retired) old->close();
    return StatusCode::kOk;
}

std::uint64_t ChannelManager::generation(const ChannelId& id) const
{
    std::shared_lock<std::shared_mutex> lock(*index_mutex_);
    const auto found = generations_.find(id);
    return found == generations_.end() ? 0 : found->second;
}

IChannel* ChannelManager::get_channel(const ChannelId& channel_id)
{
    std::shared_lock<std::shared_mutex> index_lock(*index_mutex_);
    const auto iterator = channels_.find(channel_id);
    if (iterator == channels_.end()) {
        return nullptr;
    }
    return iterator->second.get();
}

const ChannelStatus* ChannelManager::get_channel_status(const ChannelId& channel_id) const
{
    std::shared_lock<std::shared_mutex> index_lock(*index_mutex_);
    const auto iterator = channels_.find(channel_id);
    if (iterator == channels_.end()) {
        return nullptr;
    }
    thread_local ChannelStatus status_snapshot;
    status_snapshot = iterator->second->status();
    return &status_snapshot;
}

std::vector<ChannelStatus> ChannelManager::snapshot_statuses() const
{
    std::shared_lock<std::shared_mutex> index_lock(*index_mutex_);
    std::vector<ChannelStatus> statuses;
    statuses.reserve(channels_.size());

    for (const auto& entry : channels_) {
        statuses.push_back(entry.second->status());
    }

    return statuses;
}

}  // namespace edge_controller
