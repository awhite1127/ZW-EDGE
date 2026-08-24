// 根据规则、当前值和连续次数维护告警状态并生成事件。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "service/alarm_evaluator.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>

#include "common/logger.h"
#include "common/time_utils.h"

namespace edge_controller {
namespace {

constexpr auto kActiveCheckpointInterval = std::chrono::seconds(60);

// 生成值显示文本。
std::string value_text(double value, std::uint32_t precision)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(static_cast<int>(std::min<std::uint32_t>(precision, 12))) << value;
    return stream.str();
}

// 返回告警方向的中文名称。
std::string direction_name(const std::string& direction)
{
    return direction == "low" ? "低限告警" : "高限告警";
}

// 选择事件使用的有效时间戳。
TimestampMs effective_timestamp(const PointValue& point, TimestampMs fallback)
{
    if (point.sample_time_ms != 0) return point.sample_time_ms;
    if (fallback != 0) return fallback;
    return time_utils::system_now_ms();
}

ServiceEvent make_trigger_event(
    const AlarmRule& rule,
    const AlarmRuntimeState& state,
    const AlarmPointContext& context)
{
    ServiceEvent event;
    event.timestamp_ms = state.last_evaluated_at_ms;
    event.level = rule.level;
    event.source = "data_alarm";
    event.target_id = rule.device_id;
    event.diagnosis.target_id = rule.point_key;
    event.diagnosis.target_name = context.point_name;
    event.summary = context.device_name + " " + context.point_name + direction_name(state.direction);
    event.detail = "当前值 " + value_text(state.current_value, context.precision) + context.unit +
                   "，阈值 " + value_text(state.threshold_value, context.precision) + context.unit +
                   "，方向=" + (state.direction == "high" ? "高限" : "低限");
    return event;
}

ServiceEvent make_acknowledgement_event(
    const AlarmRuntimeState& state,
    const AlarmPointContext& context)
{
    ServiceEvent event;
    event.timestamp_ms = state.acknowledged_at_ms;
    event.level = "info";
    event.source = "alarm_ack";
    event.target_id = state.device_id;
    event.diagnosis.target_id = state.point_key;
    event.diagnosis.target_name = context.point_name;
    event.summary = context.device_name + " " + context.point_name + "告警已确认";
    event.detail = "确认用户 " + state.acknowledged_by +
                   "，当前值 " + value_text(state.current_value, context.precision) + context.unit +
                   "，阈值 " + value_text(state.threshold_value, context.precision) + context.unit;
    return event;
}

ServiceEvent make_recovery_event(
    const AlarmRule& rule,
    const AlarmRuntimeState& state,
    const AlarmPointContext& context,
    const std::string& reason,
    TimestampMs timestamp_ms)
{
    const auto boundary = state.direction == "high"
                              ? rule.high_threshold - rule.hysteresis
                              : rule.low_threshold + rule.hysteresis;
    const auto duration_ms = timestamp_ms >= state.active_since_ms ? timestamp_ms - state.active_since_ms : 0;
    ServiceEvent event;
    event.timestamp_ms = timestamp_ms;
    event.level = "info";
    event.source = "data_alarm";
    event.target_id = rule.device_id;
    event.diagnosis.target_id = rule.point_key;
    event.diagnosis.target_name = context.point_name;
    event.summary = context.device_name + " " + context.point_name + direction_name(state.direction) + "已恢复";
    event.detail = "恢复值 " + value_text(state.current_value, context.precision) + context.unit +
                   "，恢复边界 " + value_text(boundary, context.precision) + context.unit +
                   "，原阈值 " + value_text(state.threshold_value, context.precision) + context.unit +
                   "，持续 " + std::to_string(duration_ms) + "ms";
    if (!reason.empty()) event.detail += "，解除原因=" + reason;
    return event;
}

}  // namespace

