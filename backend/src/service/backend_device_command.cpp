// 校验并执行设备模板声明的主动读写命令。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#include "service/backend_service.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <utility>

#include "collect/modbus_trace_recorder.h"
#include "collect/modbus_write_executor.h"
#include "common/enums.h"
#include "common/format_utils.h"
#include "common/time_utils.h"
#include "model/diagnosis_status.h"
#include "protocol/modbus_rtu_protocol.h"
#include "protocol/modbus_tcp_client.h"

namespace edge_controller {

namespace {

// 返回状态码对应的稳定名称。
const char* status_code_name(StatusCode status)
{
    switch (status) {
    case StatusCode::kOk:
        return "kOk";
    case StatusCode::kInvalidArgument:
        return "kInvalidArgument";
    case StatusCode::kInvalidState:
        return "kInvalidState";
    case StatusCode::kNotFound:
        return "kNotFound";
    case StatusCode::kTimeout:
        return "kTimeout";
    case StatusCode::kIoError:
        return "kIoError";
    case StatusCode::kProtocolError:
        return "kProtocolError";
    case StatusCode::kInternalError:
        return "kInternalError";
    case StatusCode::kConflict:
        return "kConflict";
    }
    return "kUnknown";
}

void set_error_response(
    ModbusWriteMultipleRegistersResponse* response,
    StatusCode status,
    DiagnosisErrorCode diagnosis_error_code,
    const std::string& message)
{
    if (response == nullptr) {
        return;
    }
    response->success = false;
    response->status = status_code_name(status);
    response->diagnosis_error_code = to_string(diagnosis_error_code);
    response->error_message = message;
    response->timestamp_ms = time_utils::system_now_ms();
}

void set_read_error_response(
    ModbusReadHoldingRegistersResponse* response,
    StatusCode status,
    DiagnosisErrorCode diagnosis_error_code,
    const std::string& message)
{
    if (response == nullptr) {
        return;
    }
    response->success = false;
    response->status = status_code_name(status);
    response->diagnosis_error_code = to_string(diagnosis_error_code);
    response->error_message = message;
    response->timestamp_ms = time_utils::system_now_ms();
}

// 填充主站响应字段。
void fill_master_response_fields(
    const MasterNodeConfig& master,
    const ChannelConfig* channel,
    ModbusWriteMultipleRegistersResponse* response)
{
    if (response == nullptr) {
        return;
    }
    response->master_id = master.master_id;
    response->master_name = master.master_name;
    response->channel_id = master.channel_id;
    response->channel_name = channel == nullptr ? std::string{} : channel->channel_name;
    response->protocol = to_string(master.protocol);
    response->slave_address = master.target_address;
}

// 填充主站响应字段。
void fill_master_response_fields(
    const MasterNodeConfig& master,
    const ChannelConfig* channel,
    ModbusReadHoldingRegistersResponse* response)
{
    if (response == nullptr) {
        return;
    }
    response->master_id = master.master_id;
    response->master_name = master.master_name;
    response->channel_id = master.channel_id;
    response->channel_name = channel == nullptr ? std::string{} : channel->channel_name;
    response->protocol = to_string(master.protocol);
    response->slave_address = master.target_address;
}

// 将写执行结果填充到设备命令响应。
void fill_executor_response(
    const ModbusWriteMultipleRegistersResult& result,
    ModbusWriteMultipleRegistersResponse* response)
{
    if (response == nullptr) {
        return;
    }
    response->success = result.success;
    response->register_count = result.register_count;
    response->request_hex = format_utils::bytes_to_hex(result.request_frame);
    response->response_hex = format_utils::bytes_to_hex(result.response_frame);
    response->status = status_code_name(result.transport_status);
    response->diagnosis_error_code = to_string(result.diagnosis_error_code);
    response->error_message = result.error_message;
    response->timestamp_ms = result.timestamp_ms == 0 ? time_utils::system_now_ms() : result.timestamp_ms;
}

// 填充读取响应。
void fill_read_response(
    const ModbusReadResult& result,
    ModbusReadHoldingRegistersResponse* response)
{
    if (response == nullptr) {
        return;
    }
    response->success = result.success;
    response->values = result.registers;
    response->request_hex = format_utils::bytes_to_hex(result.request_frame);
    response->response_hex = format_utils::bytes_to_hex(result.response_frame);
    response->status = status_code_name(result.transport_status);
    response->diagnosis_error_code = to_string(result.diagnosis_error_code);
    response->error_message = result.error_message;
    response->timestamp_ms = result.timestamp_ms == 0 ? time_utils::system_now_ms() : result.timestamp_ms;
}

// 将 EM100 分散的时间寄存器组合为显示文本。
std::string packed_record_time(std::uint16_t year_month, std::uint16_t day_hour, std::uint16_t minute_second)
{
    auto year = static_cast<int>((year_month >> 8U) & 0xFFU);
    const auto month = static_cast<int>(year_month & 0xFFU);
    const auto day = static_cast<int>((day_hour >> 8U) & 0xFFU);
    const auto hour = static_cast<int>(day_hour & 0xFFU);
    const auto minute = static_cast<int>((minute_second >> 8U) & 0xFFU);
    const auto second = static_cast<int>(minute_second & 0xFFU);
    if (year > 0 && year < 100) {
        year += 2000;
    }
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return "暂无有效时间";
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return buffer;
}

struct EM100EventCodeInfo {
    std::string content;
    double scale{1.0};
    std::string unit;
    bool has_data{true};
};

// 返回 EM100 事件码对应的类型与说明。
EM100EventCodeInfo em100_event_code_info(std::uint16_t code)
{
    switch (code) {
    case 0:
        return {"绝缘电阻低", 1.0, "MΩ", true};
    case 1:
        return {"剩余电流越限", 1.0, "mA", true};
    case 2:
        return {"线路残压交流电压超标", 1.0, "V", true};
    case 3:
        return {"线路残压直流电压超标", 0.1, "V", true};
    case 4:
        return {"测试完成后，线路交流泄放超标", 1.0, "V", true};
    case 5:
        return {"测试完成后，线路直流泄放超标", 0.1, "V", true};
    case 6:
        return {"内部高压模块故障", 1.0, "", false};
    case 7:
        return {"测试中断，电机启动", 1.0, "", false};
    case 8:
        return {"测试中断", 1.0, "", false};
    case 9:
        return {"自动启动绝缘测试", 1.0, "", false};
    case 10:
        return {"手动启动绝缘测试", 1.0, "", false};
    case 11:
        return {"遥控启动绝缘测试", 1.0, "", false};
    case 12:
        return {"内部测试高压异常", 0.1, "V", true};
    case 13:
        return {"测试输出电压异常", 0.1, "V", true};
    case 14:
        return {"高压采集模块异常", 1.0, "", false};
    case 15:
        return {"测试中断，测量回路窜入交流", 1.0, "V", true};
    default:
        return {"未知事件代码 " + std::to_string(code), 1.0, "", true};
    }
}

// 缩放 EM100 原始记录值并附加工程单位。
std::string scaled_record_value(std::uint16_t raw, double scale, const std::string& unit)
{
    std::ostringstream stream;
    const auto value = static_cast<double>(raw) * scale;
    stream << std::fixed << std::setprecision(scale == 1.0 ? 0 : 1) << value;
    if (!unit.empty()) {
        stream << " " << unit;
    }
    return stream.str();
}

// 解析EM100事件寄存器。
void parse_em100_event_registers(EM100RecordReadResponse* response)
{
    if (response == nullptr || response->raw_registers.size() != 6) {
        return;
    }
    const auto& values = response->raw_registers;
    response->valid_record = (values[0] & 0x8000U) == 0U;
    response->unread_count = values[0] & 0x7FFFU;
    if (!response->valid_record) {
        response->content = "当前无未读事件记录";
        response->data_text = "-";
        response->record_time = "暂无有效时间";
        return;
    }
    const auto info = em100_event_code_info(values[2]);
    response->content = info.content;
    response->data_text = info.has_data ? scaled_record_value(values[1], info.scale, info.unit) : "-";
    response->record_time = packed_record_time(values[3], values[4], values[5]);
}

// 解析 EM100 测试记录寄存器。
void parse_em100_test_registers(EM100RecordReadResponse* response)
{
    if (response == nullptr || response->raw_registers.size() != 6) {
        return;
    }
    const auto& values = response->raw_registers;
    response->valid_record = (values[0] & 0x8000U) == 0U;
    response->unread_count = values[0] & 0x7FFFU;
    if (!response->valid_record) {
        response->content = "当前无未读测试记录";
        response->data_text = "-";
        response->ratio_type = "-";
        response->ratio_value_text = "-";
        response->record_time = "暂无有效时间";
        return;
    }
    response->content = "绝缘测试记录";
    response->data_text = scaled_record_value(values[1], 1.0, "MΩ");
    const bool is_polarization = (values[2] & 0x8000U) != 0U;
    response->ratio_type = is_polarization ? "极化值" : "吸收比";
    response->ratio_value_text = scaled_record_value(static_cast<std::uint16_t>(values[2] & 0x7FFFU), 0.01, "");
    response->record_time = packed_record_time(values[3], values[4], values[5]);
}

}  // namespace

// 对指定主站执行一次 Modbus 写多个保持寄存器。
StatusCode BackendService::write_multiple_holding_registers(
    const ModbusWriteMultipleRegistersRequest& request,
    ModbusWriteMultipleRegistersResponse* response,
    std::string* error_message)
{
    // 初始化响应、统一失败诊断并校验请求范围。
    if (response == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Modbus 写入响应输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    *response = ModbusWriteMultipleRegistersResponse{};
    response->master_id = request.master_id;
    response->function_code = 16;
    response->start_register = request.start_register;
    response->register_count = static_cast<std::uint32_t>(request.values.size());
    response->status = status_code_name(StatusCode::kOk);
    response->diagnosis_error_code = to_string(DiagnosisErrorCode::kNone);
    response->timestamp_ms = time_utils::system_now_ms();

    auto fail = [&](StatusCode status, DiagnosisErrorCode diagnosis_error_code, const std::string& message) {
        set_error_response(response, status, diagnosis_error_code, message);
        if (error_message != nullptr) {
            *error_message = message;
        }
        return status;
    };

    if (request.master_id.empty()) {
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid, "master_id 不能为空");
    }
    if (request.values.empty()) {
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid, "写入寄存器值不能为空");
    }
    if (request.values.size() > ModbusRtuProtocol::kMaxWriteMultipleRegisterCount) {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "单次写入保持寄存器数量超出范围，允许范围为 1-123");
    }
    const auto end_register =
        static_cast<std::uint32_t>(request.start_register) + static_cast<std::uint32_t>(request.values.size()) - 1U;
    if (end_register > 0xFFFFU) {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "写入寄存器范围超出 uint16 地址空间");
    }

    // 串行化手动写入，并在配置快照中查找主站和通道。
    std::lock_guard<std::mutex> command_lock(manual_modbus_mutex_);
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) {
        return fail(StatusCode::kInvalidState, DiagnosisErrorCode::kConfigInvalid, "后端服务尚未初始化");
    }

    const MasterNodeConfig* master = nullptr;
    std::size_t master_index = 0;
    std::string lookup_error;
    const auto master_status = require_master_config_locked(
        request.master_id,
        &master,
        &master_index,
        &lookup_error);
    (void)master_index;
    if (!is_ok(master_status) || master == nullptr) {
        return fail(
            master_status,
            DiagnosisErrorCode::kConfigInvalid,
            lookup_error.empty() ? "未找到主控: " + request.master_id : lookup_error);
    }

    const auto* channel = find_channel_config(master->channel_id);
    fill_master_response_fields(*master, channel, response);
    if (channel == nullptr) {
        return fail(
            StatusCode::kNotFound,
            DiagnosisErrorCode::kConfigInvalid,
            "未找到通道: " + master->channel_id);
    }
    if (!channel->enabled) {
        return fail(
            StatusCode::kInvalidState,
            DiagnosisErrorCode::kConfigInvalid,
            "通道未启用: " + channel->channel_id);
    }
    if (master->protocol != MasterProtocol::kModbusRtu &&
        master->protocol != MasterProtocol::kModbusTcp) {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "当前写入入口仅支持 Modbus RTU 或 Modbus TCP 主控");
    }

    // 等待当前共享通讯资源上的轮询事务自然结束，并阻止下一轮事务在 FC10 完成前进入。
    // 作用域退出会覆盖成功、超时、协议错误和通道错误等全部返回路径。
    auto communication_lease =
        channel_manager_.acquire_communication_lease(master->channel_id);

    // RTU 写入前准备独占通道，TCP 则直接复用配置。
    if (master->protocol == MasterProtocol::kModbusRtu) {
        std::string prepare_error;
        const auto prepare_status = channel_manager_.prepare_rtu_channel_for_collection(
            master->channel_id,
            &prepare_error);
        if (!is_ok(prepare_status)) {
            refresh_channel_statuses();
            return fail(
                prepare_status,
                prepare_status == StatusCode::kTimeout ? DiagnosisErrorCode::kModbusTimeout : DiagnosisErrorCode::kChannelOpenFailed,
                prepare_error.empty() ? "RTU 通道准备失败: " + master->channel_id : prepare_error);
        }
    }

    // 执行写入；RTU fd 保持打开，错误、配置重载或退出时统一关闭。
    ModbusWriteExecutor executor(&channel_manager_, &communication_trace_store_);
    const auto result = executor.write_multiple_holding_registers(
        *master,
        request.start_register,
        request.values);
    fill_executor_response(result, response);

    refresh_channel_statuses();

    if (!result.success && error_message != nullptr) {
        *error_message = result.error_message.empty() ? "Modbus FC10 写入失败" : result.error_message;
    }
    return result.success ? StatusCode::kOk : result.transport_status;
}

