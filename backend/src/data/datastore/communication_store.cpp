// 通讯报文仓库保存有限条最近请求/响应记录，避免诊断数据无界占用内存。
#include "data/datastore/communication_store.h"

#include <algorithm>
#include <utility>

namespace edge_controller {

namespace {

constexpr std::size_t kDefaultMaxRecordsPerChannel = 50;
constexpr std::size_t kMaxRecordsPerChannel = 50;

// 规范化输入并返回稳定结果。
std::size_t normalized_max_records_per_channel(std::size_t max_records_per_channel)
{
    if (max_records_per_channel == 0 || max_records_per_channel > kMaxRecordsPerChannel) {
        return kDefaultMaxRecordsPerChannel;
    }
    return max_records_per_channel;
}

// 规范化输入并返回稳定结果。
std::size_t normalized_limit(std::size_t limit, std::size_t max_records_per_channel)
{
    const auto effective_limit = limit == 0 ? max_records_per_channel : limit;
    return std::min(effective_limit, max_records_per_channel);
}

}  // namespace

CommunicationTraceStore::CommunicationTraceStore(std::size_t max_records_per_channel)
    : max_records_per_channel_(normalized_max_records_per_channel(max_records_per_channel))
{
}

// 追加一条通讯报文记录并执行容量裁剪。
void CommunicationTraceStore::append(CommunicationTraceRecord record)
{
    if (record.channel_id.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    record.sequence = next_sequence_++;

    auto& records = records_by_channel_[record.channel_id];
    records.push_back(std::move(record));
    while (records.size() > max_records_per_channel_) {
        records.pop_front();
    }
}

// 返回指定通道的通讯报文记录。
std::vector<CommunicationTraceRecord> CommunicationTraceStore::get_by_channel(
    const ChannelId& channel_id,
    std::size_t limit) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = records_by_channel_.find(channel_id);
    if (iterator == records_by_channel_.end()) {
        return {};
    }

    const auto& records = iterator->second;
    const auto count = std::min(normalized_limit(limit, max_records_per_channel_), records.size());
    std::vector<CommunicationTraceRecord> result;
    result.reserve(count);

    const auto start_offset = records.size() - count;
    for (std::size_t index = start_offset; index < records.size(); ++index) {
        result.push_back(records[index]);
    }
    return result;
}

// 清空通道。
void CommunicationTraceStore::clear_channel(const ChannelId& channel_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    records_by_channel_.erase(channel_id);
}

// 清空全部。
void CommunicationTraceStore::clear_all()
{
    std::lock_guard<std::mutex> lock(mutex_);
    records_by_channel_.clear();
}

}  // namespace edge_controller
