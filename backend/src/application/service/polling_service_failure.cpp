// 轮询失败归一化：累计连续失败、生成诊断状态和事件，并在恢复后清理临时错误。
#include "application/service/polling_service.h"
#include "application/service/alarm_evaluator.h"
#include "application/service/polling_service_internal.h"
#include "communication/collect/block_read_plan.h"
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

namespace edge_controller {

using namespace polling_service_internal;

// 在未发起 IO 的情况下构造主站采集失败结果。
PollingService::MasterCollectionResult PollingService::mark_master_collection_failed_without_io(
    const MasterPollingTarget& target,
    const std::string& error_message,
    DiagnosisErrorCode error_code)
{
    const auto& master_config = target.master;
    MasterCollectionResult result;
    result.started_at_ms = time_utils::system_now_ms();
    result.finished_at_ms = result.started_at_ms;
    result.error_message = error_message;
    result.collect_result.error_message = error_message;
    result.collect_result.diagnosis_error_code = error_code;
    result.collect_result.timestamp_ms = result.finished_at_ms;

    MasterNodeStatus master_status;
    const auto cached_master_status = data_store_.get_master_status(master_config.master_id);
    if (cached_master_status.has_value()) {
        master_status = *cached_master_status;
    }

    master_status.master_id = master_config.master_id;
    master_status.online = false;
    master_status.last_collect_success = false;
    master_status.communication_quality = DataQuality::kBad;
    master_status.last_failure_time_ms = result.finished_at_ms;
    ++master_status.consecutive_failure_count;
    master_status.consecutive_timeout_count = 0;
    master_status.diagnosis = make_diagnosis(
        DiagnosisLevel::kMaster,
        master_config.master_id,
        master_config.master_name,
        DiagnosisRunStatus::kError,
        error_code,
        master_status.last_success_time_ms,
        result.finished_at_ms,
        master_status.consecutive_failure_count);
    master_status.diagnosis.message = error_message;
    master_status.last_error_message = error_message;
    data_store_.update_master_status(master_status);

    result.master_status = master_status;
    result.device_statuses = mark_devices_collect_failed(
        target,
        result.finished_at_ms,
        error_message, error_code);
    return result;
}

// 将单个主站采集结果累计到本轮统计。
void PollingService::account_master_collection_result(
    const MasterNodeConfig& master,
    const MasterCollectionResult& result,
    std::size_t* success_master_count,
    std::size_t* failed_master_count,
    std::size_t* success_device_count,
    std::size_t* failed_device_count,
    std::string* first_error_message)
{
    if (!result.success) {
        if (failed_master_count != nullptr) {
            ++(*failed_master_count);
        }
        std::string current_error_message =
            !result.error_message.empty() ? result.error_message : result.collect_result.error_message;
        std::string error_target_id = master.master_id;
        if (result.communication_success && !result.mapping_success) {
            for (const auto& device_status : result.device_statuses) {
                if (diagnosis_has_issue(device_status.diagnosis)) {
                    error_target_id = device_status.device_id;
                    if (!device_status.diagnosis.message.empty()) {
                        current_error_message = device_status.diagnosis.message;
                    }
                    break;
                }
            }
        }
        if (first_error_message != nullptr && first_error_message->empty()) {
            *first_error_message = current_error_message;
        }
        set_last_error(error_target_id, current_error_message, result.finished_at_ms);
        if (Logger::debug_enabled()) {
            Logger::debug(
                "轮询对象失败: " + error_target_id +
                "，错误=" + current_error_message);
        }
    } else if (success_master_count != nullptr) {
        ++(*success_master_count);
    }

    for (const auto& device_status : result.device_statuses) {
        if (device_status.last_collect_success) {
            if (success_device_count != nullptr) {
                ++(*success_device_count);
            }
        } else if (failed_device_count != nullptr) {
            ++(*failed_device_count);
        }
    }
}

// 主站采集失败时标记其下设备为采集失败。
std::vector<DeviceStatus> PollingService::mark_devices_collect_failed(
    const MasterPollingTarget& target,
    TimestampMs failure_time_ms,
    const std::string& error_message,
    DiagnosisErrorCode error_code)
{
    const auto& master_config = target.master;
    const auto& devices = target.devices;
    std::vector<DeviceStatus> failed_statuses;

    // 通道准备失败或轮询异常虽然没有发出请求，仍按完整 read_blocks 模型生成
    // 本轮 bad/invalid 点位，避免旧有效值继续以有效质量进入实时、MQTT 或页面。
    const auto& device_template = target.collector_runtime.device_template;
    const auto& plans = target.collector_runtime.read_plans;
    if (device_template != nullptr &&
        target.collector_runtime.preparation_error.empty() &&
        !plans.empty()) {
        std::vector<DeviceReadResult> read_results;
        read_results.reserve(devices.size());
        for (std::size_t device_index = 0; device_index < devices.size(); ++device_index) {
            DeviceReadResult device_result;
            device_result.device_id = devices[device_index]->device_id;
            device_result.device_index = static_cast<std::uint32_t>(device_index);
            device_result.device_base_address = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(master_config.block_start_register) +
                static_cast<std::uint64_t>(device_index) *
                    static_cast<std::uint64_t>(device_template->device_address_stride));
            read_results.push_back(std::move(device_result));
        }
        const auto failure_code = error_code;
        for (const auto& plan : plans) {
            DeviceReadBlockResult block_result;
            block_result.block_key = plan.block_key;
            block_result.display_name = plan.block_display_name;
            block_result.function_code = plan.function_code;
            block_result.request_address = plan.request_address;
            block_result.register_count = plan.register_count;
            block_result.success = false;
            block_result.diagnosis_error_code =
                failure_code == DiagnosisErrorCode::kNone
                    ? DiagnosisErrorCode::kUnknownError
                    : failure_code;
            block_result.error_message = error_message;
            read_results[plan.device_index].read_blocks.insert_or_assign(
                plan.block_key, std::move(block_result));
        }
        auto mapped = RegisterMapper::map_devices(
            master_config,
            devices,
            target.mapper_runtime,
            read_results,
            failure_time_ms);
        if (mapped.device_statuses.size() == devices.size()) {
            failed_statuses = prepare_device_statuses_for_store(mapped.device_statuses);
        }
    }