// 读取 EM100 协议规定的显式保持寄存器范围。
StatusCode BackendService::read_em100_holding_register_range_once(
    const MasterNodeId& master_id,
    const DeviceId& device_id,
    std::uint16_t start_register,
    std::uint16_t register_count,
    ModbusReadHoldingRegistersResponse* response,
    std::string* error_message)
{
    // 这是 EM100 记录消费命令的底层 FC03 操作，不是通用设备类型即时读取入口。
    // 通用设备类型读取由 read_blocks 决定请求集合。
    // 本操作只返回本次寄存器结果并记录通讯报文，不写实时缓存、RegisterMapper 或历史数据。
    if (response == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 Modbus 读取响应输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    *response = ModbusReadHoldingRegistersResponse{};
    response->master_id = master_id;
    response->start_register = start_register;
    response->register_count = register_count;
    response->status = status_code_name(StatusCode::kOk);
    response->diagnosis_error_code = to_string(DiagnosisErrorCode::kNone);
    response->timestamp_ms = time_utils::system_now_ms();

    auto fail = [&](StatusCode status, DiagnosisErrorCode diagnosis_error_code, const std::string& message) {
        set_read_error_response(response, status, diagnosis_error_code, message);
        if (error_message != nullptr) {
            *error_message = message;
        }
        return status;
    };

    if (master_id.empty()) {
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid, "master_id 不能为空");
    }
    if (device_id.empty()) {
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid, "device_id 不能为空");
    }
    if (register_count == 0 || register_count > ModbusRtuProtocol::kMaxReadRegisterCount) {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "单次读取寄存器数量超出范围，允许范围为 1-125");
    }
    const auto end_register =
        static_cast<std::uint32_t>(start_register) + static_cast<std::uint32_t>(register_count) - 1U;
    if (end_register > 0xFFFFU) {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "读取寄存器范围超出 uint16 地址空间");
    }

    std::lock_guard<std::mutex> command_lock(manual_modbus_mutex_);
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    if (!initialized_) {
        return fail(StatusCode::kInvalidState, DiagnosisErrorCode::kConfigInvalid, "后端服务尚未初始化");
    }

    const MasterNodeConfig* master = nullptr;
    std::size_t master_index = 0;
    std::string lookup_error;
    const auto master_status = require_master_config_locked(master_id, &master, &master_index, &lookup_error);
    (void)master_index;
    if (!is_ok(master_status) || master == nullptr) {
        return fail(
            master_status,
            DiagnosisErrorCode::kConfigInvalid,
            lookup_error.empty() ? "未找到主控: " + master_id : lookup_error);
    }
    const auto* device = find_device_config(device_id);
    if (device == nullptr) {
        return fail(
            StatusCode::kNotFound,
            DiagnosisErrorCode::kConfigInvalid,
            "未找到设备: " + device_id);
    }
    if (device->master_id != master_id) {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "EM100 记录读取设备与主控不匹配");
    }
    if (master->device_template != "EM100") {
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "当前主控未绑定 EM100 绝缘监测模板");
    }
    const auto read_function_code = ModbusRtuProtocol::kReadHoldingRegistersFunction;
    const std::string read_function_name = "FC03";
    response->function_code = read_function_code;

    const auto* channel_config = find_channel_config(master->channel_id);
    fill_master_response_fields(*master, channel_config, response);
    if (channel_config == nullptr) {
        return fail(StatusCode::kNotFound, DiagnosisErrorCode::kConfigInvalid, "未找到通道: " + master->channel_id);
    }
    if (!channel_config->enabled) {
        return fail(StatusCode::kInvalidState, DiagnosisErrorCode::kConfigInvalid, "通道未启用: " + channel_config->channel_id);
    }
    if (master->protocol != MasterProtocol::kModbusRtu && master->protocol != MasterProtocol::kModbusTcp) {
        return fail(StatusCode::kInvalidArgument, DiagnosisErrorCode::kConfigInvalid, "EM100 记录读取仅支持 Modbus RTU 或 Modbus TCP 主控");
    }

    auto communication_lease =
        channel_manager_.acquire_communication_lease(master->channel_id);

    auto* channel = channel_manager_.get_channel(master->channel_id);
    if (channel == nullptr) {
        return fail(StatusCode::kNotFound, DiagnosisErrorCode::kConfigInvalid, "未找到通道实例: " + master->channel_id);
    }

    if (master->protocol == MasterProtocol::kModbusTcp) {
        const auto started_at_ms = time_utils::system_now_ms();
        ModbusTcpClient tcp_client(channel);
        ModbusReadResult read_result;
        const auto status = tcp_client.read_registers(
            *master,
            read_function_code,
            start_register,
            register_count,
            &read_result);
        fill_read_response(read_result, response);
        append_modbus_trace(
            &communication_trace_store_,
            *master,
            read_function_code,
            start_register,
            register_count,
            read_result.request_frame,
            read_result.response_frame,
            started_at_ms,
            read_result.transport_status,
            read_result.error_message,
            -1,
            device_id);
        refresh_channel_statuses();
        if (!read_result.success && error_message != nullptr) {
            *error_message = read_result.error_message.empty()
                                 ? "Modbus " + read_function_name + " 读取失败"
                                 : read_result.error_message;
        }
        return read_result.success ? StatusCode::kOk : status;
    }

    std::string prepare_error;
    const auto prepare_status = channel_manager_.prepare_rtu_channel_for_collection(master->channel_id, &prepare_error);
    if (!is_ok(prepare_status)) {
        refresh_channel_statuses();
        return fail(
            prepare_status,
            prepare_status == StatusCode::kTimeout ? DiagnosisErrorCode::kModbusTimeout : DiagnosisErrorCode::kChannelOpenFailed,
            prepare_error.empty() ? "RTU 通道准备失败: " + master->channel_id : prepare_error);
    }

    ModbusReadResult read_result;
    read_result.timestamp_ms = time_utils::system_now_ms();
    read_result.request_frame = ModbusRtuProtocol::build_read_registers_request(
        static_cast<std::uint8_t>(master->target_address),
        read_function_code,
        start_register,
        register_count);
    if (read_result.request_frame.empty()) {
        refresh_channel_statuses();
        return fail(
            StatusCode::kInvalidArgument,
            DiagnosisErrorCode::kConfigInvalid,
            "构造 Modbus RTU " + read_function_name + " 请求失败");
    }

    ChannelTraceContext trace_context;
    trace_context.channel_id = master->channel_id;
    trace_context.channel_name = channel_config->channel_name;
    trace_context.master_id = master->master_id;
    trace_context.master_name = master->master_name;
    trace_context.device_id = device_id;
    trace_context.target = channel_target_description(*channel_config);
    const auto timeout_ms = static_cast<int>(
        std::max<std::uint32_t>(
            master->response_timeout_ms > 0 ? master->response_timeout_ms : channel_config->response_timeout_ms,
            1));
    const auto started_at_ms = read_result.timestamp_ms;
    read_result.transport_status = channel->transceive(
        read_result.request_frame,
        timeout_ms,
        &read_result.response_frame,
        trace_context);
    read_result.timestamp_ms = time_utils::system_now_ms();
    if (!is_ok(read_result.transport_status)) {
        const auto channel_status = channel->status();
        read_result.error_message = channel_status.last_error_message.empty() ? "通道响应超时" : channel_status.last_error_message;
        read_result.diagnosis_error_code = read_result.transport_status == StatusCode::kTimeout
                                               ? DiagnosisErrorCode::kModbusTimeout
                                               : classify_channel_error(read_result.error_message);
    } else {
        std::string parse_error;
        const auto parse_status = ModbusRtuProtocol::parse_read_registers_response(
            static_cast<std::uint8_t>(master->target_address),
            read_function_code,
            register_count,
            read_result.response_frame,
            &read_result.registers,
            &parse_error);
        if (!is_ok(parse_status)) {
            read_result.error_message = parse_error;
            read_result.diagnosis_error_code = classify_modbus_error(parse_error);
            read_result.transport_status = parse_status;
        } else {
            read_result.success = true;
            read_result.diagnosis_error_code = DiagnosisErrorCode::kNone;
            read_result.transport_status = StatusCode::kOk;
        }
    }

    fill_read_response(read_result, response);
    append_modbus_trace(
        &communication_trace_store_,
        *master,
        read_function_code,
        start_register,
        register_count,
        read_result.request_frame,
        read_result.response_frame,
        started_at_ms,
        read_result.transport_status,
        read_result.error_message,
        -1,
        device_id);
    refresh_channel_statuses();
    if (!read_result.success && error_message != nullptr) {
        *error_message = read_result.error_message.empty()
                             ? "Modbus " + read_function_name + " 读取失败"
                             : read_result.error_message;
    }
    return read_result.success ? StatusCode::kOk : read_result.transport_status;
}

