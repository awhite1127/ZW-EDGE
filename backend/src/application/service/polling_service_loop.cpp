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

// TCP 或单通道 worker 循环采集该通道下的主站。
void PollingService::channel_worker_loop(
    ChannelId channel_id,
    std::vector<MasterPollingTarget> targets)
{
    MasterCollector collector(channel_manager_, communication_trace_store_);
    Logger::info(
        "通道轮询线程已启动: channel=" + channel_id +
        " master_count=" + std::to_string(targets.size()));

    while (!stop_requested_.load()) {
        const auto cycle_started_at_ms = time_utils::system_now_ms();
        const auto cycle_started_at_steady_ms = time_utils::steady_now_ms();
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
                    cycle_started_at_steady_ms < target.next_poll_steady_ms) {
                    continue;
                }
                refresh_master_runtime_if_needed(&target);

                ++processed_master_count;
                // 一个主站的全部读取区块构成单次通讯事务；控制命令会在边界处等待，不能插入帧间。
                auto communication_lease = channel_manager_ == nullptr
                    ? ChannelManager::CommunicationLease{}
                    : channel_manager_->acquire_communication_lease(channel_id);
                const auto result =
                    execute_master_collection(channel_id, collector, target);
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
        const auto cycle_finished_at_steady_ms = time_utils::steady_now_ms();
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

        // 固定全局周期：本通道耗时小于周期时补等待，大于周期时直接进入下一轮。
        const auto elapsed_ms =
            cycle_finished_at_steady_ms - cycle_started_at_steady_ms;
        const auto wait_ms = elapsed_ms >= poll_interval_ms_
                                 ? 0U
                                 : (poll_interval_ms_ - static_cast<std::uint32_t>(elapsed_ms));

        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait_for(
            lock,
            std::chrono::milliseconds(wait_ms),
            [this]() { return stop_requested_.load(); });
    }

    Logger::info("通道轮询线程已退出: " + channel_id);
}