// 初始化报警存储并加载规则和活动报警。
StatusCode AlarmEvaluator::initialize(
    AlarmStore* store,
    const std::vector<AlarmPointContext>& contexts,
    EventCallback event_callback,
    std::string* error_message)
{
    if (store == nullptr) {
        if (error_message != nullptr) *error_message = "告警判定器缺少 AlarmStore";
        return StatusCode::kInvalidArgument;
    }

    std::unique_lock<std::mutex> mutation_lock(mutation_mutex_);
    std::vector<AlarmRule> rules;
    auto status = store->list_rules(&rules, error_message);
    if (!is_ok(status)) return status;
    std::vector<AlarmRuntimeState> states;
    status = store->list_runtime_states(&states, error_message);
    if (!is_ok(status)) return status;

    // 初始化与运行期重载也属于一次 SQLite/内存代际切换，必须和采集批次串行。
    std::unique_lock<std::mutex> lock(mutex_);
    store_ = store;
    event_callback_ = std::move(event_callback);
    rules_.clear();
    states_.clear();
    contexts_.clear();
    last_checkpoint_times_.clear();
    for (const auto& context : contexts) contexts_[{context.device_id, context.point_key}] = context;
    for (const auto& rule : rules) rules_[{rule.device_id, rule.point_key}] = rule;
    for (const auto& state : states) states_[{state.device_id, state.point_key}] = state;
    initialized_ = true;
    status = synchronize_topology_locked(contexts, "拓扑失效", error_message);
    const bool should_dispatch = begin_event_dispatch_locked();
    lock.unlock();
    mutation_lock.unlock();
    if (should_dispatch) dispatch_pending_events();
    return status;
}

// 判断报警评估器是否已初始化。
bool AlarmEvaluator::initialized() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