// 执行设备模板中定义的写命令。
StatusCode BackendService::execute_device_command(
    const DeviceCommandExecuteRequest& request,
    DeviceCommandExecuteResponse* response,
    std::string* error_message)
{
    if (response == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少设备命令执行响应输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    *response = DeviceCommandExecuteResponse{};
    response->device_id = request.device_id;
    response->command_key = request.command_key;

    auto fail = [&](StatusCode status, const std::string& message) {
        response->success = false;
        set_error_response(&response->write_result, status, DiagnosisErrorCode::kConfigInvalid, message);
        if (error_message != nullptr) {
            *error_message = message;
        }
        return status;
    };

    if (request.device_id.empty()) {
        return fail(StatusCode::kInvalidArgument, "device_id 不能为空");
    }
    if (request.command_key.empty()) {
        return fail(StatusCode::kInvalidArgument, "command_key 不能为空");
    }

    // 在配置快照中查找设备、所属主站、设备类型和命令定义。
    DeviceConfig device;
    MasterNodeConfig master;
    DeviceTemplateWriteCommandDefinition command;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            return fail(StatusCode::kInvalidState, "后端服务尚未初始化");
        }

        bool device_found = false;
        for (const auto& item : system_config_.devices) {
            if (item.device_id == request.device_id) {
                device = item;
                device_found = true;
                break;
            }
        }
        if (!device_found) {
            return fail(StatusCode::kNotFound, "未找到设备: " + request.device_id);
        }

        bool master_found = false;
        for (const auto& item : system_config_.master_nodes) {
            if (item.master_id == device.master_id) {
                master = item;
                master_found = true;
                break;
            }
        }
        if (!master_found) {
            return fail(StatusCode::kNotFound, "未找到设备所属主控: " + device.master_id);
        }

    const auto device_template = find_device_template(master.device_template);
        if (device_template == nullptr) {
            return fail(StatusCode::kNotFound, "未找到设备模板: " + master.device_template);
        }
        response->template_id = device_template->template_id;
        if (!device_template->builtin) {
            return fail(StatusCode::kInvalidArgument, "自定义设备类型不支持控制操作");
        }

        bool command_found = false;
        for (const auto& item : device_template->write_commands) {
            if (item.key == request.command_key) {
                command = item;
                command_found = true;
                break;
            }
        }
        if (!command_found) {
            return fail(StatusCode::kNotFound, "设备模板不支持该操作: " + request.command_key);
        }
    }

    response->device_name = device.device_name;
    response->command_name = command.name;
    response->warnings = command.warnings;
    response->success_hint = command.success_hint;

    // 校验命令功能码、寄存器数量和值来源定义。
    if (command.function_code != 16) {
        return fail(StatusCode::kInvalidArgument, "当前设备命令仅支持 FC16 写多个保持寄存器");
    }
    if (command.register_count == 0) {
        return fail(StatusCode::kInvalidArgument, "设备命令寄存器数量不能为 0");
    }
    if (command.register_count > ModbusRtuProtocol::kMaxWriteMultipleRegisterCount) {
        return fail(StatusCode::kInvalidArgument, "设备命令寄存器数量超出 FC16 单次写入上限");
    }
    const bool has_fixed_values = !command.fixed_values.empty();
    const bool has_value_fields = !command.value_fields.empty();
    if (has_fixed_values && has_value_fields) {
        return fail(StatusCode::kInvalidArgument, "设备命令不能同时配置固定值和输入字段");
    }
    if (has_fixed_values && command.fixed_values.size() != command.register_count) {
        return fail(StatusCode::kInvalidArgument, "设备命令固定值数量与寄存器数量不一致");
    }
    if (has_value_fields && command.value_fields.size() != command.register_count) {
        return fail(StatusCode::kInvalidArgument, "设备命令字段数量与寄存器数量不一致");
    }
    if (!has_fixed_values && !has_value_fields) {
        return fail(StatusCode::kInvalidArgument, "设备命令缺少固定值或输入字段");
    }

    // 计算设备实际写入地址并检查 uint16 地址边界。
    ModbusWriteMultipleRegistersRequest write_request;
    write_request.master_id = master.master_id;
    const auto start_register = command.has_absolute_register
                                    ? static_cast<std::uint32_t>(command.absolute_register)
                                    : static_cast<std::uint32_t>(master.block_start_register) +
                                          static_cast<std::uint32_t>(device.register_offset) +
                                          static_cast<std::uint32_t>(command.register_offset);
    const auto end_register = start_register + static_cast<std::uint32_t>(command.register_count) - 1U;
    if (end_register > 0xFFFFU) {
        return fail(StatusCode::kInvalidArgument, "设备命令写入寄存器范围超出 uint16 地址空间");
    }
    write_request.start_register = static_cast<std::uint16_t>(start_register);
    write_request.values.reserve(command.register_count);

    // 将固定值或用户输入字段转换为寄存器值。
    if (has_fixed_values) {
        write_request.values = command.fixed_values;
    }

    for (const auto& field : command.value_fields) {
        const auto value_iterator = request.values.find(field.key);
        if (value_iterator == request.values.end()) {
            return fail(StatusCode::kInvalidArgument, "缺少命令参数: " + field.key);
        }
        const auto value = value_iterator->second;
        if (field.type == "uint16") {
            if (value < field.min || value > field.max) {
                return fail(
                    StatusCode::kInvalidArgument,
                    "命令参数超出范围: " + field.key);
            }
        } else if (field.type == "enum") {
            bool matched = false;
            for (const auto& option : field.options) {
                if (option.value == value) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return fail(StatusCode::kInvalidArgument, "命令参数枚举值非法: " + field.key);
            }
        } else {
            return fail(StatusCode::kInvalidArgument, "不支持的命令参数类型: " + field.type);
        }
        write_request.values.push_back(value);
    }

    // 调用统一的多寄存器写入流程并转换响应。
    auto write_response = ModbusWriteMultipleRegistersResponse{};
    const auto status = write_multiple_holding_registers(write_request, &write_response, error_message);
    response->write_result = std::move(write_response);
    response->success = response->write_result.success;
    if (!response->success && error_message != nullptr && error_message->empty()) {
        *error_message = response->write_result.error_message;
    }
    return status;
}

