// 拥有并管理所有具体通道实例，负责批量打开、查询和关闭。
// 边界：只维护资源所有权和拓扑索引，不复制协议或业务规则。

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "communication/channel/i_channel.h"
#include "shared/common/status_code.h"
#include "data/model/channel_config.h"
#include "data/model/channel_status.h"

namespace edge_controller {

struct ChannelOpenFailure {
    ChannelId channel_id;
    std::string device_path;
    StatusCode status{StatusCode::kOk};
    std::string error_message;
    TimestampMs attempt_time_ms{0};
};

struct ChannelOpenSummary {
    std::size_t enabled_count{0};
    std::size_t opened_count{0};
    std::size_t failed_count{0};
    std::vector<ChannelOpenFailure> failures;
};

class ChannelManager {
public:
    using CommunicationLease = std::unique_lock<std::mutex>;
    // 根据通道配置创建串口或 TCP 通道实例。
    StatusCode initialize(const std::vector<ChannelConfig>& channels, std::string* error_message = nullptr);
    // 打开所有启用通道，并返回打开结果摘要。
    StatusCode open_enabled_channels(ChannelOpenSummary* summary = nullptr);
    // 只打开指定 ID 集合中的启用通道。
    StatusCode open_enabled_channels(
        const std::vector<ChannelId>& channel_ids,
        ChannelOpenSummary* summary = nullptr);
    // 关闭并释放所有已管理通道。
    void close_all();
    // 为 RTU 主站采集准备指定通道；已打开 fd 复用并在本轮请求前刷新缓冲。
    StatusCode prepare_rtu_channel_for_collection(
        const ChannelId& channel_id,
        std::string* error_message = nullptr);
    // 独占指定通道实际使用的通讯资源；同一物理串口路径共享锁，TCP 按通道隔离。
    CommunicationLease acquire_communication_lease(const ChannelId& channel_id);

    // 按通道 ID 获取通道收发接口。
    IChannel* get_channel(const ChannelId& channel_id);
    // 按通道 ID 获取最近运行状态。
    const ChannelStatus* get_channel_status(const ChannelId& channel_id) const;
    // 快照当前所有通道状态，供页面和诊断查询使用。
    std::vector<ChannelStatus> snapshot_statuses() const;

private:
    // 根据过滤条件打开启用通道，是公开打开接口的统一实现。
    StatusCode open_enabled_channels_filtered(
        const std::vector<ChannelId>* channel_ids,
        ChannelOpenSummary* summary);

    std::unordered_map<ChannelId, std::unique_ptr<IChannel>> channels_{};
    std::unordered_map<ChannelId, std::shared_ptr<std::mutex>> communication_mutexes_{};
};

}  // namespace edge_controller
