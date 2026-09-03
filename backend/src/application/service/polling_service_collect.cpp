// 每轮采集执行路径：选择启用主站、调用采集器并合并状态；通道所有权仍由 ChannelManager 持有。
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
#include <unordered_map>
#include <utility>
#include <vector>
#include "shared/common/enums.h"
#include "shared/common/logger.h"
#include "shared/common/readable_error.h"
#include "shared/common/time_utils.h"
#include "data/model/device_value_status.h"

namespace edge_controller {

using namespace polling_service_internal;

// 从当前配置生成启用主站的稳定运行时目标。
std::vector<PollingService::MasterPollingTarget>
PollingService::get_enabled_master_targets() const
{
    std::vector<MasterPollingTarget> targets;
    if (system_config_ == nullptr) {
        return targets;
    }

    std::map<ChannelId, bool> channel_enabled_by_id;
    for (const auto& channel : system_config_->channels) {
        channel_enabled_by_id[channel.channel_id] = channel.enabled;
    }

    targets.reserve(system_config_->master_nodes.size());
    for (const auto& master : system_config_->master_nodes) {
        if (!master.enabled ||
            (master.protocol != MasterProtocol::kModbusRtu &&
             master.protocol != MasterProtocol::kModbusTcp)) {
            continue;
        }

        const auto channel_iterator = channel_enabled_by_id.find(master.channel_id);
        if (channel_iterator == channel_enabled_by_id.end() || !channel_iterator->second) {
            continue;
        }

        MasterPollingTarget target;
        target.master = master;
        if (topology_manager_ != nullptr) {
            const auto& devices =
                topology_manager_->get_devices_by_master(master.master_id);
            target.devices.reserve(devices.size());
            for (const auto* device : devices) {
                if (device != nullptr && device->enabled) {
                    target.devices.push_back(device);
                }
            }
        }
        if (target.devices.empty()) {
            continue;
        }

        // 先记录世代再获取模板；若注册表并发更新，下一轮会看到世代差异并重建。
        target.template_generation = device_template_registry_generation();
        target.collector_runtime =
            MasterCollector::build_runtime_plan(target.master, target.devices);
        target.mapper_runtime = RegisterMapper::build_runtime_plan(
            target.collector_runtime.device_template);
        if (data_store_ != nullptr) {
            const auto cached_status =
                data_store_->get_master_status(target.master.master_id);
            if (cached_status.has_value() &&
                cached_status->last_poll_time_ms != 0) {
                // 持久状态使用系统时间；启动时只换算一次剩余等待，并限制在一个周期内。
                // 后续调度完全使用稳态时钟，不受 NTP/人工校时前后跳影响。
                const auto system_now_ms = time_utils::system_now_ms();
                TimestampMs remaining_ms = target.master.poll_interval_ms;
                if (system_now_ms >= cached_status->last_poll_time_ms) {
                    const auto elapsed_ms =
                        system_now_ms - cached_status->last_poll_time_ms;
                    remaining_ms =
                        elapsed_ms >= target.master.poll_interval_ms
                            ? 0
                            : target.master.poll_interval_ms - elapsed_ms;
                }
                target.next_poll_steady_ms =
                    time_utils::steady_now_ms() + remaining_ms;
            }
        }
        targets.push_back(std::move(target));
    }
    return targets;
}

// 模板注册表变化后，在目标所属 worker 内事务式替换读取和映射运行计划。
void PollingService::refresh_master_runtime_if_needed(
    MasterPollingTarget* target) const
{
    if (target == nullptr) {
        return;
    }
    const auto generation = device_template_registry_generation();
    if (target->template_generation == generation) {
        return;
    }

    auto collector_runtime =
        MasterCollector::build_runtime_plan(target->master, target->devices);
    auto mapper_runtime = RegisterMapper::build_runtime_plan(
        collector_runtime.device_template);
    target->collector_runtime = std::move(collector_runtime);
    target->mapper_runtime = std::move(mapper_runtime);
    target->template_generation = generation;
}

// 对单个主站执行一次采集、解析和状态映射。
PollingService::MasterCollectionResult PollingService::execute_master_collection(
    const ChannelId& worker_channel_id,
    MasterCollector& collector,
    MasterPollingTarget& target)
{
    const auto& master_config = target.master;
    MasterCollectionResult result;
    result.started_at_ms = time_utils::system_now_ms();
    result.master_status.master_id = master_config.master_id;

    if (topology_manager_ == nullptr || channel_manager_ == nullptr || data_store_ == nullptr) {
        return mark_master_collection_failed_without_io(
            target,
            "轮询采集服务尚未初始化",
            DiagnosisErrorCode::kPollingNotRunning);
    }

    if (!master_config.enabled) {
        return mark_master_collection_failed_without_io(
            target,
            "主控未启用",
            DiagnosisErrorCode::kConfigInvalid);
    }

    if (master_config.channel_id != worker_channel_id) {
        return mark_master_collection_failed_without_io(
            target,
            "主控绑定通道与当前轮询线程不一致: master=" + master_config.master_id +
                " master_channel=" + master_config.channel_id +
                " worker_channel=" + worker_channel_id,
            DiagnosisErrorCode::kConfigInvalid);
    }

    if (master_config.protocol != MasterProtocol::kModbusRtu &&
        master_config.protocol != MasterProtocol::kModbusTcp) {
        return mark_master_collection_failed_without_io(
            target,
            "当前采集链路仅接受 Modbus RTU 或 Modbus TCP 主控",
            DiagnosisErrorCode::kConfigInvalid);
    }

    MasterNodeStatus master_status;
    const auto cached_master_status = data_store_->get_master_status(master_config.master_id);
    if (cached_master_status.has_value()) {
        master_status = *cached_master_status;
    }
    master_status.master_id = master_config.master_id;

    if (stop_requested_.load()) {
        return mark_master_collection_failed_without_io(
            target,
            "轮询停止中，已中断本次主站采集",
            DiagnosisErrorCode::kPollingNotRunning);
    }
    if (Logger::debug_enabled()) {
        Logger::debug(
            "通道 worker " + worker_channel_id +
            " 主控 " + master_config.master_id +
            " 开始按多区块模型采集，设备数量=" +
            std::to_string(target.devices.size()));
    }

    // 重试由 MasterCollector 在每个读取区块内部独立执行；这里每轮只调用一次，
    // 避免一个区块失败导致已经成功的区块被整主站重复读取。
    const auto poll_started_at_steady_ms = time_utils::steady_now_ms();
    result.collect_result = collector.collect_once(
        master_config,
        target.devices,
        target.collector_runtime,
        &master_status,
        &stop_requested_);
    if (stop_requested_.load()) {
        result.error_message = "轮询停止中，采集已取消";
        result.finished_at_ms = time_utils::system_now_ms();
        return result;
    }
    update_channel_status(master_config.channel_id);
    result.communication_success = result.collect_result.any_success;
    result.error_message = result.collect_result.error_message;

    if (result.collect_result.device_results.empty()) {
        const auto failure_time = result.collect_result.timestamp_ms != 0
                                      ? result.collect_result.timestamp_ms
                                      : time_utils::system_now_ms();
        if (result.error_message.empty()) result.error_message = "主控采集未生成设备区块结果";
        result.device_statuses = mark_devices_collect_failed(
            target, failure_time, result.error_message);
        data_store_->update_master_status(master_status);
        target.next_poll_steady_ms =
            poll_started_at_steady_ms + master_config.poll_interval_ms;
        result.master_status = master_status;
        result.finished_at_ms = time_utils::system_now_ms();
        return result;
    }

    result.map_result = RegisterMapper::map_devices(
        master_config,
        target.devices,
        target.mapper_runtime,
        result.collect_result.device_results,
        result.collect_result.timestamp_ms);
    result.mapping_success = result.map_result.success;

    result.device_statuses = prepare_device_statuses_for_store(result.map_result.device_statuses);
    const auto& statuses_to_store = result.device_statuses;

    // 数值状态只在首次出现或状态码发生变化时生成事件，避免每轮采集重复上报。
    std::vector<DeviceId> value_status_device_ids;
    value_status_device_ids.reserve(statuses_to_store.size());
    for (const auto& device_status : statuses_to_store) {
        if (is_device_value_status_error_code(device_status.diagnosis.error_code)) {
            value_status_device_ids.push_back(device_status.device_id);
        }
    }
    std::unordered_map<DeviceId, std::string> previous_value_status_codes;
    for (const auto& previous : data_store_->get_device_statuses(value_status_device_ids)) {
        previous_value_status_codes.emplace(
            previous.device_id,
            previous.diagnosis.error_code);
    }
    std::vector<const DeviceStatus*> changed_value_statuses;
    changed_value_statuses.reserve(value_status_device_ids.size());
    for (const auto& device_status : statuses_to_store) {
        const auto& code = device_status.diagnosis.error_code;
        if (!is_device_value_status_error_code(code)) {
            continue;
        }
        const auto previous = previous_value_status_codes.find(device_status.device_id);
        if (previous == previous_value_status_codes.end() || previous->second != code) {
            changed_value_statuses.push_back(&device_status);
        }
    }

    if (Logger::debug_enabled()) {
        for (const auto& device_status : statuses_to_store) {
            std::string message =
                "设备 " + device_status.device_id +
                " 采集点数=" + std::to_string(device_status.points.size());
            if (device_status.has_resistance) {
                message += "，接地电阻=" + std::to_string(device_status.resistance_value);
            }
            if (!device_status.last_error_message.empty()) {
                message += "，错误=" + device_status.last_error_message;
            }
            Logger::debug(message);
        }
    }
    data_store_->update_device_statuses(statuses_to_store);
    notify_device_status_updated(statuses_to_store);
    for (const auto* device_status : changed_value_statuses) {
        set_last_error(
            device_status->device_id,
            device_status->diagnosis.message,
            device_status->updated_at_ms);
    }
    write_history_records(master_config, statuses_to_store);
    if (alarm_evaluator_ != nullptr) {
        // 告警判定使用已写入运行缓存的同一批设备状态，避免页面实时值与告警判断来源不一致。
        std::string alarm_error;
        const auto alarm_status = alarm_evaluator_->evaluate_batch(statuses_to_store, &alarm_error);
        if (!is_ok(alarm_status)) {
            Logger::error("告警评估批次提交失败：" + alarm_error);
        }
    }

    if (!result.map_result.success) {
        // 映射错误单独通过诊断与本次服务结果上报；通讯质量和完整通讯成功标记
        // 必须保留 MasterCollector 按读取区块结果计算出的值。
        master_status.last_failure_time_ms = result.collect_result.timestamp_ms;
        ++master_status.consecutive_failure_count;
        master_status.consecutive_timeout_count = 0;
        if (!result.map_result.errors.empty()) {
            result.error_message = result.map_result.errors.front();
        } else {
            result.error_message = "设备数据映射失败";
        }
        master_status.diagnosis = make_diagnosis(
            DiagnosisLevel::kMaster,
            master_config.master_id,
            master_config.master_name,
            result.collect_result.any_success ? DiagnosisRunStatus::kWarning : DiagnosisRunStatus::kError,
            DiagnosisErrorCode::kDeviceParseFailed,
            master_status.last_success_time_ms,
            master_status.last_failure_time_ms,
            master_status.consecutive_failure_count);
        master_status.diagnosis.message = result.error_message;
        master_status.last_error_message = result.error_message;
    }

    data_store_->update_master_status(master_status);
    target.next_poll_steady_ms =
        poll_started_at_steady_ms + master_config.poll_interval_ms;
    result.master_status = master_status;
    result.success = result.collect_result.success && result.map_result.success;
    result.finished_at_ms = time_utils::system_now_ms();

    if (Logger::debug_enabled()) {
        Logger::debug(
            "通道 worker " + worker_channel_id +
            " 主控 " + master_config.master_id + " 数据已写入运行缓存");
    }
    return result;
}

// 合并旧状态和本轮状态，准备写入 DataStore。
std::vector<DeviceStatus> PollingService::prepare_device_statuses_for_store(
    const std::vector<DeviceStatus>& statuses) const
{
    std::vector<DeviceStatus> merged_statuses;
    merged_statuses.reserve(statuses.size());

    std::unordered_map<DeviceId, DeviceStatus> cached_by_id;
    if (data_store_ != nullptr) {
        std::vector<DeviceId> merge_ids;
        merge_ids.reserve(statuses.size());
        for (const auto& status : statuses) {
            if (!status.last_collect_success ||
                is_device_value_status_error_code(status.diagnosis.error_code)) {
                merge_ids.push_back(status.device_id);
            }
        }
        const auto cached_statuses = data_store_->get_device_statuses(merge_ids);
        cached_by_id.reserve(cached_statuses.size());
        for (const auto& cached : cached_statuses) cached_by_id.emplace(cached.device_id, cached);
    }

    for (auto status : statuses) {
        const auto cached = cached_by_id.find(status.device_id);
        if (!status.last_collect_success && cached != cached_by_id.end()) {
            status = merge_failed_status(cached->second, status);
        } else if (is_device_value_status_error_code(status.diagnosis.error_code) &&
                   cached != cached_by_id.end() &&
                   cached->second.diagnosis.error_code == status.diagnosis.error_code) {
            status.diagnosis.consecutive_failures =
                cached->second.diagnosis.consecutive_failures + 1;
        }
        merged_statuses.push_back(status);
    }

    return merged_statuses;
}

}  // namespace edge_controller