// 读取 EM100 事件记录。
StatusCode BackendService::read_em100_event_record(
    const DeviceId& device_id,
    EM100RecordReadResponse* response,
    std::string* error_message)
{
    if (response == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 EM100 事件记录响应输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    *response = EM100RecordReadResponse{};
    response->device_id = device_id;
    response->record_type = "event";
    response->title = "事件记录";

    DeviceConfig device;
    MasterNodeConfig master;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) {
                *error_message = "后端服务尚未初始化";
            }
            return StatusCode::kInvalidState;
        }
        const auto* device_config = find_device_config(device_id);
        if (device_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "未找到设备: " + device_id;
            }
            return StatusCode::kNotFound;
        }
        const auto* master_config = find_master_config(device_config->master_id);
        if (master_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "未找到设备所属主控: " + device_config->master_id;
            }
            return StatusCode::kNotFound;
        }
        if (master_config->device_template != "EM100") {
            if (error_message != nullptr) {
                *error_message = "当前设备不是 EM100 绝缘监测模板";
            }
            return StatusCode::kInvalidArgument;
        }
        device = *device_config;
        master = *master_config;
    }

    response->device_name = device.device_name;
    response->template_id = "EM100";
    const auto status = read_em100_holding_register_range_once(
        master.master_id,
        device.device_id,
        0xD400,
        6,
        &response->read_result,
        error_message);
    response->success = response->read_result.success;
    response->raw_registers = response->read_result.values;
    if (response->success) {
        // EM100 记录读取会消耗设备内一条未读记录，因此只在手动接口中解析。
        parse_em100_event_registers(response);
    }
    return status;
}

