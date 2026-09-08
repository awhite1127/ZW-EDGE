// 轮询状态快照：在锁内复制计数与最近错误，避免 IPC 查询观察到跨周期的混合状态。
#include "application/service/polling_service.h"
#include "application/service/alarm_evaluator.h"
#include "application/service/polling_service_internal.h"
#include <chrono>
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

// 同步指定通道的最新运行状态。
void PollingService::update_channel_status(const ChannelId& channel_id)
{
    const auto* channel_status = channel_manager_.get_channel_status(channel_id);
    if (channel_status != nullptr) {
        data_store_.update_channel_status(*channel_status);
    }
}

// 汇总通道 worker 的采集结果并发布系统轮询状态。
void PollingService::publish_aggregate_status(
    bool polling_running,
    const std::string& polling_state,
    const std::string& status_message)
{
    // 多个 worker 各自维护通道快照，这里聚合成总览页和实时页使用的一份系统状态。
    TimestampMs started_at_ms = 0;
    TimestampMs finished_at_ms = 0;
    std::size_t master_count = 0;
    std::size_t success_master_count = 0;
    std::size_t failed_master_count = 0;
    std::size_t success_device_count = 0;
    std::size_t failed_device_count = 0;
    bool has_error = false;
    std::string error_message;

    {
        std::lock_guard<std::mutex> lock(summary_mutex_);
        for (const auto& entry : channel_cycle_snapshots_) {
            const auto& snapshot = entry.second;
            if (started_at_ms == 0 || (snapshot.started_at_ms != 0 && snapshot.started_at_ms < started_at_ms)) {
                started_at_ms = snapshot.started_at_ms;
            }
            if (snapshot.finished_at_ms > finished_at_ms) {
                finished_at_ms = snapshot.finished_at_ms;
            }
            master_count += snapshot.master_count;
            success_master_count += snapshot.success_master_count;
            failed_master_count += snapshot.failed_master_count;
            success_device_count += snapshot.success_device_count;
            failed_device_count += snapshot.failed_device_count;
            if (snapshot.has_error) {
                has_error = true;
                if (error_message.empty()) {
                    error_message = snapshot.error_message;
                }
            }
        }
    }

    update_polling_status(
        polling_running,
        has_error ? "fault" : polling_state,
        started_at_ms,
        finished_at_ms,
        master_count,
        success_master_count,
        failed_master_count,
        success_device_count,
        failed_device_count,
        has_error,
        error_message,
        has_error ? "采集异常" : status_message);
}

// 更新内存中的轮询摘要和系统状态。
void PollingService::update_polling_status(
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
    const std::string& status_message)
{
    DiagnosisStatus diagnosis;
    if (has_error) {
        const auto object_diagnosis = data_store_.get_current_object_diagnosis();
        diagnosis = diagnosis_has_issue(object_diagnosis)
                        ? object_diagnosis
                        : make_diagnosis(
            DiagnosisLevel::kSystem,
            "polling",
            "轮询服务",
            DiagnosisRunStatus::kError,
            DiagnosisErrorCode::kUnknownError,
            0,
            cycle_finished_at_ms != 0 ? cycle_finished_at_ms : time_utils::system_now_ms(),
            0);
        if (!diagnosis_has_issue(object_diagnosis) && !error_message.empty()) diagnosis.message = error_message;
    } else if (polling_running) {
        diagnosis = make_normal_diagnosis(
            DiagnosisLevel::kSystem,
            "polling",
            "轮询服务",
            cycle_finished_at_ms);
    } else {
        diagnosis = make_diagnosis(
            DiagnosisLevel::kSystem,
            "polling",
            "轮询服务",
            DiagnosisRunStatus::kWarning,
            DiagnosisErrorCode::kPollingNotRunning,
            0,
            cycle_finished_at_ms != 0 ? cycle_finished_at_ms : time_utils::system_now_ms(),
            0);
    }

    PollingCycleSummary summary_snapshot;
    {
        std::lock_guard<std::mutex> lock(summary_mutex_);
        last_cycle_summary_.polling_running = polling_running;
        last_cycle_summary_.polling_state = polling_state;
        if (cycle_started_at_ms != 0) {
            last_cycle_summary_.last_cycle_started_at_ms = cycle_started_at_ms;
        }
        if (cycle_finished_at_ms != 0) {
            last_cycle_summary_.last_cycle_finished_at_ms = cycle_finished_at_ms;
        }
        last_cycle_summary_.last_cycle_master_count = master_count;
        last_cycle_summary_.last_cycle_success_master_count = success_master_count;
        last_cycle_summary_.last_cycle_failed_master_count = failed_master_count;
        last_cycle_summary_.last_cycle_success_device_count = success_device_count;
        last_cycle_summary_.last_cycle_failed_device_count = failed_device_count;
        last_cycle_summary_.last_cycle_has_error = has_error;
        last_cycle_summary_.diagnosis = std::move(diagnosis);
        last_cycle_summary_.last_cycle_error_message = has_error ? last_cycle_summary_.diagnosis.message : "";
        last_cycle_summary_.last_heartbeat_ms = time_utils::steady_now_ms();
        if (!has_error) {
            last_error_summary_ = {};
        }
        summary_snapshot = last_cycle_summary_;
    }

    // 只同步轮询标量，不再复制包含全部对象状态的 SystemStatus。
    data_store_.update_polling_summary(summary_snapshot, status_message);
}

}  // namespace edge_controller
