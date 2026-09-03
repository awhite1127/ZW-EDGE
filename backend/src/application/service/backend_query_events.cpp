// 查询、筛选和清理服务事件。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "application/service/backend_service.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "shared/common/time_utils.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

// 获取指定通道最近通讯报文。
std::vector<CommunicationTraceRecord> BackendService::get_channel_communication_traces(
    const ChannelId& channel_id,
    std::size_t limit) const
{
    return communication_trace_store_.get_by_channel(channel_id, limit);
}

// 清空指定通道通讯报文缓存。
void BackendService::clear_channel_communication_traces(const ChannelId& channel_id)
{
    communication_trace_store_.clear_channel(channel_id);
}

// 获取最近轮询摘要。
PollingCycleSummary BackendService::get_recent_polling_summary() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (polling_service_ != nullptr) {
        return polling_service_->get_last_cycle_summary();
    }
    return data_store_.get_polling_summary();
}

// 获取最近错误摘要。
ServiceErrorSummary BackendService::get_recent_error_summary() const
{
    std::shared_lock<std::shared_mutex> service_lock(service_mutex_);

    const auto current_error = backend_internal::build_current_error_summary(data_store_.get_system_status());
    if (current_error.has_error) {
        return current_error;
    }

    return {};
}

// 获取最近后端事件列表。
std::vector<ServiceEvent> BackendService::get_recent_events(std::size_t limit) const
{
    std::vector<ServiceEvent> events;
    std::string ignored;
    event_store_.list_recent(limit, &events, &ignored);
    return events;
}

// 获取最近事件。
StatusCode BackendService::get_recent_events(
    std::size_t limit,
    std::vector<ServiceEvent>* events,
    std::string* error_message) const
{
    return event_store_.list_recent(limit, events, error_message);
}

// 读取历史事件页；筛选、分页和统计的一致性由 EventStore 保证。
StatusCode BackendService::query_service_events(
    const EventHistoryQuery& query,
    EventHistoryResult* result,
    std::string* error_message) const
{
    return event_store_.query_history(query, result, error_message);
}

// 分页读取历史事件导出记录。
StatusCode BackendService::export_service_events(
    const EventExportQuery& query,
    std::vector<ServiceEvent>* events,
    std::string* error_message) const
{
    return event_store_.export_events(query, events, error_message);
}

// 清空持久化历史事件。
StatusCode BackendService::clear_recent_events(std::string* error_message)
{
    return event_store_.clear_all(error_message);
}

}  // namespace edge_controller
