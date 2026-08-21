// 拥有轮询工作线程及共享状态，定义采集主循环能力边界。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "collect/master_collector.h"
#include "collect/register_mapper.h"
#include "common/status_code.h"
#include "datastore/communication_store.h"
#include "datastore/data_store.h"
#include "datastore/history_store.h"
#include "manager/channel_manager.h"
#include "manager/topology_manager.h"
#include "model/service_summary.h"
#include "model/system_config.h"
#include "model/time_change.h"

namespace edge_controller {

class AlarmEvaluator;

namespace polling_service_internal {

// 生成 RTU worker 的物理总线键。同一路径必须落入同一 worker；缺失路径的异常配置按通道隔离。
inline std::string rtu_physical_worker_key(const ChannelConfig& channel)
{
    if (!channel.device_path.empty()) {
        return "serial:" + channel.device_path;
    }
    if (!channel.port_name.empty()) {
        return "serial:" + channel.port_name;
    }
    return "channel:" + channel.channel_id;
}

// 在线程规划和创建前校验配置容量，防止绕过配置入口的异常配置放大 worker 数量。
inline StatusCode validate_polling_start_channel_limits(
    const SystemConfig& system_config,
    std::string* error_message)
{
    return validate_channel_count_limits(system_config.channels, error_message)
               ? StatusCode::kOk
               : StatusCode::kInvalidArgument;
}

// 线程创建到一半失败时，唤醒并回收已经启动的 worker，再撤销运行标志。
inline void rollback_started_workers(
    std::atomic<bool>& running,
    std::atomic<bool>& stop_requested,
    std::condition_variable& wakeup,
    std::vector<std::thread>& workers)
{
    stop_requested.store(true);
    wakeup.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
    running.store(false);
}

}  // namespace polling_service_internal

// 基础轮询服务。
// 按物理链路分组采集：同一串口总线内串行，不同串口路径和 TCP 通道之间并行。
class PollingService {
public:
    using ErrorEventCallback = std::function<void(
        const std::string& source,
        const std::string& target_id,
        const std::string& message,
        TimestampMs timestamp_ms)>;
    using DeviceStatusUpdateCallback = std::function<void(const std::vector<DeviceStatus>&)>;

    // 构造 PollingService 实例。
    PollingService(
        const SystemConfig* system_config,
        TopologyManager* topology_manager,
        ChannelManager* channel_manager,
        DataStore* data_store,
        HistoryStore* history_store,
        CommunicationTraceStore* communication_trace_store,
        AlarmEvaluator* alarm_evaluator,
        std::uint32_t poll_interval_ms,
        ErrorEventCallback error_event_callback = {});
    // 销毁 PollingService 实例并释放相关资源。
    ~PollingService();

    // 启动轮询线程。
    StatusCode start();
    // 请求停止并等待轮询线程退出。
    void stop();
    // 判断轮询服务是否正在运行。
    bool is_running() const;
    // 获取最近一轮轮询摘要。
    PollingCycleSummary get_last_cycle_summary() const;
    // 获取轮询线程最近一次错误摘要。
    ServiceErrorSummary get_last_error_summary() const;
    // 清空最近错误信息摘要。
    void clear_last_error_summary();
    // 时间明显跳变后丢弃尚未落库的分桶状态；不修改已经写入的历史数据。
    std::size_t on_system_time_adjusted(TimestampMs after_time_ms);
    // 判断当前是否处于历史写入保护期。
    bool history_write_protected() const;
    // 设置轻量设备状态更新回调；传入空函数可在停止/重配前解除目标。
    void set_device_status_update_callback(DeviceStatusUpdateCallback callback);

private:
    struct MasterPollingTarget {
        MasterNodeConfig master;
        std::vector<const DeviceConfig*> devices;
        MasterCollectorRuntimePlan collector_runtime;
        RegisterMapperRuntimePlan mapper_runtime;
        std::uint64_t template_generation{0};
        // 仅用于进程内调度；使用稳态时钟，避免系统时间校准改变轮询节奏。
        TimestampMs next_poll_steady_ms{0};
    };

    struct MasterCollectionResult {
        bool success{false};
        bool communication_success{false};
        bool mapping_success{false};
        TimestampMs started_at_ms{0};
        TimestampMs finished_at_ms{0};
        MasterCollectResult collect_result;
        RegisterMapperResult map_result;
        MasterNodeStatus master_status;
        std::vector<DeviceStatus> device_statuses;
        std::string error_message;
    };