// 根据当前设备拓扑同步报警规则的可用性。
StatusCode AlarmEvaluator::synchronize_topology(
    const std::vector<AlarmPointContext>& contexts,
    const std::string& removal_reason,
    std::string* error_message)
{
    std::unique_lock<std::mutex> mutation_lock(mutation_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto status = synchronize_topology_locked(contexts, removal_reason, error_message);
    const bool should_dispatch = begin_event_dispatch_locked();
    lock.unlock();
    mutation_lock.unlock();
    if (should_dispatch) dispatch_pending_events();
    return status;
}

// 调用方已持有变更串行锁和状态锁时同步拓扑。
StatusCode AlarmEvaluator::synchronize_topology_locked(
    const std::vector<AlarmPointContext>& contexts,
    const std::string& removal_reason,
    std::string* error_message)
{
    if (!initialized_ || store_ == nullptr) {
        if (error_message != nullptr) *error_message = "告警判定服务尚未初始化";
        return StatusCode::kInvalidState;
    }

    std::map<Key, AlarmPointContext> next_contexts;
    for (const auto& context : contexts) next_contexts[{context.device_id, context.point_key}] = context;

    // 规则绑定的是设备数据项。拓扑变化后，如果目标点位已不存在，必须清理规则和活动状态。
    std::vector<Key> invalid_keys;
    for (const auto& item : rules_) {
        if (next_contexts.find(item.first) == next_contexts.end()) invalid_keys.push_back(item.first);
    }
    for (const auto& key : invalid_keys) {
        const auto clear_status = clear_state_locked(
            key, removal_reason, time_utils::system_now_ms(), error_message);
        if (!is_ok(clear_status)) return clear_status;
        const auto delete_status = store_->delete_rule(key.first, key.second, error_message);
        if (!is_ok(delete_status)) return delete_status;
        rules_.erase(key);
        last_checkpoint_times_.erase(key);
    }

    std::vector<Key> stale_state_keys;
    for (const auto& item : states_) {
        const auto rule = rules_.find(item.first);
        if (next_contexts.find(item.first) == next_contexts.end() || rule == rules_.end() || !rule->second.enabled) {
            stale_state_keys.push_back(item.first);
        }
    }
    for (const auto& key : stale_state_keys) {
        const auto clear_status = clear_state_locked(
            key, removal_reason, time_utils::system_now_ms(), error_message);
        if (!is_ok(clear_status)) return clear_status;
    }
    contexts_ = std::move(next_contexts);
    return StatusCode::kOk;
}

// 列出规则。
std::vector<AlarmRule> AlarmEvaluator::list_rules(const std::optional<DeviceId>& device_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AlarmRule> result;
    for (const auto& item : rules_) {
        if (!device_id.has_value() || item.first.first == *device_id) result.push_back(item.second);
    }
    return result;
}

// 返回当前活动报警列表。
std::vector<ActiveAlarmView> AlarmEvaluator::list_active_alarms() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActiveAlarmView> result;
    for (const auto& item : states_) {
        if (item.second.state != "active") continue;
        const auto rule = rules_.find(item.first);
        const auto context = contexts_.find(item.first);
        if (rule == rules_.end() || context == contexts_.end() || !rule->second.enabled) continue;
        ActiveAlarmView view;
        view.device_id = context->second.device_id;
        view.device_name = context->second.device_name;
        view.master_id = context->second.master_id;
        view.template_id = context->second.template_id;
        view.point_key = context->second.point_key;
        view.point_name = context->second.point_name;
        view.unit = context->second.unit;
        view.precision = context->second.precision;
        view.direction = item.second.direction;
        view.level = rule->second.level;
        view.current_value = item.second.current_value;
        view.threshold_value = item.second.threshold_value;
        view.active_since_ms = item.second.active_since_ms;
        view.last_evaluated_at_ms = item.second.last_evaluated_at_ms;
        view.acknowledged = item.second.acknowledged;
        view.acknowledged_at_ms = item.second.acknowledged_at_ms;
        view.acknowledged_by = item.second.acknowledged_by;
        result.push_back(std::move(view));
    }
    std::sort(result.begin(), result.end(), [](const ActiveAlarmView& left, const ActiveAlarmView& right) {
        if (left.active_since_ms != right.active_since_ms) return left.active_since_ms > right.active_since_ms;
        if (left.device_id != right.device_id) return left.device_id < right.device_id;
        return left.point_key < right.point_key;
    });
    return result;
}

