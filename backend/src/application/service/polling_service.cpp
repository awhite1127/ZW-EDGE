// 轮询服务装配与生命周期：创建工作线程和共享状态，具体循环、采集、历史写入分文件实现。
#include "application/service/polling_service.h"
#include "application/service/alarm_evaluator.h"
#include "application/service/polling_service_internal.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <exception>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>
#include "shared/common/enums.h"
#include "shared/common/logger.h"
#include "shared/common/readable_error.h"
#include "shared/common/time_utils.h"

namespace edge_controller {

using namespace polling_service_internal;

// PollingService 负责持续轮询调度；单主站采集步骤仍委托 MasterCollector / RegisterMapper 完成。
PollingService::PollingService(
    const SystemConfig& system_config,
    ChannelManager& channel_manager,
    DataStore& data_store,
    HistoryStore* history_store,
    CommunicationTraceStore* communication_trace_store,
    AlarmEvaluator* alarm_evaluator,
    std::uint32_t poll_interval_ms,
    ErrorEventCallback error_event_callback)
    : system_config_(system_config),
      channel_manager_(channel_manager),
      data_store_(data_store),
      history_store_(history_store),
      communication_trace_store_(communication_trace_store),
      alarm_evaluator_(alarm_evaluator),
      poll_interval_ms_(poll_interval_ms == 0 ? 1000 : poll_interval_ms),
      error_event_callback_(std::move(error_event_callback))
{
}

PollingService::~PollingService()
{
    stop();
}

// 启动采集轮询线程，并按通道类型分配 worker。
StatusCode PollingService::start()
{
    if (running_.load()) {
        return StatusCode::kInvalidState;
    }
    std::string channel_limit_error;
    const auto channel_limit_status =
        polling_service_internal::validate_polling_start_channel_limits(
            system_config_, &channel_limit_error);
    if (!is_ok(channel_limit_status)) {
        const auto failed_at_ms = time_utils::system_now_ms();
        Logger::error("轮询启动失败：" + channel_limit_error);
        try {
            set_last_error("polling", channel_limit_error, failed_at_ms);
            update_polling_status(
                false,
                "error",
                0,
                failed_at_ms,
                0,
                0,
                0,
                0,
                0,
                true,
                channel_limit_error,
                "轮询未启动：通道配置超过容量上限");
        } catch (const std::exception& status_error) {
            Logger::error("记录轮询容量错误失败：" + std::string(status_error.what()));
        } catch (...) {
            Logger::error("记录轮询容量错误失败：未知异常");
        }
        return channel_limit_status;
    }

    stop_requested_.store(false);
    auto enabled_targets = get_enabled_master_targets();
    std::map<ChannelId, std::size_t> target_counts_by_channel;
    std::map<ChannelId, std::vector<MasterPollingTarget>> targets_by_channel;
    // 每个逻辑通道拥有可独立重配置的调度器，物理串口互斥由 ChannelManager 唯一管理。
    for (auto& target : enabled_targets) {
        const auto channel_id = target.master.channel_id;
        ++target_counts_by_channel[channel_id];
        targets_by_channel[channel_id].push_back(std::move(target));
    }

    if (target_counts_by_channel.empty()) {
        // 没有可采集目标时不创建 worker，但仍写入系统诊断，供 Web 页面解释“未启动”的原因。
        {
            std::lock_guard<std::mutex> lock(summary_mutex_);
            channel_cycle_snapshots_.clear();
            last_cycle_summary_.polling_running = false;
            last_cycle_summary_.polling_state = "stopped";
            last_cycle_summary_.last_cycle_master_count = 0;
            last_cycle_summary_.last_cycle_success_master_count = 0;
            last_cycle_summary_.last_cycle_failed_master_count = 0;
            last_cycle_summary_.last_cycle_success_device_count = 0;
            last_cycle_summary_.last_cycle_failed_device_count = 0;
            last_cycle_summary_.last_cycle_has_error = false;
            last_cycle_summary_.diagnosis = make_diagnosis(
                DiagnosisLevel::kSystem,
                "polling",
                "轮询服务",
                DiagnosisRunStatus::kWarning,
                DiagnosisErrorCode::kPollingNotRunning,
                0,
                time_utils::system_now_ms(),
                0);
            last_cycle_summary_.last_cycle_error_message.clear();
            last_cycle_summary_.last_heartbeat_ms = time_utils::steady_now_ms();
        }
        update_polling_status(
            false,
            "stopped",
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            false,
            "",
            "当前无有效启用采集目标，轮询未启动");
        Logger::warn("当前无有效启用采集目标，轮询未启动");
        return StatusCode::kInvalidState;
    }

    {
        std::lock_guard<std::mutex> lock(summary_mutex_);
        channel_cycle_snapshots_.clear();
        for (const auto& group : target_counts_by_channel) {
            channel_cycle_snapshots_[group.first] = {};
        }
        last_cycle_summary_.polling_running = true;
        last_cycle_summary_.polling_state = "running";
        last_cycle_summary_.last_heartbeat_ms = time_utils::steady_now_ms();
    }

    running_.store(true);
    const auto rollback_started_workers = [this](const std::string& error_message) {
        polling_service_internal::rollback_started_workers(
            running_, stop_requested_, wait_cv_, channel_workers_);
        if (freshness_worker_.joinable()) freshness_worker_.join();
        persistence_queue_.stop();
        try {
            const auto failed_at_ms = time_utils::system_now_ms();
            set_last_error("polling", error_message, failed_at_ms);
            update_polling_status(
                false,
                "error",
                0,
                failed_at_ms,
                0,
                0,
                0,
                0,
                0,
                true,
                error_message,
                "轮询线程启动失败");
        } catch (const std::exception& status_error) {
            Logger::error("回滚轮询线程后更新状态失败：" + std::string(status_error.what()));
        } catch (...) {
            Logger::error("回滚轮询线程后更新状态失败：未知异常");
        }
    };
    try {
        persistence_queue_.start();
        freshness_worker_ = std::thread(&PollingService::freshness_worker_loop, this);
        channel_workers_.reserve(targets_by_channel.size());
        for (auto& group : targets_by_channel) {
            channel_workers_.emplace_back(
                &PollingService::channel_worker_loop,
                this,
                group.first,
                std::move(group.second));
        }
    } catch (const std::exception& error) {
        const auto message = "创建通道轮询线程失败：" + std::string(error.what());
        Logger::error(message);
        rollback_started_workers(message);
        return StatusCode::kInternalError;
    } catch (...) {
        const std::string message = "创建通道轮询线程失败：未知异常";
        Logger::error(message);
        rollback_started_workers(message);
        return StatusCode::kInternalError;
    }

    update_polling_status(true, "running", 0, 0, 0, 0, 0, 0, 0, false, "", "轮询运行中");
    return StatusCode::kOk;
}

// 请求停止轮询并等待所有 worker 退出。
void PollingService::stop()
{
    const bool was_running = running_.load();
    const bool had_worker = !channel_workers_.empty();
    // 通过原子标记和条件变量唤醒，让轮询线程尽快退出等待态。
    stop_requested_.store(true);
    if (was_running || had_worker) {
        PollingCycleSummary summary_snapshot;
        {
            std::lock_guard<std::mutex> lock(summary_mutex_);
            summary_snapshot = last_cycle_summary_;
        }
        update_polling_status(
            true,
            "stopping",
            0,
            0,
            summary_snapshot.last_cycle_master_count,
            summary_snapshot.last_cycle_success_master_count,
            summary_snapshot.last_cycle_failed_master_count,
            summary_snapshot.last_cycle_success_device_count,
            summary_snapshot.last_cycle_failed_device_count,
            summary_snapshot.last_cycle_has_error,
            summary_snapshot.last_cycle_error_message,
            "轮询停止中");
    }
    wait_cv_.notify_all();

    for (auto& worker : channel_workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    channel_workers_.clear();
    if (freshness_worker_.joinable()) freshness_worker_.join();
    if (was_running || had_worker) expire_device_values(true);
    persistence_queue_.stop();
    flush_pending_history_records();

    running_.store(false);

    if (was_running || had_worker) {
        PollingCycleSummary summary_snapshot;
        {
            std::lock_guard<std::mutex> lock(summary_mutex_);
            summary_snapshot = last_cycle_summary_;
        }
        update_polling_status(
            false,
            "stopped",
            summary_snapshot.last_cycle_started_at_ms,
            summary_snapshot.last_cycle_finished_at_ms == 0 ? time_utils::system_now_ms() : summary_snapshot.last_cycle_finished_at_ms,
            summary_snapshot.last_cycle_master_count,
            summary_snapshot.last_cycle_success_master_count,
            summary_snapshot.last_cycle_failed_master_count,
            summary_snapshot.last_cycle_success_device_count,
            summary_snapshot.last_cycle_failed_device_count,
            summary_snapshot.last_cycle_has_error,
            summary_snapshot.last_cycle_error_message,
            "轮询已停止");
    }
}

void PollingService::invalidate_channels(const std::vector<ChannelConfig>& previous, const std::vector<ChannelConfig>& next)
{
    std::vector<MasterNodeId> masters;
    for (const auto& master : system_config_.master_nodes) {
        const auto old = std::find_if(previous.begin(), previous.end(), [&](const auto& c) { return c.channel_id == master.channel_id; });
        const auto current = std::find_if(next.begin(), next.end(), [&](const auto& c) { return c.channel_id == master.channel_id; });
        if (old != previous.end() && (current == next.end() || channel_transport_key(*old) != channel_transport_key(*current)))
            masters.push_back(master.master_id);
    }
    std::lock_guard<std::mutex> lock(publication_mutex_);
    notify_device_status_updated(data_store_.expire_device_values(true, &masters));
}

// 状态写入和北向通知共用顺序边界，防止超期通知覆盖随后成功的新采样。
bool PollingService::publish_device_statuses(const std::vector<DeviceStatus>& statuses,
    const ChannelId& channel, std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(publication_mutex_);
    if (!channel.empty() && channel_manager_.generation(channel) != generation) return false;
    data_store_.update_device_statuses(statuses);
    notify_device_status_updated(statuses);
    return true;
}

void PollingService::expire_device_values(bool stopped)
{
    std::lock_guard<std::mutex> lock(publication_mutex_);
    notify_device_status_updated(data_store_.expire_device_values(stopped));
}

void PollingService::freshness_worker_loop()
{
    while (!stop_requested_.load()) {
        try { expire_device_values(false); }
        catch (const std::exception& error) { Logger::error("更新采样质量失败：" + std::string(error.what())); }
        catch (...) { Logger::error("更新采样质量发生未知异常"); }
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop_requested_.load(); });
    }
}

