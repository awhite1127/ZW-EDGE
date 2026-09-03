// 有界通讯报文内存仓库接口。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "shared/common/types.h"
#include "data/model/communication_trace.h"

namespace edge_controller {

class CommunicationTraceStore {
public:
    // 构造 CommunicationTraceStore 实例。
    explicit CommunicationTraceStore(std::size_t max_records_per_channel = 50);

    // 追加。
    void append(CommunicationTraceRecord record);
    // 获取按通道。
    std::vector<CommunicationTraceRecord> get_by_channel(
        const ChannelId& channel_id,
        std::size_t limit = 50) const;
    // 清空通道。
    void clear_channel(const ChannelId& channel_id);
    // 清空全部。
    void clear_all();

private:
    std::size_t max_records_per_channel_{50};
    std::uint64_t next_sequence_{1};
    // 多通道采集线程并发追加报文；锁内只维护有界内存队列，不执行设备或磁盘 I/O。
    mutable std::mutex mutex_;
    std::unordered_map<ChannelId, std::deque<CommunicationTraceRecord>> records_by_channel_;
};

}  // namespace edge_controller