// 确认活动告警。
StatusCode AlarmEvaluator::acknowledge_active_alarm(
    const DeviceId& device_id,
    const std::string& point_key,
    const std::string& acknowledged_by,
    const std::optional<TimestampMs>& active_since_ms,
    ActiveAlarmView* alarm,
    std::string* message,
    std::string* error_message)
{
    if (device_id.empty() || point_key.empty() || acknowledged_by.empty() || alarm == nullptr) {
        if (error_message != nullptr) *error_message = "设备、数据项、确认用户和告警输出不能为空";
        return StatusCode::kInvalidArgument;
    }
    std::unique_lock<std::mutex> mutation_lock(mutation_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto finish = [&](StatusCode status) {
        const bool should_dispatch = begin_event_dispatch_locked();
        lock.unlock();
        mutation_lock.unlock();
        if (should_dispatch) dispatch_pending_events();
        return status;
    };
    if (!initialized_ || store_ == nullptr) {
        if (error_message != nullptr) *error_message = "告警判定服务尚未初始化";
        return finish(StatusCode::kInvalidState);
    }
    const Key key{device_id, point_key};
    const auto state = states_.find(key);
    const auto rule = rules_.find(key);
    const auto context = contexts_.find(key);
    if (state == states_.end() || state->second.state != "active" || rule == rules_.end() ||
        context == contexts_.end() || !rule->second.enabled) {
        if (error_message != nullptr) *error_message = "活动告警不存在或已恢复";
        return finish(StatusCode::kNotFound);
    }
    if (active_since_ms.has_value() && *active_since_ms != state->second.active_since_ms) {
        if (error_message != nullptr) *error_message = "当前告警状态已变化，请刷新后重试";
        return finish(StatusCode::kInvalidState);
    }

    if (!state->second.acknowledged) {
        auto updated = state->second;
        updated.acknowledged = true;
        updated.acknowledged_at_ms = time_utils::system_now_ms();
        updated.acknowledged_by = acknowledged_by;
        updated.updated_at_ms = std::max(updated.updated_at_ms, updated.acknowledged_at_ms);
        const auto status = persist_state_locked(updated, error_message);
        if (!is_ok(status)) return finish(status);
        queue_acknowledgement_event_locked(updated, context->second);
    }

    const auto& current = states_.at(key);
    alarm->device_id = context->second.device_id;
    alarm->device_name = context->second.device_name;
    alarm->master_id = context->second.master_id;
    alarm->template_id = context->second.template_id;
    alarm->point_key = context->second.point_key;
    alarm->point_name = context->second.point_name;
    alarm->unit = context->second.unit;
    alarm->precision = context->second.precision;
    alarm->direction = current.direction;
    alarm->level = rule->second.level;
    alarm->current_value = current.current_value;
    alarm->threshold_value = current.threshold_value;
    alarm->active_since_ms = current.active_since_ms;
    alarm->last_evaluated_at_ms = current.last_evaluated_at_ms;
    alarm->acknowledged = current.acknowledged;
    alarm->acknowledged_at_ms = current.acknowledged_at_ms;
    alarm->acknowledged_by = current.acknowledged_by;
    if (message != nullptr) *message = "告警已确认";
    return finish(StatusCode::kOk);
}

// 新增或更新规则。
StatusCode AlarmEvaluator::upsert_rule(const AlarmRule& rule, const AlarmPointContext& context, std::string* error_message)
{
    std::unique_lock<std::mutex> mutation_lock(mutation_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto finish = [&](StatusCode result) {
        const bool should_dispatch = begin_event_dispatch_locked();
        lock.unlock();
        mutation_lock.unlock();
        if (should_dispatch) dispatch_pending_events();
        return result;
    };
    if (!initialized_ || store_ == nullptr) {
        if (error_message != nullptr) *error_message = "告警判定服务尚未初始化";
        return finish(StatusCode::kInvalidState);
    }
    if (context.device_id != rule.device_id || context.point_key != rule.point_key) {
        if (error_message != nullptr) *error_message = "告警规则与数据项上下文不一致";
        return finish(StatusCode::kInvalidArgument);
    }
    const Key key{rule.device_id, rule.point_key};
    auto status = store_->upsert_rule(rule, error_message);
    if (!is_ok(status)) return finish(status);
    rules_[key] = rule;
    contexts_[key] = context;

    const auto state = states_.find(key);
    // 正在活动的告警在规则变更后不能盲目保留：方向被关闭时恢复；阈值变化时刷新展示阈值。
    const bool state_direction_disabled = state != states_.end() &&
        ((state->second.direction == "high" && !rule.high_enabled) || (state->second.direction == "low" && !rule.low_enabled));
    if (!rule.enabled || state_direction_disabled) {
        status = clear_state_locked(
            key,
            !rule.enabled ? "规则停用" : "规则变更",
            time_utils::system_now_ms(),
            error_message);
    } else if (state != states_.end() && state->second.state == "active") {
        auto updated = state->second;
        updated.threshold_value = updated.direction == "high" ? rule.high_threshold : rule.low_threshold;
        updated.updated_at_ms = std::max(time_utils::system_now_ms(), updated.last_evaluated_at_ms);
        status = persist_state_locked(updated, error_message);
    }
    return finish(status);
}

// 删除规则。
StatusCode AlarmEvaluator::delete_rule(
    const DeviceId& device_id,
    const std::string& point_key,
    const std::string& reason,
    std::string* error_message)
{
    std::unique_lock<std::mutex> mutation_lock(mutation_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto finish = [&](StatusCode status) {
        const bool should_dispatch = begin_event_dispatch_locked();
        lock.unlock();
        mutation_lock.unlock();
        if (should_dispatch) dispatch_pending_events();
        return status;
    };
    if (!initialized_ || store_ == nullptr) {
        if (error_message != nullptr) *error_message = "告警判定服务尚未初始化";
        return finish(StatusCode::kInvalidState);
    }
    const Key key{device_id, point_key};
    if (rules_.find(key) == rules_.end()) {
        if (error_message != nullptr) *error_message = "告警规则不存在";
        return finish(StatusCode::kNotFound);
    }
    auto status = clear_state_locked(key, reason, time_utils::system_now_ms(), error_message);
    if (!is_ok(status)) return finish(status);
    status = store_->delete_rule(device_id, point_key, error_message);
    if (!is_ok(status)) return finish(status);
    rules_.erase(key);
    contexts_.erase(key);
    last_checkpoint_times_.erase(key);
    return finish(StatusCode::kOk);
}

// 使用同一采集批次的设备状态原子评估全部相关告警规则。
StatusCode AlarmEvaluator::evaluate_batch(
    const std::vector<DeviceStatus>& statuses,
    std::string* error_message)
{
    std::unique_lock<std::mutex> mutation_lock(mutation_mutex_);
    EvaluationPlan plan;
    AlarmStore* store = nullptr;
    {
        // 本锁内只读取已提交状态并计算 overlay，不执行 SQLite I/O。
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || store_ == nullptr) {
            if (error_message != nullptr) *error_message = "告警判定服务尚未初始化";
            return StatusCode::kInvalidState;
        }
        store = store_;
        const auto checkpoint_time = std::chrono::steady_clock::now();
        // 多区块设备可能只成功一部分区块；在线设备按点位自身质量判定。
        for (const auto& status : statuses) {
            if (!status.online) continue;
            for (const auto& point : status.points) {
                if (!point.valid || point.quality != DataQuality::kGood || !std::isfinite(point.value)) continue;
                const Key key{status.device_id, point.key};
                const auto rule = rules_.find(key);
                const auto context = contexts_.find(key);
                if (rule == rules_.end() || context == contexts_.end() || !rule->second.enabled) continue;
                evaluate_point_locked(
                    rule->second,
                    context->second,
                    point,
                    effective_timestamp(point, status.updated_at_ms),
                    checkpoint_time,
                    &plan);
            }
        }
    }

    std::vector<AlarmRuntimeState> upserts;
    std::vector<AlarmRuntimeStateKey> deletes;
    std::vector<ServiceEvent> events;
    upserts.reserve(plan.persistence_updates.size());
    deletes.reserve(plan.persistence_updates.size());
    events.reserve(plan.events.size());
    for (const auto& update : plan.persistence_updates) {
        if (update.second.has_value()) {
            upserts.push_back(*update.second);
        } else {
            deletes.push_back(AlarmRuntimeStateKey{update.first.first, update.first.second});
        }
    }
    // 事件文本格式化不占用 evaluator 状态锁；数据库失败时这些对象仍只存在于局部计划中。
    for (const auto& event : plan.events) {
        if (event.kind == PlannedEvent::Kind::kTrigger) {
            events.push_back(make_trigger_event(event.rule, event.state, event.context));
        } else {
            events.push_back(make_recovery_event(
                event.rule,
                event.state,
                event.context,
                event.reason,
                event.timestamp_ms));
        }
    }
    if (!upserts.empty() || !deletes.empty()) {
        const auto status = store->apply_runtime_state_batch(upserts, deletes, error_message);
        if (!is_ok(status)) return status;
    }

    bool should_dispatch = false;
    {
        // mutation_mutex_ 保证计划期间没有其他写者；SQLite 提交后再一次性发布内存状态。
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& update : plan.state_updates) {
            if (update.second.has_value()) {
                states_[update.first] = std::move(*update.second);
            } else {
                states_.erase(update.first);
            }
        }
        for (auto& update : plan.checkpoint_updates) {
            if (update.second.has_value()) {
                last_checkpoint_times_[update.first] = *update.second;
            } else {
                last_checkpoint_times_.erase(update.first);
            }
        }
        for (auto& event : events) pending_events_.push_back(std::move(event));
        should_dispatch = begin_event_dispatch_locked();
    }
    mutation_lock.unlock();
    if (should_dispatch) dispatch_pending_events();
    return StatusCode::kOk;
}

// 保留单设备兼容入口，共用同一批处理提交语义。
void AlarmEvaluator::evaluate(const DeviceStatus& status)
{
    if (!status.online) return;
    std::string error;
    const auto result = evaluate_batch(std::vector<DeviceStatus>{status}, &error);
    if (!is_ok(result)) {
        Logger::error("告警评估批处理失败：" + error);
    }
}

// 保存当前告警评估时间。
StatusCode AlarmEvaluator::save_now(std::string* error_message)
{
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    std::vector<AlarmRuntimeState> states;
    AlarmStore* store = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || store_ == nullptr) return StatusCode::kOk;
        store = store_;
        states.reserve(states_.size());
        for (const auto& item : states_) {
            if (item.second.state != "normal") states.push_back(item.second);
        }
    }
    return store->apply_runtime_state_batch(states, {}, error_message);
}

