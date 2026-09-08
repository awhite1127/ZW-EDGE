// 诊断状态模型：把通信、映射和运行错误收敛为稳定错误码、中文消息与处理建议。
#include "data/model/diagnosis_status.h"

#include <array>

namespace edge_controller {

namespace {

constexpr std::array<DiagnosisDefinition, 29> kDefinitions{{
    {DiagnosisErrorCode::kNone,
     "NONE",
     "无错误",
     ""},
    {DiagnosisErrorCode::kPollingNotRunning,
     "POLLING_NOT_RUNNING",
     "轮询未运行",
     "请确认采集目标已启用，并在系统概览或配置保存后启动轮询"},
    {DiagnosisErrorCode::kConfigInvalid,
     "CONFIG_INVALID",
     "配置无效",
     "请检查通道、主控、设备模板和寄存器数量配置是否完整一致"},
    {DiagnosisErrorCode::kChannelOpenFailed,
     "CHANNEL_OPEN_FAILED",
     "通道打开失败",
     "请检查通道目标是否存在、权限是否正确，或是否被其他程序占用"},
    {DiagnosisErrorCode::kChannelConfigFailed,
     "CHANNEL_CONFIG_FAILED",
     "通道参数配置失败",
     "请检查 RTU 串口参数或 TCP 目标参数是否完整且被当前系统支持"},
    {DiagnosisErrorCode::kChannelIoError,
     "CHANNEL_IO_ERROR",
     "通道读写失败",
     "请检查通信链路、系统日志和现场干扰情况"},
    {DiagnosisErrorCode::kModbusTimeout,
     "MODBUS_TIMEOUT",
     "主控通信超时，未收到从站响应",
     "请检查设备电源、通信链路、Unit ID / 从站地址和协议参数配置"},
    {DiagnosisErrorCode::kModbusCrcError,
     "MODBUS_CRC_ERROR",
     "Modbus CRC 校验失败",
     "请检查总线干扰、屏蔽接地、终端电阻、波特率和校验位配置"},
    {DiagnosisErrorCode::kModbusAddressMismatch,
     "MODBUS_ADDRESS_MISMATCH",
     "Modbus 响应地址不匹配",
     "请检查 Unit ID / 从站地址配置，确认总线上没有地址冲突设备"},
    {DiagnosisErrorCode::kModbusFunctionMismatch,
     "MODBUS_FUNCTION_MISMATCH",
     "Modbus 功能码不匹配",
     "请确认设备支持设备类型配置的 FC03/FC04 读取方式，并检查是否接入了错误设备"},
    {DiagnosisErrorCode::kModbusExceptionResponse,
     "MODBUS_EXCEPTION_RESPONSE",
     "Modbus 异常响应",
     "请根据设备手册核对异常码，并检查寄存器地址、数量和从站功能支持"},
    {DiagnosisErrorCode::kModbusLengthInvalid,
     "MODBUS_LENGTH_INVALID",
     "Modbus 响应长度异常",
     "请检查主控寄存器起始地址、寄存器数量和设备协议版本"},
    {DiagnosisErrorCode::kTcpConnectFailed,
     "TCP_CONNECT_FAILED",
     "TCP 连接失败",
     "请检查远端 IP / 主机名、端口、防火墙和设备 TCP 服务状态"},
    {DiagnosisErrorCode::kTcpConnectTimeout,
     "TCP_CONNECT_TIMEOUT",
     "TCP 连接超时",
     "请检查网络连通性、网关配置、远端设备是否在线以及端口是否开放"},
    {DiagnosisErrorCode::kTcpSendFailed,
     "TCP_SEND_FAILED",
     "TCP 发送失败",
     "请检查网络链路、远端连接状态、防火墙和 Modbus TCP 服务稳定性"},
    {DiagnosisErrorCode::kTcpResponseTimeout,
     "TCP_RESPONSE_TIMEOUT",
     "TCP 响应超时",
     "请检查远端设备负载、Unit ID、寄存器范围和响应超时配置"},
    {DiagnosisErrorCode::kTcpRemoteClosed,
     "TCP_REMOTE_CLOSED",
     "TCP 远端关闭连接",
     "请检查远端 Modbus TCP 服务是否允许当前连接和请求参数"},
    {DiagnosisErrorCode::kMbapTransactionMismatch,
     "MBAP_TRANSACTION_ID_MISMATCH",
     "MBAP Transaction ID 不匹配",
     "请检查是否接入了错误设备，或是否存在响应乱序、连接复用异常"},
    {DiagnosisErrorCode::kMbapProtocolInvalid,
     "MBAP_PROTOCOL_ID_INVALID",
     "MBAP Protocol ID 异常",
     "请确认远端目标是标准 Modbus TCP 服务"},
    {DiagnosisErrorCode::kMbapLengthInvalid,
     "MBAP_LENGTH_INVALID",
     "MBAP Length 异常",
     "请检查远端 Modbus TCP 响应格式和网关协议转换配置"},
    {DiagnosisErrorCode::kModbusTcpUnitMismatch,
     "MODBUS_TCP_UNIT_ID_MISMATCH",
     "Modbus TCP Unit ID 不匹配",
     "请检查主站 Unit ID / 从站地址与远端网关映射配置"},
    {DiagnosisErrorCode::kModbusTcpExceptionResponse,
     "MODBUS_TCP_EXCEPTION_RESPONSE",
     "Modbus TCP 异常响应",
     "请根据设备手册核对异常码，并检查寄存器地址、数量和功能支持"},
    {DiagnosisErrorCode::kModbusTcpResponseLengthInvalid,
     "MODBUS_TCP_RESPONSE_LENGTH_INVALID",
     "Modbus TCP 响应数据长度异常",
     "请检查寄存器数量、设备协议版本和远端响应格式"},
    {DiagnosisErrorCode::kDeviceParseFailed,
     "DEVICE_PARSE_FAILED",
     "设备寄存器解析失败",
     "请检查设备模板、设备寄存器偏移和主控读取寄存器范围"},
    {DiagnosisErrorCode::kDeviceFault,
     "DEVICE_FAULT",
     "设备故障",
     "请检查设备供电、传感器和设备内部故障状态"},
    {DiagnosisErrorCode::kDeviceOpenCircuit,
     "DEVICE_OPEN_CIRCUIT",
     "设备开路",
     "请检查设备接线、端子和传感器回路"},
    {DiagnosisErrorCode::kDataInvalid,
     "DATA_INVALID",
     "数据无效或设备故障",
     "请检查设备接线、传感器状态或现场设备是否开路"},
    {DiagnosisErrorCode::kModbusWriteEchoMismatch,
     "MODBUS_WRITE_ECHO_MISMATCH",
     "Modbus 写入响应回显不匹配",
     "请检查写入起始地址、寄存器数量以及远端设备或网关的响应实现"},
    {DiagnosisErrorCode::kUnknownError,
     "UNKNOWN_ERROR",
     "未分类错误",
     "请查看历史事件详情和后端日志，结合现场设备状态继续排查"},
}};

}  // namespace

// 将枚举值转换为字符串。
const char* to_string(DiagnosisLevel level)
{
    switch (level) {
    case DiagnosisLevel::kSystem:
        return "system";
    case DiagnosisLevel::kChannel:
        return "channel";
    case DiagnosisLevel::kMaster:
        return "master";
    case DiagnosisLevel::kDevice:
        return "device";
    }
    return "system";
}

// 将枚举值转换为字符串。
const char* to_string(DiagnosisRunStatus status)
{
    switch (status) {
    case DiagnosisRunStatus::kNormal:
        return "normal";
    case DiagnosisRunStatus::kWarning:
        return "warning";
    case DiagnosisRunStatus::kError:
        return "error";
    case DiagnosisRunStatus::kOffline:
        return "offline";
    }
    return "error";
}

// 将枚举值转换为字符串。
const char* to_string(DiagnosisErrorCode error_code)
{
    return diagnosis_definition(error_code).code;
}

// 返回诊断错误码对应的稳定定义。
const DiagnosisDefinition& diagnosis_definition(DiagnosisErrorCode error_code)
{
    for (const auto& definition : kDefinitions) {
        if (definition.error_code == error_code) {
            return definition;
        }
    }
    return kDefinitions.back();
}

DiagnosisErrorCode diagnosis_code_from_string(const std::string& code)
{
    for (const auto& definition : kDefinitions)
        if (code == definition.code) return definition.error_code;
    return DiagnosisErrorCode::kUnknownError;
}

// 判断诊断状态是否包含有效问题。
bool diagnosis_has_issue(const DiagnosisStatus& diagnosis)
{
    return !diagnosis.error_code.empty() && diagnosis.error_code != "NONE";
}

// 返回诊断状态的严重程度排序值。
int diagnosis_severity(const DiagnosisStatus& diagnosis)
{
    if (diagnosis.status == "offline" || diagnosis.status == "error") {
        return 3;
    }
    if (diagnosis.status == "warning") {
        return 2;
    }
    return diagnosis_has_issue(diagnosis) ? 1 : 0;
}

// 比较两个诊断状态并判断候选项是否更适合展示。
bool diagnosis_is_better(const DiagnosisStatus& candidate, const DiagnosisStatus& current)
{
    if (!diagnosis_has_issue(candidate)) {
        return false;
    }
    if (!diagnosis_has_issue(current)) {
        return true;
    }

    const auto candidate_severity = diagnosis_severity(candidate);
    const auto current_severity = diagnosis_severity(current);
    if (candidate_severity != current_severity) {
        return candidate_severity > current_severity;
    }
    if (candidate.last_error_time_ms != current.last_error_time_ms) {
        return candidate.last_error_time_ms > current.last_error_time_ms;
    }
    return candidate.consecutive_failures > current.consecutive_failures;
}

// 比较并选择诊断。
void consider_diagnosis(const DiagnosisStatus& candidate, DiagnosisStatus* current)
{
    if (current != nullptr && diagnosis_is_better(candidate, *current)) {
        *current = candidate;
    }
}

// 创建诊断。
DiagnosisStatus make_diagnosis(
    DiagnosisLevel level,
    const std::string& target_id,
    const std::string& target_name,
    DiagnosisRunStatus status,
    DiagnosisErrorCode error_code,
    TimestampMs last_success_time_ms,
    TimestampMs last_error_time_ms,
    std::uint32_t consecutive_failures)
{
    const auto& definition = diagnosis_definition(error_code);
    DiagnosisStatus diagnosis;
    diagnosis.level = to_string(level);
    diagnosis.target_id = target_id;
    diagnosis.target_name = target_name;
    diagnosis.status = to_string(status);
    diagnosis.error_code = definition.code;
    diagnosis.message = definition.message;
    diagnosis.suggestion = definition.suggestion;
    diagnosis.last_success_time_ms = last_success_time_ms;
    diagnosis.last_error_time_ms = last_error_time_ms;
    diagnosis.consecutive_failures = consecutive_failures;
    return diagnosis;
}

// 构造无异常的正常诊断状态。
DiagnosisStatus make_normal_diagnosis(
    DiagnosisLevel level,
    const std::string& target_id,
    const std::string& target_name,
    TimestampMs last_success_time_ms)
{
    return make_diagnosis(
        level,
        target_id,
        target_name,
        DiagnosisRunStatus::kNormal,
        DiagnosisErrorCode::kNone,
        last_success_time_ms,
        0,
        0);
}

}  // namespace edge_controller
