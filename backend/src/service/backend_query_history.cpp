// 组装单设备历史视图并提供分层历史查询。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "service/backend_service.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include "common/time_utils.h"
#include "common/logger.h"
#include "service/backend_service_internal.h"

namespace edge_controller {

namespace {

using HistoryEnabledPointKeys = std::unordered_set<std::string>;

struct HistoryEnabledIndex {
    std::unordered_map<DeviceId, std::string> template_by_device;
    std::unordered_map<std::string, HistoryEnabledPointKeys> points_by_template;
};

// 构造单个设备类型的历史启用点位集合。
HistoryEnabledPointKeys build_history_enabled_point_keys(const std::string& template_id)
{
    HistoryEnabledPointKeys keys;
    const auto definition = find_device_template(template_id);
    if (definition == nullptr) return keys;
    keys.reserve(definition->fields.size());
    for (const auto& field : definition->fields) {
        if (device_template_field_history_enabled(field)) {
            keys.insert(field.field_key);
        }
    }
    return keys;
}

// 构建设备到模板、模板到历史点位的两级索引；共享模板只解析一次。
HistoryEnabledIndex build_history_enabled_index(const SystemConfig& config)
{
    std::unordered_map<MasterNodeId, std::string> template_by_master;
    template_by_master.reserve(config.master_nodes.size());
    for (const auto& master : config.master_nodes) {
        template_by_master[master.master_id] = master.device_template;
    }
    HistoryEnabledIndex result;
    result.template_by_device.reserve(config.devices.size());
    result.points_by_template.reserve(config.master_nodes.size());
    for (const auto& device : config.devices) {
        const auto master = template_by_master.find(device.master_id);
        if (master == template_by_master.end()) continue;
        result.template_by_device[device.device_id] = master->second;
        if (result.points_by_template.find(master->second) == result.points_by_template.end()) {
            result.points_by_template.emplace(
                master->second,
                build_history_enabled_point_keys(master->second));
        }
    }
    return result;
}

// 判断历史记录是否仍允许展示。
bool history_point_enabled(
    const HistoryEnabledIndex& enabled,
    const DeviceId& device_id,
    const std::string& point_key)
{
    const auto device = enabled.template_by_device.find(device_id);
    if (device == enabled.template_by_device.end()) return false;
    const auto points = enabled.points_by_template.find(device->second);
    return points != enabled.points_by_template.end() &&
           points->second.find(point_key) != points->second.end();
}

// 判断历史记录是否仍允许展示。
bool history_record_enabled(const HistoryEnabledIndex& enabled, const HistoryRecord& record)
{
    return history_point_enabled(enabled, record.device_id, record.point_key);
}

}  // namespace

// 获取单设备历史页聚合视图。
StatusCode BackendService::get_device_history_view(
    const DeviceHistoryQuery& query,
    DeviceHistoryView* view,
    std::string* error_message) const
{
    if (view == nullptr) {
        if (error_message != nullptr) {
            *error_message = "历史页输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (query.device_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "设备ID不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    DeviceConfig device;
    MasterNodeConfig master;
    ChannelConfig channel;
    HistoryEnabledPointKeys history_enabled_points;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        const auto* device_config = find_device_config(query.device_id);
        if (device_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备不存在或已被删除：" + query.device_id;
            }
            return StatusCode::kNotFound;
        }
        device = *device_config;

        const auto* master_config = find_master_config(device.master_id);
        if (master_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备所属主控不存在：" + device.master_id;
            }
            return StatusCode::kNotFound;
        }
        master = *master_config;
        // 单设备历史页只需当前模板的点位集合，不为每次查询遍历并复制全拓扑索引。
        history_enabled_points = build_history_enabled_point_keys(master.device_template);

        const auto* channel_config = find_channel_config(master.channel_id);
        if (channel_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "设备所属通道不存在：" + master.channel_id;
            }
            return StatusCode::kNotFound;
        }
        channel = *channel_config;
    }

    DeviceHistoryView result;
    result.device = std::move(device);
    result.master = std::move(master);
    result.channel = std::move(channel);
    std::vector<HistoryRecord> history_records;
    std::vector<HistoryPointSummary> history_points;
    std::string history_error;
    HistoryQueryOptions options;
    options.days = query.days;
    options.start_date = query.start_date;
    options.end_date = query.end_date;
    options.point_key = query.point_key;
    options.sample_period = query.sample_period;
    const auto history_status = history_store_.get_device_history(query.device_id, options, &history_records, &history_error);
    if (!is_ok(history_status)) {
        if (error_message != nullptr) {
            *error_message = history_error.empty() ? "edge-history.db 历史数据查询失败" : history_error;
        }
        return history_status;
    }
    const auto points_status = history_store_.get_device_history_points(
        query.device_id,
        options.sample_period,
        &history_points,
        &history_error);
    if (!is_ok(points_status)) {
        if (error_message != nullptr) {
            *error_message = history_error.empty() ? "edge-history.db 历史点位查询失败" : history_error;
        }
        return points_status;
    }

    history_records.erase(std::remove_if(history_records.begin(), history_records.end(), [&](const auto& record) {
        return history_enabled_points.find(record.point_key) == history_enabled_points.end();
    }), history_records.end());
    history_points.erase(std::remove_if(history_points.begin(), history_points.end(), [&](const auto& point) {
        return history_enabled_points.find(point.point_key) == history_enabled_points.end();
    }), history_points.end());

    result.history_records = std::move(history_records);
    result.history_points = std::move(history_points);
    result.stats.record_count = result.history_records.size();
    if (!result.history_records.empty()) {
        result.stats.earliest_date = result.history_records.front().date;
        result.stats.latest_date = result.history_records.back().date;
    }

    *view = std::move(result);
    return StatusCode::kOk;
}