// 在持锁状态下持久化状态。
StatusCode AlarmEvaluator::persist_state_locked(const AlarmRuntimeState& state, std::string* error_message)
{
    const auto status = store_->upsert_runtime_state(state, error_message);
    if (!is_ok(status)) return status;
    const Key key{state.device_id, state.point_key};
    states_[key] = state;
    last_checkpoint_times_[key] = std::chrono::steady_clock::now();
    return StatusCode::kOk;
}

// 从本批 overlay 或已提交状态读取指定键。
const AlarmRuntimeState* AlarmEvaluator::planned_state_locked(
    const EvaluationPlan& plan,
    const Key& key) const
{
    const auto planned = plan.state_updates.find(key);
    if (planned != plan.state_updates.end()) {
        return planned->second.has_value() ? &*planned->second : nullptr;
    }
    const auto current = states_.find(key);
    return current == states_.end() ? nullptr : &current->second;
}

// 从本批 overlay 或已提交检查点读取指定键。
const AlarmEvaluator::CheckpointTime* AlarmEvaluator::planned_checkpoint_locked(
    const EvaluationPlan& plan,
    const Key& key) const
{
    const auto planned = plan.checkpoint_updates.find(key);
    if (planned != plan.checkpoint_updates.end()) {
        return planned->second.has_value() ? &*planned->second : nullptr;
    }
    const auto current = last_checkpoint_times_.find(key);
    return current == last_checkpoint_times_.end() ? nullptr : &current->second;
}