    // 从当前配置生成启用主控的配置世代运行时目标。
    std::vector<MasterPollingTarget> get_enabled_master_targets() const;
    // 模板注册表更新后，在对应 worker 内刷新主站读取与映射计划。
    void refresh_master_runtime_if_needed(MasterPollingTarget* target) const;
    // 轮询内部对单个主站执行一次采集步骤。
    MasterCollectionResult execute_master_collection(
        const ChannelId& worker_channel_id,
        MasterCollector& collector,
        MasterPollingTarget& target);
    // 在不执行 IO 的情况下构造主站采集失败结果。
    MasterCollectionResult mark_master_collection_failed_without_io(
        const MasterPollingTarget& target,
        const std::string& error_message,
        DiagnosisErrorCode error_code);
    // 统计主站采集结果。
    void account_master_collection_result(
        const MasterNodeConfig& master,
        const MasterCollectionResult& result,
        std::size_t* success_master_count,
        std::size_t* failed_master_count,
        std::size_t* success_device_count,
        std::size_t* failed_device_count,
        std::string* first_error_message);
    // 主控采集失败时，把该主控下启用设备统一标记为失败态。
    std::vector<DeviceStatus> mark_devices_collect_failed(
        const MasterPollingTarget& target,
        TimestampMs failure_time_ms,
        const std::string& error_message);
    // 构造设备失败态，不清空历史业务值，只把本次状态改为失败。
    std::vector<DeviceStatus> build_failed_device_statuses(
        const std::vector<const DeviceConfig*>& devices,
        TimestampMs failure_time_ms,
        const std::string& error_message) const;
    // 以缓存中的旧状态为基底，合并本次要写入的设备状态。
    std::vector<DeviceStatus> prepare_device_statuses_for_store(
        const std::vector<DeviceStatus>& statuses) const;
    // 写入历史数据记录。
    void write_history_records(
        const MasterNodeConfig& master_config,
        const std::vector<DeviceStatus>& statuses);
    // 批量收集历史记录并按节流策略决定是否写入，整批只获取一次节流锁。
    void collect_history_records_for_write(
        const std::vector<HistoryRecord>& records,
        TimestampMs now_ms,
        std::uint64_t time_generation,
        std::vector<HistoryRecord>* records_to_write);
    // 将写入失败的历史记录恢复到待处理队列。
    void restore_pending_history_records(
        const std::vector<HistoryRecord>& records,
        std::uint64_t time_generation);
    // 批量写入当前待处理历史记录。
    void flush_pending_history_records();
    // 把当前通道运行态同步回 DataStore。
    void update_channel_status(const ChannelId& channel_id);
    // 通道工作线程：同一通道内串行采集，不同通道 worker 并行。
    void channel_worker_loop(ChannelId channel_id, std::vector<MasterPollingTarget> targets);
    // RTU 物理总线工作线程：同一路径内的通道串行，不同串口路径由不同 worker 并行。
    void rtu_worker_loop(
        std::string physical_worker_key,
        std::map<ChannelId, std::vector<MasterPollingTarget>> targets_by_channel);
    // 汇总各通道最近周期结果并发布到 DataStore。
    void publish_aggregate_status(bool polling_running, const std::string& polling_state, const std::string& status_message);
    // 同步轮询状态到 DataStore 和本地摘要缓存。
    void update_polling_status(
        bool polling_running,
        const std::string& polling_state,
        TimestampMs cycle_started_at_ms,
        TimestampMs cycle_finished_at_ms,
        std::size_t master_count,
        std::size_t success_master_count,
        std::size_t failed_master_count,
        std::size_t success_device_count,
        std::size_t failed_device_count,
        bool has_error,
        const std::string& error_message,
        const std::string& status_message);
    // 记录轮询线程最近一次错误。
    void set_last_error(
        const std::string& target_id,
        const std::string& message,
        TimestampMs timestamp_ms);
    // 通知订阅方设备状态已经更新。
    void notify_device_status_updated(const std::vector<DeviceStatus>& statuses) noexcept;

    struct ChannelCycleSnapshot {
        TimestampMs started_at_ms{0};
        TimestampMs finished_at_ms{0};
        std::size_t master_count{0};
        std::size_t success_master_count{0};
        std::size_t failed_master_count{0};
        std::size_t success_device_count{0};
        std::size_t failed_device_count{0};
        bool has_error{false};
        std::string error_message;
    };

    // 执行 operator< 运算符对应的对象操作。
    struct HistoryWriteThrottleKey {
        DeviceId device_id;
        std::string point_key;
        std::string sample_period;
        TimestampMs bucket_start_ms{0};

        bool operator<(const HistoryWriteThrottleKey& other) const;
    };

    const SystemConfig* system_config_{nullptr};
    TopologyManager* topology_manager_{nullptr};
    ChannelManager* channel_manager_{nullptr};
    DataStore* data_store_{nullptr};
    HistoryStore* history_store_{nullptr};
    CommunicationTraceStore* communication_trace_store_{nullptr};
    AlarmEvaluator* alarm_evaluator_{nullptr};
    // 当前版本只支持全局统一轮询周期。
    std::uint32_t poll_interval_ms_{1000};
    ErrorEventCallback error_event_callback_{};
    mutable std::mutex device_status_callback_mutex_;
    DeviceStatusUpdateCallback device_status_update_callback_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    // wait_mutex_ 仅与条件变量配合中断周期等待，不保护采集业务状态。
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::vector<std::thread> channel_workers_;

    // 多个通道 worker 并行提交周期结果，摘要锁保证聚合计数不会混合不同写入时刻。
    mutable std::mutex summary_mutex_;
    // 轮询摘要和最近错误供上层直接查询。
    PollingCycleSummary last_cycle_summary_{};
    ServiceErrorSummary last_error_summary_{};
    std::map<ChannelId, ChannelCycleSnapshot> channel_cycle_snapshots_;

    // 历史节流表会被采集线程、停止刷新和时间跳变处理共同访问，必须独立于摘要锁串行化。
    mutable std::mutex history_write_mutex_;
    std::map<HistoryWriteThrottleKey, TimestampMs> history_write_times_;
    std::map<HistoryWriteThrottleKey, HistoryRecord> pending_history_records_;
    std::atomic<bool> history_time_invalid_{false};
    std::atomic<std::uint64_t> history_time_generation_{0};
};

}  // namespace edge_controller