// 获取历史数据概览摘要。
StatusCode BackendService::get_history_overview_summaries(
    std::vector<HistoryOverviewSummary>* summaries,
    std::string* error_message) const
{
    if (summaries == nullptr) return StatusCode::kInvalidArgument;
    HistoryEnabledIndex history_enabled;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        history_enabled = build_history_enabled_index(system_config_);
    }
    const auto status = history_store_.get_history_overview_summaries(summaries, error_message);
    if (!is_ok(status)) return status;
    summaries->erase(std::remove_if(summaries->begin(), summaries->end(), [&](const auto& summary) {
        return !history_point_enabled(history_enabled, summary.device_id, summary.point_key);
    }), summaries->end());
    return StatusCode::kOk;
}

// 分页读取历史采样导出记录。
StatusCode BackendService::export_history_records(
    const HistoryExportQuery& query,
    std::vector<HistoryRecord>* records,
    std::string* error_message) const
{
    if (records == nullptr) return StatusCode::kInvalidArgument;
    records->clear();
    HistoryEnabledIndex history_enabled;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        history_enabled = build_history_enabled_index(system_config_);
    }

    const auto requested_limit = query.limit == 0 ? 500U : std::min(query.limit, 5000U);
    std::uint32_t enabled_offset = 0;
    HistoryExportQuery page_query = query;
    page_query.limit = 5000;
    page_query.offset = 0;
    while (records->size() < requested_limit) {
        std::vector<HistoryRecord> page;
        const auto status = history_store_.export_records(page_query, &page, error_message);
        if (!is_ok(status)) return status;
        for (auto& record : page) {
            if (!history_record_enabled(history_enabled, record)) continue;
            if (enabled_offset++ < query.offset) continue;
            records->push_back(std::move(record));
            if (records->size() >= requested_limit) break;
        }
        if (page.size() < page_query.limit || records->size() >= requested_limit) break;
        page_query.offset += static_cast<std::uint32_t>(page.size());
    }
    return StatusCode::kOk;
}

// 读取数据维护摘要。
StatusCode BackendService::get_data_maintenance_summary(
    DataMaintenanceSummary* summary,
    std::string* error_message) const
{
    if (summary == nullptr) {
        if (error_message != nullptr) *error_message = "数据维护摘要输出参数为空";
        return StatusCode::kInvalidArgument;
    }
    DataMaintenanceSummary result;
    {
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        result = data_maintenance_summary_;
    }
    std::string count_error;
    auto status = history_store_.count_by_period("raw_10min", &result.current_raw_10min_count, &count_error);
    if (is_ok(status)) status = history_store_.count_by_period("hour", &result.current_hour_count, &count_error);
    if (is_ok(status)) status = history_store_.count_by_period("day", &result.current_day_count, &count_error);
    if (is_ok(status)) status = event_store_.record_count(&result.current_event_count, &count_error);
    if (!is_ok(status)) {
        if (error_message != nullptr) *error_message = count_error.empty() ? "统计数据维护数量失败" : count_error;
        return status;
    }
    *summary = std::move(result);
    return StatusCode::kOk;
}

// 清理超过保留期限的历史、事件和报警数据。
StatusCode BackendService::cleanup_expired_data(
    DataMaintenanceSummary* summary,
    std::string* error_message)
{
    HistoryCleanupResult history_result;
    std::uint64_t deleted_events = 0;
    std::string cleanup_error;
    auto status = history_store_.cleanup_expired(&history_result, &cleanup_error);
    if (is_ok(status)) status = event_store_.cleanup_expired(&deleted_events, &cleanup_error);

    DataMaintenanceSummary result;
    result.last_cleanup_time_ms = time_utils::system_now_ms();
    result.last_cleanup_success = is_ok(status);
    result.deleted_raw_10min_count = history_result.deleted_raw_10min_count;
    result.deleted_hour_count = history_result.deleted_hour_count;
    result.deleted_day_count = history_result.deleted_day_count;
    result.deleted_event_count = deleted_events;
    const auto total_deleted = result.deleted_raw_10min_count + result.deleted_hour_count +
                               result.deleted_day_count + result.deleted_event_count;
    if (is_ok(status)) {
        std::ostringstream message;
        message << "已清理超期数据 " << total_deleted << " 条（10 分钟样本 "
                << result.deleted_raw_10min_count << " 条、小时聚合 " << result.deleted_hour_count
                << " 条、天聚合 " << result.deleted_day_count << " 条、历史事件 "
                << result.deleted_event_count << " 条）";
        result.last_cleanup_result = message.str();
    } else {
        result.last_cleanup_result = cleanup_error.empty() ? "清理超期数据失败" : cleanup_error;
    }
    {
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        data_maintenance_summary_ = result;
    }
    if (!is_ok(status)) {
        Logger::error("数据维护清理失败，不影响采集继续运行：" + result.last_cleanup_result);
        if (error_message != nullptr) *error_message = result.last_cleanup_result;
        if (summary != nullptr) *summary = result;
        return status;
    }

    append_event(
        "info", "system", "data_maintenance", "数据维护：" + result.last_cleanup_result,
        "仅删除超过保存期限的数据，采集配置、告警规则和实时数据未受影响。",
        result.last_cleanup_time_ms);
    DataMaintenanceSummary counted;
    status = get_data_maintenance_summary(&counted, error_message);
    if (is_ok(status)) result = counted;
    if (summary != nullptr) *summary = result;
    return status;
}

}  // namespace edge_controller
