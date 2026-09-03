// 轮询启停协调层：耗时 stop 在主服务锁外执行，避免阻塞其他 IPC 状态查询。
#include "application/service/backend_service.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "shared/common/filesystem_compat.h"
#include "shared/common/logger.h"
#include "shared/common/readable_error.h"
#include "shared/common/time_utils.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

namespace {

// 停止轮询服务外部锁。
void stop_polling_service_outside_lock(std::unique_ptr<PollingService>& polling_service)
{
    if (polling_service != nullptr) {
        polling_service->stop();
    }
}

}  // namespace

// 启动后端采集轮询。
StatusCode BackendService::start_polling()
{
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    if (config_apply_in_progress_.load()) {
        return StatusCode::kInvalidState;
    }
    std::string error_message;
    return start_polling_locked(&error_message);
}

// 停止后端采集轮询。
void BackendService::stop_polling()
{
    std::unique_ptr<PollingService> polling_to_stop;
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        if (config_apply_in_progress_.load()) {
            return;
        }
        polling_to_stop = detach_polling_service_locked("stopping", "轮询停止中");
    }
    stop_polling_service_outside_lock(polling_to_stop);
    {
        std::unique_lock<std::shared_mutex> lock(service_mutex_);
        merge_stopped_polling_service_locked(
            polling_to_stop.get(),
            initialized_ ? "stopped" : "not_started",
            initialized_ ? "轮询已停止" : "后端服务已停止");
    }
}

// 判断采集轮询是否正在运行。
bool BackendService::is_polling_running() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return is_polling_running_locked();
}

// 在持锁状态下判断采集轮询是否正在运行。
bool BackendService::is_polling_running_locked() const
{
    return polling_service_ != nullptr && polling_service_->is_running();
}

// 判断当前配置中是否存在启用的采集目标。
bool BackendService::has_enabled_collection_target_locked() const
{
    for (const auto& master : system_config_.master_nodes) {
        if (!master.enabled ||
            (master.protocol != MasterProtocol::kModbusRtu &&
             master.protocol != MasterProtocol::kModbusTcp)) {
            continue;
        }

        const auto* channel = find_channel_config(master.channel_id);
        if (channel == nullptr || !channel->enabled) {
            continue;
        }

        const auto& devices = topology_manager_.get_devices_by_master(master.master_id);
        for (const auto* device : devices) {
            if (device != nullptr && device->enabled) {
                return true;
            }
        }
    }
    return false;
}

