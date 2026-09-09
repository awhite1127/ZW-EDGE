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

#include "shared/common/status_code.h"
#include "data/datastore/alarm_store.h"
#include "data/model/alarm.h"
#include "data/model/device_status.h"
#include "data/model/service_summary.h"

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
        std::string* error_message = nullptr,
        std::function<bool()> suppress_events = {});
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
    // 按采集顺序原子评估一批设备状态。
    StatusCode evaluate_batch(
        const std::vector<DeviceStatus>& statuses,
        std::string* error_message = nullptr,
        bool sampling_gap = false);
    // 评估。
    void evaluate(const DeviceStatus& status);
    // 保存当前告警评估时间。
    StatusCode save_now(std::string* error_message = nullptr);

private:
    using Key = std::pair<DeviceId, std::string>;
    using CheckpointTime = std::chrono::steady_clock::time_point;

    struct PlannedEvent {
        enum class Kind {
            kTrigger,
            kRecovery,
        };

        Kind kind{Kind::kTrigger};
        AlarmRule rule;
        AlarmRuntimeState state;
        AlarmPointContext context;
        std::string reason;
        TimestampMs timestamp_ms{0};
    };

    struct EvaluationPlan {
        // optional 为 nullopt 表示删除该键；有值表示插入或覆盖。
        std::map<Key, std::optional<AlarmRuntimeState>> state_updates;
        std::map<Key, std::optional<CheckpointTime>> checkpoint_updates;
        std::map<Key, std::optional<AlarmRuntimeState>> persistence_updates;
        std::vector<PlannedEvent> events;
    };

    // 在持锁状态下持久化状态。
    StatusCode persist_state_locked(const AlarmRuntimeState& state, std::string* error_message);
    // 在持锁状态下清空状态。
    StatusCode clear_state_locked(
        const Key& key,
        const std::string& reason,
        TimestampMs timestamp_ms,
        std::string* error_message);
    // 调用方已持有 mutation_mutex_ 与 mutex_ 时同步拓扑，不直接派发事件。
    StatusCode synchronize_topology_locked(
        const std::vector<AlarmPointContext>& contexts,
        const std::string& removal_reason,
        std::string* error_message);
    // 读取计划 overlay 中的最新状态，未覆盖时回退到已提交内存状态。
    const AlarmRuntimeState* planned_state_locked(const EvaluationPlan& plan, const Key& key) const;
    // 读取计划 overlay 中的最新检查点。
    const CheckpointTime* planned_checkpoint_locked(const EvaluationPlan& plan, const Key& key) const;
    // 记录一次状态写入计划。
    void plan_state_upsert_locked(
        EvaluationPlan* plan,
        const AlarmRuntimeState& state,
        bool persist,
        CheckpointTime checkpoint_time);
    // 记录一次状态删除计划。
    void plan_state_delete_locked(EvaluationPlan* plan, const Key& key);
    // 在持锁状态下只计算点位状态迁移，不执行 I/O 或发布内存状态。
    void evaluate_point_locked(
        const AlarmRule& rule,
        const AlarmPointContext& context,
        const PointValue& point,
        TimestampMs timestamp_ms,
        CheckpointTime checkpoint_time,
        EvaluationPlan* plan);
    // 在持锁状态下竞争唯一事件分发者身份。
    bool begin_event_dispatch_locked();
    // 作为唯一分发者按 FIFO 逐条取出事件，并在锁外调用当前 callback。
    void dispatch_pending_events();

    // 所有会同时改变 SQLite 与 evaluator 内存的公共操作先持有本锁；
    // 锁顺序固定为 mutation_mutex_ -> mutex_。
    mutable std::mutex mutation_mutex_;
    // 规则、运行态和点位上下文必须按同一代更新，事件回调由实现控制在安全边界触发。
    mutable std::mutex mutex_;
    AlarmStore* store_{nullptr};
    std::function<bool()> suppress_events_;
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