// 写入本批状态 overlay，持久化变更只保留每个键的最终操作。
void AlarmEvaluator::plan_state_upsert_locked(
    EvaluationPlan* plan,
    const AlarmRuntimeState& state,
    bool persist,
    CheckpointTime checkpoint_time)
{
    if (plan == nullptr) return;
    const Key key{state.device_id, state.point_key};
    plan->state_updates[key] = state;
    if (persist) {
        plan->persistence_updates[key] = state;
        plan->checkpoint_updates[key] = checkpoint_time;
    }
}

// 在本批 overlay 中删除状态和检查点，并把最终持久化操作设为 delete。
void AlarmEvaluator::plan_state_delete_locked(EvaluationPlan* plan, const Key& key)
{
    if (plan == nullptr) return;
    plan->state_updates[key] = std::nullopt;
    plan->checkpoint_updates[key] = std::nullopt;
    plan->persistence_updates[key] = std::nullopt;
}

// 在持锁状态下竞争唯一事件分发者身份。
bool AlarmEvaluator::begin_event_dispatch_locked()
{
    if (pending_events_.empty() || event_dispatch_in_progress_) {
        return false;
    }
    event_dispatch_in_progress_ = true;
    return true;
}

// 唯一分发者逐条短锁取出事件，确保 callback 执行期间不持有 AlarmEvaluator mutex。
void AlarmEvaluator::dispatch_pending_events()
{
    std::exception_ptr first_callback_error;
    try {
        while (true) {
            ServiceEvent event;
            EventCallback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_events_.empty()) {
                    event_dispatch_in_progress_ = false;
                    break;
                }
                // 先完成可能抛异常的复制，再移除队头，避免分发准备失败时丢失事件。
                callback = event_callback_;
                event = pending_events_.front();
                pending_events_.pop_front();
            }

            if (callback) {
                try {
                    callback(std::move(event));
                } catch (...) {
                    // 继续排空 FIFO（包括 callback 重入追加的事件），最后恢复分发状态并重新抛出首个异常。
                    if (first_callback_error == nullptr) first_callback_error = std::current_exception();
                }
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        event_dispatch_in_progress_ = false;
        throw;
    }

    if (first_callback_error != nullptr) std::rethrow_exception(first_callback_error);
}

