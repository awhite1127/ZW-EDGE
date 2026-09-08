#include "communication/collect/transport_diagnosis.h"
// 单主站多区块采集编排：按设备与读取区块顺序执行协议请求并汇总局部失败。
#include "communication/collect/master_collector.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "communication/collect/block_read_plan.h"
#include "communication/collect/modbus_trace_recorder.h"
#include "shared/common/enums.h"
#include "shared/common/format_utils.h"
#include "shared/common/logger.h"
#include "shared/common/status_code.h"
#include "shared/common/time_utils.h"
#include "communication/protocol/modbus_rtu_protocol.h"
#include "communication/protocol/modbus_tcp_client.h"

namespace edge_controller {

namespace {

// 返回通道类型文本。
std::string channel_type_text(const ChannelConfig& config)
{
    return config.channel_type == ChannelType::kModbusTcp ? "modbus_tcp" : "modbus_rtu_serial";
}

// 返回 Modbus 功能码文本。
std::string function_code_text(std::uint32_t function_code)
{
    return function_code == ModbusRtuProtocol::kReadHoldingRegistersFunction ? "FC03" : "FC04";
}

// 根据 RTU 传输结果生成诊断码。
DiagnosisErrorCode rtu_transport_diagnosis(
    StatusCode transport_status,
    const ChannelStatus& channel_status,
    const std::string& error_message)
{
    (void)error_message;
    return transport_diagnosis(transport_status, channel_status, false);
}

// 生成读取计划标签。
std::string plan_label(const BlockReadPlan& plan)
{
    return plan.block_display_name.empty() ? plan.block_key : plan.block_display_name;
}

// 汇总并压缩采集失败信息。
std::string compact_failure_summary(
    const std::vector<std::string>& failed_labels,
    const std::string& first_failure_detail,
    bool any_success)
{
    if (failed_labels.empty()) return {};
    if (failed_labels.size() == 1 && !first_failure_detail.empty()) {
        return (any_success ? "部分读取区块失败：" : "读取区块失败：") + first_failure_detail;
    }
    std::ostringstream stream;
    if (!any_success) {
        stream << "所有读取区块失败（" << failed_labels.size() << " 个）：";
    } else {
        stream << failed_labels.size() << " 个读取区块失败：";
    }
    const auto shown = std::min<std::size_t>(failed_labels.size(), 3);
    for (std::size_t index = 0; index < shown; ++index) {
        if (index > 0) stream << "、";
        stream << failed_labels[index];
    }
    if (failed_labels.size() > shown) stream << "等";
    return stream.str();
}

}  // namespace

// 构造 MasterCollector 并保存运行依赖。
MasterCollector::MasterCollector(
    ChannelManager* channel_manager,
    CommunicationTraceStore* communication_trace_store)
    : channel_manager_(channel_manager),
      communication_trace_store_(communication_trace_store)
{
}

// 解析模板并生成配置世代内可复用的读取计划。
MasterCollectorRuntimePlan MasterCollector::build_runtime_plan(
    const MasterNodeConfig& master_config,
    const std::vector<const DeviceConfig*>& devices)
{
    MasterCollectorRuntimePlan runtime_plan;
    runtime_plan.device_template =
        find_device_template(master_config.device_template);
    if (runtime_plan.device_template == nullptr) {
        runtime_plan.preparation_error =
            "未找到主控绑定的设备类型: " + master_config.device_template;
        return runtime_plan;
    }

    const auto plan_status = build_block_read_plan(
        master_config,
        *runtime_plan.device_template,
        devices,
        &runtime_plan.read_plans,
        &runtime_plan.preparation_error);
    if (!is_ok(plan_status) && runtime_plan.preparation_error.empty()) {
        runtime_plan.preparation_error = "生成多读取区块计划失败";
    }
    return runtime_plan;
}

// 兼容直接调用者：临时构建运行计划；PollingService 使用下方重载跨周期复用。
MasterCollectResult MasterCollector::collect_once(
    const MasterNodeConfig& master_config,
    const std::vector<const DeviceConfig*>& devices,
    MasterNodeStatus* master_status,
    const std::atomic<bool>* cancel_requested)
{
    return collect_once(
        master_config,
        devices,
        build_runtime_plan(master_config, devices),
        master_status,
        cancel_requested);
}

// 按预生成读取计划采集指定主站下的全部设备，并汇总主站运行状态。
MasterCollectResult MasterCollector::collect_once(
    const MasterNodeConfig& master_config,
    const std::vector<const DeviceConfig*>& devices,
    const MasterCollectorRuntimePlan& runtime_plan,
    MasterNodeStatus* master_status,
    const std::atomic<bool>* cancel_requested)
{
    // 初始化本轮结果，并准备无通信失败时的统一状态更新逻辑。
    MasterCollectResult result;
    const auto started_at_ms = time_utils::system_now_ms();
    const auto cancelled = [&]() {
        return cancel_requested != nullptr && cancel_requested->load();
    };
    const auto cancelled_result = [&]() {
        MasterCollectResult stopped;
        stopped.error_message = "轮询停止中，采集已取消";
        stopped.timestamp_ms = time_utils::system_now_ms();
        return stopped;
    };
    if (cancelled()) {
        return cancelled_result();
    }

    if (master_status != nullptr) {
        master_status->master_id = master_config.master_id;
        master_status->last_poll_time_ms = started_at_ms;
        master_status->last_collect_success = false;
        master_status->last_register_block.clear();
    }

    auto fail_without_io = [&](const std::string& message, DiagnosisErrorCode error_code) {
        const auto finished_at_ms = time_utils::system_now_ms();
        result.success = false;
        result.any_success = false;
        result.communication_quality = DataQuality::kBad;
        result.error_message = message;
        result.diagnosis_error_code = error_code;
        result.timestamp_ms = finished_at_ms;
        if (master_status != nullptr) {
            master_status->online = false;
            master_status->last_collect_success = false;
            master_status->communication_quality = DataQuality::kBad;
            master_status->last_failure_time_ms = finished_at_ms;
            ++master_status->consecutive_failure_count;
            master_status->consecutive_timeout_count =
                error_code == DiagnosisErrorCode::kModbusTimeout
                    ? master_status->consecutive_timeout_count + 1U
                    : 0U;
            master_status->last_cycle_duration_ms =
                static_cast<std::uint32_t>(finished_at_ms - started_at_ms);
            master_status->diagnosis = make_diagnosis(
                DiagnosisLevel::kMaster,
                master_config.master_id,
                master_config.master_name,
                error_code == DiagnosisErrorCode::kModbusTimeout
                    ? DiagnosisRunStatus::kOffline
                    : DiagnosisRunStatus::kError,
                error_code,
                master_status->last_success_time_ms,
                finished_at_ms,
                master_status->consecutive_failure_count);
            master_status->diagnosis.message = message;
            master_status->last_error_message = message;
        }
        return result;
    };

    // 校验运行依赖、协议、通道和设备类型，再生成读取计划。
    if (channel_manager_ == nullptr) {
        return fail_without_io("通道管理器未初始化", DiagnosisErrorCode::kConfigInvalid);
    }
    if (master_config.protocol != MasterProtocol::kModbusRtu &&
        master_config.protocol != MasterProtocol::kModbusTcp) {
        return fail_without_io("当前采集链路仅支持 Modbus RTU 或 Modbus TCP", DiagnosisErrorCode::kConfigInvalid);
    }

    auto* channel = channel_manager_->get_channel(master_config.channel_id);
    if (channel == nullptr) {
        return fail_without_io("未找到通道: " + master_config.channel_id, DiagnosisErrorCode::kConfigInvalid);
    }
    const auto& channel_config = channel->config();

    if (runtime_plan.device_template == nullptr) {
        return fail_without_io(
            runtime_plan.preparation_error.empty()
                ? "未找到主控绑定的设备类型: " +
                      master_config.device_template
                : runtime_plan.preparation_error,
            DiagnosisErrorCode::kConfigInvalid);
    }
    if (!runtime_plan.preparation_error.empty()) {
        return fail_without_io(
            runtime_plan.preparation_error,
            DiagnosisErrorCode::kConfigInvalid);
    }
    if (runtime_plan.read_plans.empty()) {
        return fail_without_io(
            "生成多读取区块计划失败：读取计划为空",
            DiagnosisErrorCode::kConfigInvalid);
    }

    const auto& device_template = *runtime_plan.device_template;
    const auto& plans = runtime_plan.read_plans;
    result.total_block_count = plans.size();
    // 预先为每台设备建立结果容器，读取区块按计划逐项写入。
    result.device_results.reserve(devices.size());
    for (std::size_t device_index = 0; device_index < devices.size(); ++device_index) {
        DeviceReadResult device_result;
        device_result.device_id = devices[device_index]->device_id;
        device_result.device_index = static_cast<std::uint32_t>(device_index);
        device_result.device_base_address = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(master_config.block_start_register) +
            static_cast<std::uint64_t>(device_index) * device_template.device_address_stride);
        device_result.read_blocks.reserve(device_template.read_blocks.size());
        result.device_results.push_back(std::move(device_result));
    }