// 判断轮询服务是否正在运行。
bool PollingService::is_running() const
{
    return running_.load();
}

// 获取最近一轮采集摘要。
PollingCycleSummary PollingService::get_last_cycle_summary() const
{
    std::lock_guard<std::mutex> lock(summary_mutex_);
    return last_cycle_summary_;
}

// 获取最近一次采集错误摘要。
ServiceErrorSummary PollingService::get_last_error_summary() const
{
    std::lock_guard<std::mutex> lock(summary_mutex_);
    return last_error_summary_;
}

// 清空最近采集错误摘要。
void PollingService::clear_last_error_summary()
{
    {
        std::lock_guard<std::mutex> lock(summary_mutex_);
        last_error_summary_ = {};
        last_cycle_summary_.last_cycle_has_error = false;
        last_cycle_summary_.diagnosis = make_normal_diagnosis(DiagnosisLevel::kSystem, "polling", "轮询服务", 0);
        last_cycle_summary_.last_cycle_error_message.clear();
    }

    auto system_status = data_store_.get_system_status();
    system_status.last_poll_cycle_has_error = false;
    system_status.diagnosis = make_normal_diagnosis(DiagnosisLevel::kSystem, "polling", "轮询服务", 0);
    system_status.last_poll_cycle_error_message.clear();
    data_store_.update_system_status(system_status);
}

// 设置设备状态更新回调。
void PollingService::set_device_status_update_callback(DeviceStatusUpdateCallback callback)
{
    std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
    device_status_update_callback_ = std::move(callback);
}

// 通知设备状态。
void PollingService::notify_device_status_updated(const std::vector<DeviceStatus>& statuses) noexcept
{
    if (statuses.empty()) return;
    DeviceStatusUpdateCallback callback;
    {
        std::lock_guard<std::mutex> lock(device_status_callback_mutex_);
        callback = device_status_update_callback_;
    }
    if (!callback) return;
    try {
        callback(statuses);
    } catch (const std::exception& error) {
        Logger::warn("设备状态更新回调异常，已与轮询隔离：" + std::string(error.what()));
    } catch (...) {
        Logger::warn("设备状态更新回调发生未知异常，已与轮询隔离");
    }
}

}  // namespace edge_controller