// 在持锁状态下清空状态。
StatusCode AlarmEvaluator::clear_state_locked(
    const Key& key,
    const std::string& reason,
    TimestampMs timestamp_ms,
    std::string* error_message)
{
    const auto state = states_.find(key);
    if (state == states_.end()) return StatusCode::kOk;
    const auto status = store_->delete_runtime_state(key.first, key.second, error_message);
    if (!is_ok(status)) return status;
    if (state->second.state == "active") {
        const auto rule = rules_.find(key);
        const auto context = contexts_.find(key);
        if (rule != rules_.end() && context != contexts_.end()) {
            queue_recovery_event_locked(
                rule->second, state->second, context->second, reason, timestamp_ms);
        }
    }
    states_.erase(state);
    last_checkpoint_times_.erase(key);
    return StatusCode::kOk;
}

// 在持锁状态下生成告警触发事件。
void AlarmEvaluator::queue_trigger_event_locked(
    const AlarmRule& rule,
    const AlarmRuntimeState& state,
    const AlarmPointContext& context)
{
    pending_events_.push_back(make_trigger_event(rule, state, context));
}

// 排队记录确认事件。
void AlarmEvaluator::queue_acknowledgement_event_locked(
    const AlarmRuntimeState& state,
    const AlarmPointContext& context)
{
    pending_events_.push_back(make_acknowledgement_event(state, context));
}

// 在持锁状态下生成告警恢复事件。
void AlarmEvaluator::queue_recovery_event_locked(
    const AlarmRule& rule,
    const AlarmRuntimeState& state,
    const AlarmPointContext& context,
    const std::string& reason,
    TimestampMs timestamp_ms)
{
    pending_events_.push_back(make_recovery_event(rule, state, context, reason, timestamp_ms));
}