    if (Logger::debug_enabled()) {
        Logger::debug(
            "开始多区块采集主控 " + master_config.master_id +
            "，主控名称=" + master_config.master_name +
            "，通道=" + master_config.channel_id +
            "，通道类型=" + channel_type_text(channel_config) +
            "，目标=" + channel_target_description(channel_config, "未配置串口设备") +
            "，设备数量=" + std::to_string(master_config.device_count) +
            "，区块数量=" + std::to_string(device_template.read_blocks.size()) +
            "，请求总数=" + std::to_string(plans.size()));
    }

    // 计算重试和超时参数，并按协议准备 TCP 客户端。
    const auto effective_timeout_ms =
        master_config.response_timeout_ms > 0
            ? master_config.response_timeout_ms
            : channel_config.response_timeout_ms;
    const auto slave_address = static_cast<std::uint8_t>(master_config.target_address);
    const auto max_attempts = 1U + master_config.retry_count;
    const bool use_tcp = master_config.protocol == MasterProtocol::kModbusTcp;
    ModbusTcpClient tcp_client(channel, cancel_requested);
    const auto rtu_target =
        use_tcp ? std::string{} : channel_target_description(channel_config, "未配置串口设备");

    std::vector<std::string> failed_labels;
    std::string first_failure_detail;
    DiagnosisErrorCode first_failure_code = DiagnosisErrorCode::kNone;