// 单个 RTU worker 只负责一条物理串口总线，并在该总线内按通道顺序串行采集。
void PollingService::rtu_worker_loop(
    std::string physical_worker_key,
    std::map<ChannelId, std::vector<MasterPollingTarget>> targets_by_channel)
{
    std::size_t total_master_count = 0;
    for (const auto& group : targets_by_channel) {
        total_master_count += group.second.size();
    }
    Logger::info(
        "rtu_worker_start physical_bus=" + physical_worker_key +
        " channel_count=" + std::to_string(targets_by_channel.size()) +
        " master_count=" + std::to_string(total_master_count));

    std::map<ChannelId, std::unique_ptr<MasterCollector>> collectors_by_channel;
    std::map<ChannelId, const ChannelConfig*> channel_configs;
    for (const auto& group : targets_by_channel) {
        collectors_by_channel[group.first] =
            std::make_unique<MasterCollector>(channel_manager_, communication_trace_store_);
        channel_configs[group.first] =
            find_channel_config(system_config_, group.first);
    }

    std::uint64_t cycle_index = 0;
    while (!stop_requested_.load()) {
        ++cycle_index;
        const auto cycle_started_at_steady_ms = time_utils::steady_now_ms();
        std::size_t cycle_success_count = 0;
        std::size_t cycle_failure_count = 0;

        if (Logger::debug_enabled()) {
            Logger::debug(
                "rtu_cycle_start cycle=" + std::to_string(cycle_index) +
                " channel_count=" + std::to_string(targets_by_channel.size()) +
                " master_count=" + std::to_string(total_master_count));
        }

        // RTU 通道按顺序执行 IO；各串口 fd 跨周期复用，但同一时刻仍只有一个请求在执行。
        for (auto& group : targets_by_channel) {
            if (stop_requested_.load()) {
                break;
            }

            const auto& channel_id = group.first;
            const auto channel_config_iterator = channel_configs.find(channel_id);
            const auto* channel_config =
                channel_config_iterator == channel_configs.end()
                    ? nullptr
                    : channel_config_iterator->second;
            auto& collector = *collectors_by_channel[channel_id];
            const auto channel_started_at_ms = time_utils::system_now_ms();
            std::size_t processed_master_count = 0;
            std::size_t success_master_count = 0;
            std::size_t failed_master_count = 0;
            std::size_t success_device_count = 0;
            std::size_t failed_device_count = 0;
            bool has_error = false;
            std::string first_error_message;
            std::vector<MasterPollingTarget*> due_targets;
            due_targets.reserve(group.second.size());
            for (auto& target : group.second) {
                if (target.next_poll_steady_ms == 0 ||
                    cycle_started_at_steady_ms >= target.next_poll_steady_ms) {
                    refresh_master_runtime_if_needed(&target);
                    due_targets.push_back(&target);
                }
            }
            if (due_targets.empty()) {
                continue;
            }

            if (Logger::debug_enabled()) {
                Logger::debug(
                    "rtu_channel_cycle_start channel=" + channel_id +
                    " channel_name=" + (channel_config == nullptr ? std::string{} : channel_config->channel_name) +
                    " serial_device=" + channel_target_description(channel_config) +
                    " master_count=" + std::to_string(due_targets.size()));
            }

            std::string prepare_error;
            // 同一物理串口上的当前通道采集完整结束后才释放，FC10 不会中断已开始的响应收包。
            auto communication_lease = channel_manager_ == nullptr
                ? ChannelManager::CommunicationLease{}
                : channel_manager_->acquire_communication_lease(channel_id);
            const auto prepare_status = channel_manager_ == nullptr
                                            ? StatusCode::kInvalidState
                                            : channel_manager_->prepare_rtu_channel_for_collection(channel_id, &prepare_error);
            if (!is_ok(prepare_status)) {
                if (prepare_error.empty()) {
                    prepare_error = "RTU 通道准备失败: " + channel_id;
                }
                if (Logger::debug_enabled()) {
                    Logger::debug(
                        "rtu_channel_prepare_done channel_id=" + channel_id +
                        " success=false error=\"" + prepare_error + "\"");
                }
            }

            for (auto* target : due_targets) {
                const auto& master = target->master;
                if (stop_requested_.load()) {
                    break;
                }

                ++processed_master_count;
                const auto collect_started_at_steady_ms =
                    time_utils::steady_now_ms();
                if (Logger::debug_enabled()) {
                    Logger::debug(
                        "rtu_collect_start cycle=" + std::to_string(cycle_index) +
                        " channel_id=" + channel_id +
                        " channel_name=" + (channel_config == nullptr ? std::string{} : channel_config->channel_name) +
                        " master_id=" + master.master_id +
                        " master_name=" + master.master_name +
                        " serial_device=" + channel_target_description(channel_config) +
                        " slave_id=" + std::to_string(master.target_address) +
                        " base_start=" + std::to_string(master.block_start_register) +
                        " device_count=" + std::to_string(master.device_count));
                }

                MasterCollectionResult result;
                if (!is_ok(prepare_status)) {
                    result = mark_master_collection_failed_without_io(
                        *target,
                        prepare_error,
                        DiagnosisErrorCode::kChannelOpenFailed);
                } else {
                    try {
                        result = execute_master_collection(
                            channel_id, collector, *target);
                    } catch (const std::exception& error) {
                        result.error_message =
                            "RTU 主控 " + master.master_id + " 采集异常: " + std::string(error.what());
                        result.finished_at_ms = time_utils::system_now_ms();
                        result.device_statuses = mark_devices_collect_failed(
                            *target,
                            result.finished_at_ms,
                            result.error_message);
                        Logger::error(result.error_message);
                    } catch (...) {
                        result.error_message = "RTU 主控 " + master.master_id + " 采集发生未知异常";
                        result.finished_at_ms = time_utils::system_now_ms();
                        result.device_statuses = mark_devices_collect_failed(
                            *target,
                            result.finished_at_ms,
                            result.error_message);
                        Logger::error(result.error_message);
                    }
                }

                // RTU target 只要本轮已被处理，无论 prepare、受控通讯还是异常结果，
                // 都按该主站自己的周期推进，避免通道故障时退化为 worker 高频重试。
                target->next_poll_steady_ms =
                    collect_started_at_steady_ms + master.poll_interval_ms;
                account_master_collection_result(
                    master,
                    result,
                    &success_master_count,
                    &failed_master_count,
                    &success_device_count,
                    &failed_device_count,
                    &first_error_message);

                if (result.success) {
                    ++cycle_success_count;
                } else {
                    ++cycle_failure_count;
                }
                if (Logger::debug_enabled()) {
                    const auto collect_result_text =
                        result.success
                            ? "success"
                            : (result.collect_result.diagnosis_error_code ==
                                       DiagnosisErrorCode::kModbusTimeout
                                   ? "timeout"
                                   : "error");
                    const auto collect_elapsed_ms = static_cast<std::uint32_t>(
                        time_utils::steady_now_ms() -
                        collect_started_at_steady_ms);
                    Logger::debug(
                        "rtu_collect_result cycle=" + std::to_string(cycle_index) +
                        " channel_id=" + channel_id +
                        " master_id=" + master.master_id +
                        " success=" + std::string(result.success ? "true" : "false") +
                        " result=" + collect_result_text +
                        " elapsed_ms=" + std::to_string(collect_elapsed_ms) +
                        (result.error_message.empty() ? std::string{} : " error=\"" + result.error_message + "\""));
                }
            }

            const auto channel_finished_at_ms = time_utils::system_now_ms();
            has_error = !first_error_message.empty() || failed_master_count > 0 || failed_device_count > 0;
            {
                std::lock_guard<std::mutex> lock(summary_mutex_);
                auto& snapshot = channel_cycle_snapshots_[channel_id];
                snapshot.started_at_ms = channel_started_at_ms;
                snapshot.finished_at_ms = channel_finished_at_ms;
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
        }

        publish_aggregate_status(true, "running", "轮询运行中");

        // RTU 统一顺序周期：所有 RTU 通道跑完后再补等待，避免多串口并行 IO。
        const auto cycle_finished_at_steady_ms = time_utils::steady_now_ms();
        const auto elapsed_ms =
            cycle_finished_at_steady_ms - cycle_started_at_steady_ms;
        if (Logger::debug_enabled()) {
            Logger::debug(
                "rtu_cycle_end cycle=" + std::to_string(cycle_index) +
                " success=" + std::to_string(cycle_success_count) +
                " failure=" + std::to_string(cycle_failure_count) +
                " elapsed_ms=" + std::to_string(elapsed_ms));
        }
        const auto wait_ms = elapsed_ms >= poll_interval_ms_
                                 ? 0U
                                 : (poll_interval_ms_ - static_cast<std::uint32_t>(elapsed_ms));

        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait_for(
            lock,
            std::chrono::milliseconds(wait_ms),
            [this]() { return stop_requested_.load(); });
    }

    Logger::info("rtu_worker_exit physical_bus=" + physical_worker_key);
}

}  // namespace edge_controller