// 在持锁状态下评估点位。
void AlarmEvaluator::evaluate_point_locked(
    const AlarmRule& rule,
    const AlarmPointContext& context,
    const PointValue& point,
    TimestampMs timestamp_ms,
    CheckpointTime checkpoint_time,
    EvaluationPlan* plan)
{
    if (plan == nullptr) return;
    const Key key{rule.device_id, rule.point_key};
    const bool high = rule.high_enabled && point.value >= rule.high_threshold;
    const bool low = rule.low_enabled && point.value <= rule.low_threshold;
    const auto* current = planned_state_locked(*plan, key);
    if (current != nullptr && timestamp_ms < current->last_evaluated_at_ms) {
        // 旧样本可能来自缓存或乱序设备时间戳，不能覆盖较新的告警状态。
        return;
    }

    if (current == nullptr) {
        // 首次越限先进入 pending，除非规则要求 1 次即触发。
        if (!high && !low) return;
        AlarmRuntimeState next;
        next.device_id = rule.device_id;
        next.point_key = rule.point_key;
        next.direction = high ? "high" : "low";
        next.current_value = point.value;
        next.threshold_value = high ? rule.high_threshold : rule.low_threshold;
        next.consecutive_trigger_count = 1;
        next.last_evaluated_at_ms = timestamp_ms;
        next.updated_at_ms = timestamp_ms;
        if (rule.trigger_count == 1) {
            next.state = "active";
            next.active_since_ms = timestamp_ms;
        } else {
            next.state = "pending";
        }
        plan_state_upsert_locked(plan, next, true, checkpoint_time);
        if (next.state == "active") {
            plan->events.push_back(PlannedEvent{
                PlannedEvent::Kind::kTrigger, rule, next, context, {}, timestamp_ms});
        }
        return;
    }

    const auto previous = *current;
    if (previous.state == "pending") {
        // pending 要求连续同方向越限；恢复正常或方向切换都会重置计数。
        const std::string direction = high ? "high" : (low ? "low" : "none");
        if (direction == "none") {
            plan_state_delete_locked(plan, key);
            return;
        }
        AlarmRuntimeState next = previous;
        next.current_value = point.value;
        next.last_evaluated_at_ms = timestamp_ms;
        next.updated_at_ms = timestamp_ms;
        if (direction != previous.direction) {
            next.direction = direction;
            next.threshold_value = direction == "high" ? rule.high_threshold : rule.low_threshold;
            next.consecutive_trigger_count = 1;
        } else {
            ++next.consecutive_trigger_count;
        }
        if (next.consecutive_trigger_count >= rule.trigger_count) {
            next.state = "active";
            next.active_since_ms = timestamp_ms;
        }
        plan_state_upsert_locked(plan, next, true, checkpoint_time);
        if (next.state == "active") {
            plan->events.push_back(PlannedEvent{
                PlannedEvent::Kind::kTrigger, rule, next, context, {}, timestamp_ms});
        }
        return;
    }

    if (previous.state != "active") return;
    AlarmRuntimeState next = previous;
    next.current_value = point.value;
    next.last_evaluated_at_ms = timestamp_ms;
    next.updated_at_ms = std::max(timestamp_ms, previous.updated_at_ms);
    const bool recovery_boundary = previous.direction == "high"
                                       ? point.value <= rule.high_threshold - rule.hysteresis
                                       : point.value >= rule.low_threshold + rule.hysteresis;
    bool must_persist = false;
    if (recovery_boundary) {
        // 恢复使用回差后的边界，避免阈值附近抖动导致活动告警频繁开合。
        next.consecutive_trigger_count = 0;
        ++next.consecutive_recovery_count;
        must_persist = true;
        if (next.consecutive_recovery_count >= rule.recovery_count) {
            plan->events.push_back(PlannedEvent{
                PlannedEvent::Kind::kRecovery, rule, next, context, {}, timestamp_ms});
            plan_state_delete_locked(plan, key);
            return;
        }
    } else if (next.consecutive_recovery_count != 0) {
        next.consecutive_recovery_count = 0;
        must_persist = true;
    }

    const auto* checkpoint = planned_checkpoint_locked(*plan, key);
    // 活动告警即使未恢复，也定期落库当前值和评估时间，重启后页面不会长时间停在旧值。
    if (!must_persist && (checkpoint == nullptr ||
        checkpoint_time - *checkpoint >= kActiveCheckpointInterval)) {
        must_persist = true;
    }
    plan_state_upsert_locked(plan, next, must_persist, checkpoint_time);
}

}  // namespace edge_controller
