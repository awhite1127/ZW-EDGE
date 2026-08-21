// 连接告警存储、模板点位上下文和活动告警展示。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "service/backend_service.h"

#include <algorithm>

#include "common/logger.h"
#include "common/time_utils.h"
#include "service/backend_service_internal.h"

namespace edge_controller {

// 在持锁状态下构造全部可配置告警点位上下文。
std::vector<AlarmPointContext> BackendService::build_alarm_point_contexts_locked() const
{
    // 告警规则只允许绑定模板中的关键数据；上下文列表就是规则校验和展示的白名单。
    std::vector<AlarmPointContext> contexts;
    for (const auto& device : system_config_.devices) {
        const auto* master = backend_internal::find_master_config_in_list(system_config_.master_nodes, device.master_id);
        if (master == nullptr) continue;
        const auto definition = find_device_template(master->device_template);
        if (definition == nullptr) continue;
        for (const auto& field : definition->fields) {
            if (!field.summary) continue;
            contexts.push_back({device.device_id, device.device_name, master->master_id, definition->template_id,
                                field.field_key, field.display_name, field.unit, field.precision});
        }
    }
    return contexts;
}

// 按设备和点位键查找告警上下文。
const AlarmPointContext* BackendService::find_alarm_point_context(
    const std::vector<AlarmPointContext>& contexts,
    const DeviceId& device_id,
    const std::string& point_key) const
{
    const auto iterator = std::find_if(contexts.begin(), contexts.end(), [&](const AlarmPointContext& context) {
        return context.device_id == device_id && context.point_key == point_key;
    });
    return iterator == contexts.end() ? nullptr : &*iterator;
}

// 在持锁状态下同步告警拓扑。
void BackendService::synchronize_alarm_topology_locked(const std::string& reason)
{
    if (!alarm_evaluator_.initialized()) return;
    // 主站、设备或模板变化后同步告警拓扑，让已删除点位的规则自动失效并产生恢复事件。
    std::string error;
    if (!is_ok(alarm_evaluator_.synchronize_topology(build_alarm_point_contexts_locked(), reason, &error))) {
        Logger::error("告警规则拓扑协调失败：" + error);
        set_last_error("alarm_topology", "", "告警规则拓扑协调失败: " + error, time_utils::system_now_ms());
    }
}

// 列出告警规则。
StatusCode BackendService::list_alarm_rules(std::vector<AlarmRule>* rules, std::string* error_message) const
{
    if (rules == nullptr) { if (error_message) *error_message = "告警规则输出参数为空"; return StatusCode::kInvalidArgument; }
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) { if (error_message) *error_message = "后端服务尚未初始化"; return StatusCode::kInvalidState; }
    if (config_apply_in_progress_.load()) { if (error_message) *error_message = "配置正在应用中"; return StatusCode::kInvalidState; }
    *rules = alarm_evaluator_.list_rules();
    return StatusCode::kOk;
}

// 列出设备告警规则。
StatusCode BackendService::list_device_alarm_rules(const DeviceId& device_id, std::vector<AlarmRule>* rules, std::string* error_message) const
{
    if (device_id.empty() || rules == nullptr) { if (error_message) *error_message = "device_id 和输出参数不能为空"; return StatusCode::kInvalidArgument; }
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) { if (error_message) *error_message = "后端服务尚未初始化"; return StatusCode::kInvalidState; }
    if (config_apply_in_progress_.load()) { if (error_message) *error_message = "配置正在应用中"; return StatusCode::kInvalidState; }
    const auto contexts = build_alarm_point_contexts_locked();
    if (std::none_of(contexts.begin(), contexts.end(), [&](const AlarmPointContext& value) { return value.device_id == device_id; })) {
        if (error_message) *error_message = "设备不存在: " + device_id;
        return StatusCode::kNotFound;
    }
    *rules = alarm_evaluator_.list_rules(device_id);
    return StatusCode::kOk;
}

