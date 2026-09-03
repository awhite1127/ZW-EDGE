// 跨采集、协议和服务层共享的稳定诊断状态模型。
#pragma once

#include <cstdint>
#include <string>

#include "shared/common/types.h"

namespace edge_controller {

enum class DiagnosisLevel {
    kSystem,
    kChannel,
    kMaster,
    kDevice,
};

enum class DiagnosisRunStatus {
    kNormal,
    kWarning,
    kError,
    kOffline,
};

enum class DiagnosisErrorCode {
    kNone,
    kPollingNotRunning,
    kConfigInvalid,
    kChannelOpenFailed,
    kChannelConfigFailed,
    kChannelIoError,
    kModbusTimeout,
    kModbusCrcError,
    kModbusAddressMismatch,
    kModbusFunctionMismatch,
    kModbusExceptionResponse,
    kModbusLengthInvalid,
    kTcpConnectFailed,
    kTcpConnectTimeout,
    kTcpSendFailed,
    kTcpResponseTimeout,
    kTcpRemoteClosed,
    kMbapTransactionMismatch,
    kMbapProtocolInvalid,
    kMbapLengthInvalid,
    kModbusTcpUnitMismatch,
    kModbusTcpExceptionResponse,
    kModbusTcpResponseLengthInvalid,
    kDeviceParseFailed,
    kDeviceFault,
    kDeviceOpenCircuit,
    kDataInvalid,
    kModbusWriteEchoMismatch,
    kUnknownError,
};

struct DiagnosisDefinition {
    DiagnosisErrorCode error_code{DiagnosisErrorCode::kNone};
    const char* code{"NONE"};
    const char* message{"无错误"};
    const char* suggestion{""};
};

struct DiagnosisStatus {
    std::string level;
    std::string target_id;
    std::string target_name;
    std::string status{"normal"};
    std::string error_code{"NONE"};
    std::string message{"无错误"};
    std::string suggestion;
    TimestampMs last_success_time_ms{0};
    TimestampMs last_error_time_ms{0};
    std::uint32_t consecutive_failures{0};
};

// 将枚举值转换为字符串。
const char* to_string(DiagnosisLevel level);
// 将枚举值转换为字符串。
const char* to_string(DiagnosisRunStatus status);
// 将枚举值转换为字符串。
const char* to_string(DiagnosisErrorCode error_code);

// 返回诊断错误码对应的稳定定义。
const DiagnosisDefinition& diagnosis_definition(DiagnosisErrorCode error_code);
// 分类通道错误信息。
DiagnosisErrorCode classify_channel_error(const std::string& raw_message);
// 分类Modbus错误信息。
DiagnosisErrorCode classify_modbus_error(const std::string& raw_message);
// 分类运行状态错误信息。
DiagnosisErrorCode classify_runtime_error(const std::string& raw_message);
// 判断诊断状态是否包含有效问题。
bool diagnosis_has_issue(const DiagnosisStatus& diagnosis);
// 返回诊断状态的严重程度排序值。
int diagnosis_severity(const DiagnosisStatus& diagnosis);
// 比较两个诊断状态并判断候选项是否更适合展示。
bool diagnosis_is_better(const DiagnosisStatus& candidate, const DiagnosisStatus& current);
// 比较并选择诊断。
void consider_diagnosis(const DiagnosisStatus& candidate, DiagnosisStatus* current);

// 创建诊断。
DiagnosisStatus make_diagnosis(
    DiagnosisLevel level,
    const std::string& target_id,
    const std::string& target_name,
    DiagnosisRunStatus status,
    DiagnosisErrorCode error_code,
    TimestampMs last_success_time_ms,
    TimestampMs last_error_time_ms,
    std::uint32_t consecutive_failures);

// 构造无异常的正常诊断状态。
DiagnosisStatus make_normal_diagnosis(
    DiagnosisLevel level,
    const std::string& target_id,
    const std::string& target_name,
    TimestampMs last_success_time_ms);

}  // namespace edge_controller