// 读取 EM100 测试记录。
StatusCode BackendService::read_em100_test_record(
    const DeviceId& device_id,
    EM100RecordReadResponse* response,
    std::string* error_message)
{
    if (response == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少 EM100 测试记录响应输出参数";
        }
        return StatusCode::kInvalidArgument;
    }
    *response = EM100RecordReadResponse{};
    response->device_id = device_id;
    response->record_type = "test";
    response->title = "测试记录";

    DeviceConfig device;
    MasterNodeConfig master;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) {
                *error_message = "后端服务尚未初始化";
            }
            return StatusCode::kInvalidState;
        }
        const auto* device_config = find_device_config(device_id);
        if (device_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "未找到设备: " + device_id;
            }
            return StatusCode::kNotFound;
        }
        const auto* master_config = find_master_config(device_config->master_id);
        if (master_config == nullptr) {
            if (error_message != nullptr) {
                *error_message = "未找到设备所属主控: " + device_config->master_id;
            }
            return StatusCode::kNotFound;
        }
        if (master_config->device_template != "EM100") {
            if (error_message != nullptr) {
                *error_message = "当前设备不是 EM100 绝缘监测模板";
            }
            return StatusCode::kInvalidArgument;
        }
        device = *device_config;
        master = *master_config;
    }

    response->device_name = device.device_name;
    response->template_id = "EM100";
    const auto status = read_em100_holding_register_range_once(
        master.master_id,
        device.device_id,
        0xD410,
        6,
        &response->read_result,
        error_message);
    response->success = response->read_result.success;
    response->raw_registers = response->read_result.values;
    if (response->success) {
        parse_em100_test_registers(response);
    }
    return status;
}

}  // namespace edge_controller