// 在持锁状态下创建并启动轮询服务。
StatusCode BackendService::start_polling_locked(std::string* error_message)
{
    if (!initialized_) {
        if (error_message != nullptr) {
            *error_message = "后端服务尚未初始化";
        }
        return StatusCode::kInvalidState;
    }
    if (is_polling_running_locked()) {
        if (error_message != nullptr) {
            *error_message = "轮询服务已经在运行";
        }
        return StatusCode::kInvalidState;
    }
    if (!has_enabled_collection_target_locked()) {
        auto runtime_status = data_store_.get_system_status();
        runtime_status.polling_running = false;
        runtime_status.polling_state = "stopped";
        runtime_status.last_poll_cycle_started_at_ms = 0;
        runtime_status.last_poll_cycle_finished_at_ms = 0;
        runtime_status.last_poll_cycle_master_count = 0;
        runtime_status.last_poll_cycle_success_master_count = 0;
        runtime_status.last_poll_cycle_failed_master_count = 0;
        runtime_status.last_poll_cycle_success_device_count = 0;
        runtime_status.last_poll_cycle_failed_device_count = 0;
        runtime_status.last_poll_cycle_has_error = false;
        runtime_status.diagnosis = make_diagnosis(
            DiagnosisLevel::kSystem,
            "polling",
            "轮询服务",
            DiagnosisRunStatus::kWarning,
            DiagnosisErrorCode::kPollingNotRunning,
            0,
            time_utils::system_now_ms(),
            0);
        runtime_status.last_poll_cycle_error_message.clear();
        runtime_status.last_status_message = "当前无有效启用采集目标，轮询未启动";
        runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
        data_store_.update_system_status(runtime_status);
        if (error_message != nullptr) {
            *error_message = runtime_status.last_status_message;
        }
        return StatusCode::kInvalidState;
    }

    polling_service_ = std::make_unique<PollingService>(
        &system_config_,
        &topology_manager_,
        &channel_manager_,
        &data_store_,
        &history_store_,
        &communication_trace_store_,
        &alarm_evaluator_,
        system_config_.default_poll_interval_ms,
        [this](const std::string& source,
               const std::string& target_id,
               const std::string& message,
               TimestampMs timestamp_ms) {
            set_last_error(source, target_id, message, timestamp_ms);
        });
    const std::weak_ptr<ModbusExportService> weak_export_service = modbus_export_service_;
    polling_service_->set_device_status_update_callback(
        [weak_export_service](const std::vector<DeviceStatus>& statuses) {
            if (const auto export_service = weak_export_service.lock()) {
                export_service->update_device_statuses(statuses);
            }
        });
    history_sampling_service_ = polling_service_.get();

    const auto status = polling_service_->start();
    if (!is_ok(status)) {
        const auto polling_error = polling_service_->get_last_error_summary();
        history_sampling_service_ = nullptr;
        polling_service_.reset();
        if (error_message != nullptr) {
            *error_message = polling_error.has_error && !polling_error.message.empty()
                                 ? polling_error.message
                                 : "启动轮询服务失败";
        }
        return status;
    }

    auto runtime_status = data_store_.get_system_status();
    runtime_status.polling_running = true;
    runtime_status.polling_state = "running";
    runtime_status.last_status_message = "轮询已启动";
    runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
    data_store_.update_system_status(runtime_status);
    return StatusCode::kOk;
}

// 在持锁状态下摘出当前轮询服务，供锁外停止。
std::unique_ptr<PollingService> BackendService::detach_polling_service_locked(
    const std::string& polling_state,
    const std::string& status_message)
{
    auto polling_service = std::move(polling_service_);
    if (polling_service != nullptr) {
        polling_service->set_device_status_update_callback({});
        auto runtime_status = data_store_.get_system_status();
        runtime_status.polling_running = true;
        runtime_status.polling_state = polling_state;
        runtime_status.last_status_message = status_message;
        runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
        data_store_.update_system_status(runtime_status);
    }
    return polling_service;
}

// 配置应用前暂停轮询，避免采集线程和配置重载同时访问通道。
void BackendService::stop_polling_for_config_apply(
    std::unique_lock<std::shared_mutex>& lock,
    std::unique_ptr<PollingService>* polling_to_stop)
{
    if (polling_to_stop == nullptr) {
        return;
    }
    // 停采可能等待串口/网络 IO 结束，必须在锁外执行，避免阻塞状态查询和错误上报路径。
    *polling_to_stop = detach_polling_service_locked("stopping", "轮询停止中，准备应用配置");
    lock.unlock();
    stop_polling_service_outside_lock(*polling_to_stop);
    lock.lock();
    merge_stopped_polling_service_locked(polling_to_stop->get(), "config_applying", "配置应用中");
}

// 合并已停止轮询服务的摘要和错误状态。
void BackendService::merge_stopped_polling_service_locked(
    const PollingService* stopped_service,
    const std::string& polling_state,
    const std::string& status_message)
{
    if (history_sampling_service_ == stopped_service) {
        history_sampling_service_ = nullptr;
    }
    if (stopped_service != nullptr) {
        const auto polling_error = stopped_service->get_last_error_summary();
        if (polling_error.has_error) {
            set_last_error(
                polling_error.source,
                polling_error.target_id,
                polling_error.message,
                polling_error.timestamp_ms);
        }
    }
    auto runtime_status = data_store_.get_system_status();
    runtime_status.polling_running = false;
    runtime_status.polling_state = polling_state;
    runtime_status.last_status_message = status_message;
    runtime_status.last_heartbeat_ms = time_utils::steady_now_ms();
    data_store_.update_system_status(runtime_status);
}

}  // namespace edge_controller