    if (failed_statuses.size() != devices.size()) {
        failed_statuses = build_failed_device_statuses(
            devices, failure_time_ms, error_message, error_code);
    }
    if (!failed_statuses.empty()) {
        publish_device_statuses(failed_statuses);
    }
    return failed_statuses;
}

// 构造设备采集失败状态列表。
std::vector<DeviceStatus> PollingService::build_failed_device_statuses(
    const std::vector<const DeviceConfig*>& devices,
    TimestampMs failure_time_ms,
    const std::string& error_message,
    DiagnosisErrorCode error_code) const
{
    std::vector<DeviceStatus> statuses;
    statuses.reserve(devices.size());

    std::unordered_map<DeviceId, DeviceStatus> cached_by_id;
    std::vector<DeviceId> device_ids;
    device_ids.reserve(devices.size());
    for (const auto* device : devices) {
        if (device != nullptr) device_ids.push_back(device->device_id);
    }
    const auto cached_statuses = data_store_.get_device_statuses(device_ids);
    cached_by_id.reserve(cached_statuses.size());
    for (const auto& cached : cached_statuses) {
        cached_by_id.emplace(cached.device_id, cached);
    }

    for (const auto* device : devices) {
        if (device == nullptr) {
            continue;
        }

        DeviceStatus status;
        const auto cached = cached_by_id.find(device->device_id);
        if (cached != cached_by_id.end()) {
            status = cached->second;
        }

        status.device_id = device->device_id;
        status.device_name = device->device_name;
        status.master_id = device->master_id;
        status.online = false;
        status.last_collect_success = false;
        status.communication_quality = DataQuality::kBad;
        status.has_resistance = false;
        for (auto& point : status.points) {
            point.valid = false;
            point.quality = DataQuality::kBad;
        }
        status.updated_at_ms = failure_time_ms;
        status.last_failure_time_ms = failure_time_ms;
        status.diagnosis = make_diagnosis(
            DiagnosisLevel::kDevice,
            device->device_id,
            device->device_name,
            DiagnosisRunStatus::kOffline,
            error_code == DiagnosisErrorCode::kNone ? DiagnosisErrorCode::kUnknownError : error_code,
            status.last_success_time_ms,
            failure_time_ms,
            status.diagnosis.consecutive_failures + 1);
        status.diagnosis.message = error_message;
        status.last_error_message = error_message;
        statuses.push_back(status);
    }

    return statuses;
}

// 记录采集服务最近一次错误。
void PollingService::set_last_error(
    const std::string& target_id,
    const std::string& message,
    TimestampMs timestamp_ms)
{
    // 有结构化状态时使用原码；无状态的线程异常保留详情，不从文字猜协议原因。
    auto diagnosis = make_diagnosis(
        DiagnosisLevel::kSystem, target_id, target_id, DiagnosisRunStatus::kError,
        DiagnosisErrorCode::kUnknownError, 0, timestamp_ms, 0);
    if (!message.empty()) {
        // 诊断码用于分类，运行时原始文本用于向启动调用方和运维页面解释具体失败原因。
        diagnosis.message = message;
    }
    // 优先选通道、主站、设备诊断；选定后共用一处摘要更新和回调出口。
    std::string source = diagnosis.level.empty() ? "polling" : diagnosis.level;
    const auto select_diagnosis = [&](const auto& status, const char* fallback_source) {
        if (!status.has_value() || !diagnosis_has_issue(status->diagnosis)) return false;
        diagnosis = status->diagnosis;
        source = diagnosis.level.empty() ? fallback_source : diagnosis.level;
        return true;
    };
    const bool selected =
        select_diagnosis(data_store_.get_channel_status(target_id), "channel") ||
        select_diagnosis(data_store_.get_master_status(target_id), "master") ||
        select_diagnosis(data_store_.get_device_status(target_id), "device");
    {
        std::lock_guard<std::mutex> lock(summary_mutex_);
        last_error_summary_.has_error = selected || !message.empty();
        last_error_summary_.source = source;
        last_error_summary_.target_id = diagnosis.target_id;
        last_error_summary_.diagnosis = diagnosis;
        last_error_summary_.message = diagnosis.message;
        last_error_summary_.timestamp_ms = selected ? diagnosis.last_error_time_ms : timestamp_ms;
    }
    const auto callback = error_event_callback_;
    if (callback) {
        callback(source, diagnosis.target_id, diagnosis.message, timestamp_ms);
    }
}

}  // namespace edge_controller