    // 逐区块执行读取；每次尝试都记录通讯报文和诊断信息。
    for (const auto& plan : plans) {
        if (cancelled()) {
            return cancelled_result();
        }
        DeviceReadBlockResult block_result;
        block_result.block_key = plan.block_key;
        block_result.display_name = plan.block_display_name;
        block_result.function_code = plan.function_code;
        block_result.request_address = plan.request_address;
        block_result.register_count = plan.register_count;

        ChannelTraceContext trace_context;
        if (!use_tcp) {
            trace_context.channel_id = master_config.channel_id;
            trace_context.channel_name = channel_config.channel_name;
            trace_context.master_id = master_config.master_id;
            trace_context.master_name = master_config.master_name;
            trace_context.target = rtu_target;
            trace_context.device_id = plan.device_id;
            trace_context.block_key = plan.block_key;
            trace_context.block_display_name = plan.block_display_name;
        }

        StatusCode final_status = StatusCode::kInternalError;
        for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
            if (cancelled()) {
                return cancelled_result();
            }
            const auto request_started_at_ms = time_utils::system_now_ms();
            block_result.success = false;
            block_result.registers.clear();
            block_result.error_message.clear();
            block_result.diagnosis_error_code = DiagnosisErrorCode::kNone;

            if (use_tcp) {
                ModbusReadResult read_result;
                final_status = tcp_client.read_registers(
                    master_config,
                    static_cast<std::uint8_t>(plan.function_code),
                    plan.request_address,
                    plan.register_count,
                    &read_result);
                if (cancelled()) {
                    return cancelled_result();
                }
                block_result.success = is_ok(final_status) && read_result.success;
                block_result.registers = std::move(read_result.registers);
                block_result.error_message = read_result.error_message;
                block_result.diagnosis_error_code = read_result.diagnosis_error_code;
                if (!block_result.success && block_result.diagnosis_error_code == DiagnosisErrorCode::kNone) {
                    block_result.diagnosis_error_code = transport_diagnosis(final_status, channel->status(), true);
                }
                append_modbus_trace(
                    communication_trace_store_,
                    master_config,
                    plan.function_code,
                    plan.request_address,
                    plan.register_count,
                    read_result.request_frame,
                    read_result.response_frame,
                    request_started_at_ms,
                    read_result.transport_status,
                    block_result.error_message,
                    static_cast<std::int64_t>(plan.device_index),
                    plan.device_id,
                    plan.block_key,
                    plan.block_display_name);
            } else {
                const auto& request = plan.rtu_request_frame;
                if (request.empty()) {
                    final_status = StatusCode::kInvalidArgument;
                    block_result.error_message = "构造 " + function_code_text(plan.function_code) + " 请求失败，参数无效";
                    block_result.diagnosis_error_code = DiagnosisErrorCode::kConfigInvalid;
                } else {
                    std::vector<std::uint8_t> response;
                    final_status = channel->transceive(
                        request,
                        static_cast<int>(effective_timeout_ms),
                        &response,
                        trace_context,
                        cancel_requested);
                    if (cancelled()) {
                        return cancelled_result();
                    }
                    if (!is_ok(final_status)) {
                        const auto channel_status = channel->status();
                        block_result.error_message = channel_status.last_error_message.empty()
                                                       ? "通道响应超时"
                                                       : channel_status.last_error_message;
                        block_result.diagnosis_error_code = rtu_transport_diagnosis(
                            final_status, channel_status, block_result.error_message);
                    } else {
                        std::string parse_error;
                        final_status = ModbusRtuProtocol::parse_read_registers_response(
                            slave_address,
                            static_cast<std::uint8_t>(plan.function_code),
                            plan.register_count,
                            response,
                            &block_result.registers,
                            &parse_error, &block_result.diagnosis_error_code);
                        if (is_ok(final_status)) {
                            block_result.success = true;
                            block_result.diagnosis_error_code = DiagnosisErrorCode::kNone;
                        } else {
                            block_result.error_message = parse_error;

                        }
                    }
                    append_modbus_trace(
                        communication_trace_store_,
                        master_config,
                        plan.function_code,
                        plan.request_address,
                        plan.register_count,
                        request,
                        response,
                        request_started_at_ms,
                        final_status,
                        block_result.error_message,
                        static_cast<std::int64_t>(plan.device_index),
                        plan.device_id,
                        plan.block_key,
                        plan.block_display_name);
                }
            }
 
            if (block_result.success) {
                if (Logger::debug_enabled()) {
                    Logger::debug(
                        "设备 " + plan.device_id + " 区块 " + plan.block_key + " " +
                        function_code_text(plan.function_code) + " 地址=" +
                        std::to_string(plan.request_address) + " 数量=" +
                        std::to_string(plan.register_count) + " 读取成功，寄存器=" +
                        format_utils::registers_to_string(block_result.registers));
                }
                break;
            }
            if (block_result.error_message.empty()) block_result.error_message = "Modbus 读取失败";
            if (Logger::debug_enabled()) {
                Logger::debug(
                    "设备 " + plan.device_id + " 区块 " + plan.block_key +
                    " 第 " + std::to_string(attempt) + " 次读取失败：" +
                    block_result.error_message);
            }
        }

