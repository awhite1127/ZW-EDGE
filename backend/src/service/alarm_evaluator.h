// 告警评估器按规则维护触发/恢复计数，并生成活动告警与事件。
#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/status_code.h"
#include "datastore/alarm_store.h"
#include "model/alarm.h"
#include "model/device_status.h"
#include "model/service_summary.h"

namespace edge_controller {

struct AlarmPointContext {
    DeviceId device_id;
    std::string device_name;
    MasterNodeId master_id;
    std::string template_id;
    std::string point_key;
    std::string point_name;
    std::string unit;
    std::uint32_t precision{0};
};

class AlarmEvaluator {
public:
    using EventCallback = std::function<void(ServiceEvent)>;

    // 初始化。
    StatusCode initialize(
        AlarmStore* store,
        const std::vector<AlarmPointContext>& contexts,
        EventCallback event_callback,
        std::string* error_message = nullptr);
    // 判断当前组件是否已初始化。
    bool initialized() const;
    // 同步拓扑。
    StatusCode synchronize_topology(
        const std::vector<AlarmPointContext>& contexts,
        const std::string& removal_reason,
        std::string* error_message = nullptr);

    // 列出规则。
    std::vector<AlarmRule> list_rules(const std::optional<DeviceId>& device_id = std::nullopt) const;
    // 列出活动告警。
    std::vector<ActiveAlarmView> list_active_alarms() const;
    // 确认当前活动告警；active_since_ms 用于拒绝确认已经切换过轮次的告警。
    StatusCode acknowledge_active_alarm(
        const DeviceId& device_id,
        const std::string& point_key,
        const std::string& acknowledged_by,
        const std::optional<TimestampMs>& active_since_ms,
        ActiveAlarmView* alarm,
        std::string* message,
        std::string* error_message = nullptr);
    // 新增或更新规则。
    StatusCode upsert_rule(const AlarmRule& rule, const AlarmPointContext& context, std::string* error_message = nullptr);
    // 删除规则。
    StatusCode delete_rule(const DeviceId& device_id, const std::string& point_key, const std::string& reason, std::string* error_message = nullptr);
    // 评估。
    void evaluate(const DeviceStatus& status);
    // 保存当前告警评估时间。
    StatusCode save_now(std::string* error_message = nullptr);

private:
    using Key = std::pair<DeviceId, std::string>;

    // 在持锁状态下持久化状态。
    StatusCode persist_state_locked(const AlarmRuntimeState& state, std::string* error_message);
    // 在持锁状态下清空状态。
    StatusCode clear_state_locked(
        const Key& key,
        const std::string& reason,
        TimestampMs timestamp_ms,
        std::string* error_message);
    // 在持锁状态下生成告警触发事件，外部回调由公共入口在解锁后执行。
    void queue_trigger_event_locked(
        const AlarmRule& rule,
        const AlarmRuntimeState& state,
        const AlarmPointContext& context);
    // 在持锁状态下生成告警确认事件，外部回调由公共入口在解锁后执行。
    void queue_acknowledgement_event_locked(
        const AlarmRuntimeState& state,
        const AlarmPointContext& context);
    // 在持锁状态下生成告警恢复事件，外部回调由公共入口在解锁后执行。
    void queue_recovery_event_locked(
        const AlarmRule& rule,
        const AlarmRuntimeState& state,
        const AlarmPointContext& context,
        const std::string& reason,
        TimestampMs timestamp_ms);
    // 在持锁状态下评估点位。
    void evaluate_point_locked(
        const AlarmRule& rule,
        const AlarmPointContext& context,
        const PointValue& point,
        TimestampMs timestamp_ms);
    // 在持锁状态下竞争唯一事件分发者身份。
    bool begin_event_dispatch_locked();
    // 作为唯一分发者按 FIFO 逐条取出事件，并在锁外调用当前 callback。
    void dispatch_pending_events();

    // 规则、运行态和点位上下文必须按同一代更新，事件回调由实现控制在安全边界触发。
    mutable std::mutex mutex_;
    AlarmStore* store_{nullptr};
    EventCallback event_callback_{};
    std::map<Key, AlarmRule> rules_;
    std::map<Key, AlarmRuntimeState> states_;
    std::map<Key, AlarmPointContext> contexts_;
    std::map<Key, std::chrono::steady_clock::time_point> last_checkpoint_times_;
    std::deque<ServiceEvent> pending_events_;
    bool event_dispatch_in_progress_{false};
    bool initialized_{false};
};

}  // namespace edge_controller