// 新增或更新告警规则。
StatusCode BackendService::upsert_alarm_rule(const AlarmRule& requested, AlarmRule* saved_rule, std::string* message, std::string* error_message)
{
    if (saved_rule == nullptr) { if (error_message) *error_message = "保存结果参数为空"; return StatusCode::kInvalidArgument; }
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) { if (error_message) *error_message = "后端服务尚未初始化"; return StatusCode::kInvalidState; }
    if (config_apply_in_progress_.load()) { if (error_message) *error_message = "配置正在应用中"; return StatusCode::kInvalidState; }
    const auto contexts = build_alarm_point_contexts_locked();
    // 先校验设备存在，再校验点位存在，错误提示能直接对应前端当前选择。
    if (std::none_of(contexts.begin(), contexts.end(), [&](const AlarmPointContext& value) { return value.device_id == requested.device_id; })) {
        if (error_message) *error_message = "设备不存在: " + requested.device_id;
        return StatusCode::kNotFound;
    }
    const auto* context = find_alarm_point_context(contexts, requested.device_id, requested.point_key);
    if (context == nullptr) { if (error_message) *error_message = "设备数据项不存在: " + requested.point_key; return StatusCode::kNotFound; }
    if (requested.trigger_count < 1 || requested.trigger_count > 100 || requested.recovery_count < 1 || requested.recovery_count > 100) {
        if (error_message) *error_message = "trigger_count 和 recovery_count 必须在 1～100 之间";
        return StatusCode::kInvalidArgument;
    }
    auto final_rule = requested;
    final_rule.updated_at_ms = time_utils::system_now_ms();
    const auto status = alarm_evaluator_.upsert_rule(final_rule, *context, error_message);
    if (!is_ok(status)) return status;
    *saved_rule = final_rule;
    if (message) *message = final_rule.enabled ? "告警规则已保存" : "告警规则已停用";
    return StatusCode::kOk;
}

// 删除告警规则。
StatusCode BackendService::delete_alarm_rule(const DeviceId& device_id, const std::string& point_key, std::string* message, std::string* error_message)
{
    if (device_id.empty() || point_key.empty()) { if (error_message) *error_message = "device_id 和 point_key 不能为空"; return StatusCode::kInvalidArgument; }
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) { if (error_message) *error_message = "后端服务尚未初始化"; return StatusCode::kInvalidState; }
    if (config_apply_in_progress_.load()) { if (error_message) *error_message = "配置正在应用中"; return StatusCode::kInvalidState; }
    const auto status = alarm_evaluator_.delete_rule(device_id, point_key, "规则删除", error_message);
    if (!is_ok(status)) return status;
    if (message) *message = "告警规则已删除";
    return StatusCode::kOk;
}

// 返回后端当前活动报警列表。
StatusCode BackendService::list_active_alarms(std::vector<ActiveAlarmView>* alarms, std::string* error_message) const
{
    if (alarms == nullptr) { if (error_message) *error_message = "活动告警输出参数为空"; return StatusCode::kInvalidArgument; }
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) { if (error_message) *error_message = "后端服务尚未初始化"; return StatusCode::kInvalidState; }
    if (config_apply_in_progress_.load()) { if (error_message) *error_message = "配置正在应用中"; return StatusCode::kInvalidState; }
    *alarms = alarm_evaluator_.list_active_alarms();
    return StatusCode::kOk;
}

// 确认活动告警。
StatusCode BackendService::acknowledge_active_alarm(
    const DeviceId& device_id,
    const std::string& point_key,
    const std::string& acknowledged_by,
    const std::optional<TimestampMs>& active_since_ms,
    ActiveAlarmView* alarm,
    std::string* message,
    std::string* error_message)
{
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) { if (error_message) *error_message = "后端服务尚未初始化"; return StatusCode::kInvalidState; }
    if (config_apply_in_progress_.load()) { if (error_message) *error_message = "配置正在应用中"; return StatusCode::kInvalidState; }
    return alarm_evaluator_.acknowledge_active_alarm(
        device_id, point_key, acknowledged_by, active_since_ms, alarm, message, error_message);
}

}  // namespace edge_controller