        if (block_result.success) {
            ++result.successful_block_count;
        } else {
            ++result.failed_block_count;
            if (first_failure_code == DiagnosisErrorCode::kNone) {
                first_failure_code = block_result.diagnosis_error_code == DiagnosisErrorCode::kNone
                                         ? DiagnosisErrorCode::kUnknownError
                                         : block_result.diagnosis_error_code;
            }
            const auto label = plan.device_id + "/" + plan_label(plan);
            failed_labels.push_back(label);
            if (first_failure_detail.empty()) {
                first_failure_detail =
                    label + "（" + function_code_text(plan.function_code) + "，地址 " +
                    std::to_string(plan.request_address) + "，数量 " +
                    std::to_string(plan.register_count) + "）：" + block_result.error_message;
            }
        }
        result.device_results[plan.device_index].read_blocks.insert_or_assign(
            plan.block_key, std::move(block_result));
    }

    // 汇总区块成功率，并将本轮结果写回主站状态。
    result.any_success = result.successful_block_count > 0;
    result.success = result.failed_block_count == 0 && result.total_block_count > 0;
    result.communication_quality = result.success
                              ? DataQuality::kGood
                              : (result.any_success ? DataQuality::kPartial : DataQuality::kBad);
    result.diagnosis_error_code = result.success ? DiagnosisErrorCode::kNone : first_failure_code;
    result.error_message = compact_failure_summary(
        failed_labels, first_failure_detail, result.any_success);
    result.timestamp_ms = time_utils::system_now_ms();

    if (master_status != nullptr) {
        master_status->online = result.any_success;
        master_status->last_collect_success = result.success;
        master_status->communication_quality = result.communication_quality;
        master_status->last_cycle_duration_ms =
            static_cast<std::uint32_t>(result.timestamp_ms - started_at_ms);
        master_status->last_register_block.clear();
        if (result.success) {
            master_status->last_success_time_ms = result.timestamp_ms;
            master_status->last_failure_time_ms = 0;
            master_status->consecutive_failure_count = 0;
            master_status->consecutive_timeout_count = 0;
            master_status->diagnosis = make_normal_diagnosis(
                DiagnosisLevel::kMaster,
                master_config.master_id,
                master_config.master_name,
                result.timestamp_ms);
            master_status->last_error_message.clear();
        } else {
            master_status->last_failure_time_ms = result.timestamp_ms;
            ++master_status->consecutive_failure_count;
            master_status->consecutive_timeout_count =
                result.diagnosis_error_code == DiagnosisErrorCode::kModbusTimeout
                    ? master_status->consecutive_timeout_count + 1U
                    : 0U;
            master_status->diagnosis = make_diagnosis(
                DiagnosisLevel::kMaster,
                master_config.master_id,
                master_config.master_name,
                result.any_success ? DiagnosisRunStatus::kWarning : DiagnosisRunStatus::kOffline,
                result.diagnosis_error_code,
                master_status->last_success_time_ms,
                result.timestamp_ms,
                master_status->consecutive_failure_count);
            master_status->diagnosis.message = result.error_message;
            master_status->last_error_message = result.error_message;
        }
    }

    return result;
}

}  // namespace edge_controller
