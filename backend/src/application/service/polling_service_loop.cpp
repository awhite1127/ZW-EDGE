// 轮询调度循环：按稳态时钟安排周期，系统时间跳变不会触发串口或主站链路重建。
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

// 统一通道调度；RTU 与 TCP 的差异集中在采集事务内。
void PollingService::channel_worker_loop(
    ChannelId channel_id,
    std::vector<MasterPollingTarget> targets)
{
    MasterCollector collector(&channel_manager_, communication_trace_store_);
    Logger::info(
        "通道轮询线程已启动: channel=" + channel_id +
        " master_count=" + std::to_string(targets.size()));

    while (!stop_requested_.load()) {
        const auto cycle_started_at_ms = time_utils::system_now_ms();
        std::size_t processed_master_count = 0;
        std::size_t success_master_count = 0;
        std::size_t failed_master_count = 0;
        std::size_t success_device_count = 0;
        std::size_t failed_device_count = 0;
        bool has_error = false;
        std::string first_error_message;

        try {
            // 每轮只采集到期主控，避免某个短周期通道被长周期主控拖慢。
            for (auto& target : targets) {
                if (stop_requested_.load()) {
                    break;
                }
                if (target.next_poll_steady_ms != 0 &&
                    time_utils::steady_now_ms() < target.next_poll_steady_ms) {
                    continue;
                }
                target.channel_generation = channel_manager_.generation(channel_id);
                const auto started = time_utils::steady_now_ms();
                MasterCollectionResult result;
                try {
                    refresh_master_runtime_if_needed(&target);
                    result = execute_master_collection(channel_id, collector, target);
                } catch (const std::exception& error) {
                    result = mark_master_collection_failed_without_io(target,
                        "主站采集异常：" + std::string(error.what()), DiagnosisErrorCode::kUnknownError);
                } catch (...) {
                    result = mark_master_collection_failed_without_io(target,
                        "主站采集发生未知异常", DiagnosisErrorCode::kUnknownError);
                }
                target.next_poll_steady_ms = started + target.master.poll_interval_ms;
                if (result.discarded) continue;
                ++processed_master_count;
                account_master_collection_result(
                    target.master,
                    result,
                    &success_master_count,
                    &failed_master_count,
                    &success_device_count,
                    &failed_device_count,
                    &first_error_message);
            }
        } catch (const std::exception& error) {
            first_error_message = "通道 " + channel_id + " 轮询线程异常: " + std::string(error.what());
            set_last_error(channel_id, first_error_message, time_utils::system_now_ms());
            Logger::error(first_error_message);
        } catch (...) {
            first_error_message = "通道 " + channel_id + " 轮询线程发生未知 C++ 异常";
            set_last_error(channel_id, first_error_message, time_utils::system_now_ms());
            Logger::error(first_error_message);
        }

        const auto cycle_finished_at_ms = time_utils::system_now_ms();
        has_error = !first_error_message.empty() || failed_master_count > 0 || failed_device_count > 0;
        {
            std::lock_guard<std::mutex> lock(summary_mutex_);
            auto& snapshot = channel_cycle_snapshots_[channel_id];
            snapshot.started_at_ms = cycle_started_at_ms;
            snapshot.finished_at_ms = cycle_finished_at_ms;
            if (processed_master_count > 0) {
                snapshot.master_count = processed_master_count;
                snapshot.success_master_count = success_master_count;
                snapshot.failed_master_count = failed_master_count;
                snapshot.success_device_count = success_device_count;
                snapshot.failed_device_count = failed_device_count;
                snapshot.has_error = has_error;
                snapshot.error_message = first_error_message;
            } else if (has_error) {
                snapshot.has_error = true;
                snapshot.error_message = first_error_message;
            }
        }

        publish_aggregate_status(true, has_error ? "fault" : "running", has_error ? "采集异常" : "轮询运行中");

        // 按最近到期主站唤醒；耗时超过周期时直接调度，不补跑错过的轮次。
        const auto wait_started_at_ms = time_utils::steady_now_ms();
        TimestampMs wait_ms = poll_interval_ms_;
        for (const auto& target : targets) {
            const auto remaining = target.next_poll_steady_ms > wait_started_at_ms
                ? target.next_poll_steady_ms - wait_started_at_ms : 0;
            wait_ms = std::min(wait_ms, remaining);
        }

        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait_for(
            lock,
            std::chrono::milliseconds(wait_ms),
            [this]() { return stop_requested_.load(); });
    }

    Logger::info("通道轮询线程已退出: " + channel_id);
}

}  // namespace edge_controller
