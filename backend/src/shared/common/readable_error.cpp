// 将异常和底层错误统一提取为非空文本，供日志、IPC 和诊断事件使用。
#include "shared/common/readable_error.h"

#include "data/model/diagnosis_status.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
namespace edge_controller {
// 仅兼容无诊断码的历史/第三方文本展示，不参与采集和控制的错误判定。
namespace {
// 将ASCII字符串转换为小写。
std::string ascii_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

// 判断文本是否包含任一指定片段。
bool contains_any(const std::string& text, const std::initializer_list<const char*> patterns)
{
    for (const auto* pattern : patterns) {
        if (pattern != nullptr && text.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 分类通道错误信息。
DiagnosisErrorCode classify_channel_error(const std::string& raw_message)
{
    if (raw_message.empty()) {
        return DiagnosisErrorCode::kNone;
    }

    const auto lower = ascii_lower(raw_message);
    if (contains_any(raw_message, {"TCP 连接超时"})) {
        return DiagnosisErrorCode::kTcpConnectTimeout;
    }
    if (contains_any(raw_message, {"TCP 连接失败", "创建 TCP socket 失败", "解析 TCP 目标失败", "TCP host 未配置", "TCP port 非法"})) {
        return DiagnosisErrorCode::kTcpConnectFailed;
    }
    if (contains_any(raw_message, {"TCP 发送失败", "TCP 发送等待超时", "TCP 发送返回 0 字节"})) {
        return DiagnosisErrorCode::kTcpSendFailed;
    }
    if (contains_any(raw_message, {"TCP 响应超时"})) {
        return DiagnosisErrorCode::kTcpResponseTimeout;
    }
    if (contains_any(raw_message, {"TCP 远端关闭连接"})) {
        return DiagnosisErrorCode::kTcpRemoteClosed;
    }
    if (contains_any(lower, {"permission denied", "access denied", "no such file or directory", "device or resource busy", "device busy", "resource busy", "port busy"}) ||
        contains_any(raw_message, {"通道打开失败", "打开串口设备失败", "未找到通道", "权限不足", "访问被拒绝", "不存在", "被占用", "串口通道仅支持 Linux"})) {
        return DiagnosisErrorCode::kChannelOpenFailed;
    }

    if (contains_any(raw_message, {"通道参数配置失败", "读取串口参数失败", "应用串口参数失败", "不支持的波特率", "不支持的数据位"})) {
        return DiagnosisErrorCode::kChannelConfigFailed;
    }

    if (contains_any(lower, {"input/output error", "i/o error"}) ||
        contains_any(raw_message, {"通道读写失败", "串口写入失败", "串口读取失败", "等待串口响应失败", "等待串口输出完成失败", "串口写入返回 0 字节"})) {
        return DiagnosisErrorCode::kChannelIoError;
    }

    if (contains_any(lower, {"timeout", "timed out"}) || contains_any(raw_message, {"超时", "无响应"})) {
        return DiagnosisErrorCode::kModbusTimeout;
    }

    return DiagnosisErrorCode::kUnknownError;
}

// 分类Modbus错误信息。
DiagnosisErrorCode classify_modbus_error(const std::string& raw_message)
{
    if (raw_message.empty()) {
        return DiagnosisErrorCode::kNone;
    }

    const auto lower = ascii_lower(raw_message);
    if (contains_any(lower, {"timeout", "timed out"}) || contains_any(raw_message, {"超时", "无响应"})) {
        return DiagnosisErrorCode::kModbusTimeout;
    }
    if (contains_any(lower, {"crc"}) || contains_any(raw_message, {"CRC", "校验失败"})) {
        return DiagnosisErrorCode::kModbusCrcError;
    }
    // 先匹配 MBAP/TCP 专属错误，避免被后面的通用“长度异常”等片段吞掉。
    if (contains_any(raw_message, {"Transaction ID 不匹配"})) {
        return DiagnosisErrorCode::kMbapTransactionMismatch;
    }
    if (contains_any(raw_message, {"Protocol ID 异常"})) {
        return DiagnosisErrorCode::kMbapProtocolInvalid;
    }
    if (contains_any(raw_message, {"MBAP Length 异常", "MBAP 长度异常"})) {
        return DiagnosisErrorCode::kMbapLengthInvalid;
    }
    if (contains_any(raw_message, {"Unit ID 不匹配"})) {
        return DiagnosisErrorCode::kModbusTcpUnitMismatch;
    }
    if (contains_any(raw_message, {
            "Byte Count 异常",
            "Modbus TCP 响应长度不足",
            "Modbus TCP 响应数据长度异常",
            "Modbus TCP 异常响应长度不匹配"})) {
        return DiagnosisErrorCode::kModbusTcpResponseLengthInvalid;
    }
    if (contains_any(raw_message, {"Modbus TCP 异常码", "Modbus TCP 异常响应"})) {
        return DiagnosisErrorCode::kModbusTcpExceptionResponse;
    }
    if (contains_any(raw_message, {"起始寄存器地址不匹配", "写入寄存器数量不匹配"})) {
        return DiagnosisErrorCode::kModbusWriteEchoMismatch;
    }
    if (contains_any(raw_message, {"从站地址不匹配"})) {
        return DiagnosisErrorCode::kModbusAddressMismatch;
    }
    if (contains_any(raw_message, {"功能码不匹配"})) {
        return DiagnosisErrorCode::kModbusFunctionMismatch;
    }
    if (contains_any(raw_message, {"Modbus 异常响应长度不匹配"})) {
        return DiagnosisErrorCode::kModbusLengthInvalid;
    }
    if (contains_any(raw_message, {"Modbus 异常码", "Modbus 异常响应", "设备返回 Modbus 异常"})) {
        return DiagnosisErrorCode::kModbusExceptionResponse;
    }
    if (contains_any(raw_message, {"响应帧长度", "字节数不匹配", "长度不匹配", "长度异常"})) {
        return DiagnosisErrorCode::kModbusLengthInvalid;
    }
    return DiagnosisErrorCode::kUnknownError;
}

// 分类运行状态错误信息。
DiagnosisErrorCode classify_runtime_error(const std::string& raw_message)
{
    const auto modbus_code = classify_modbus_error(raw_message);
    if (modbus_code != DiagnosisErrorCode::kNone && modbus_code != DiagnosisErrorCode::kUnknownError) {
        return modbus_code;
    }

    const auto channel_code = classify_channel_error(raw_message);
    if (channel_code != DiagnosisErrorCode::kNone && channel_code != DiagnosisErrorCode::kUnknownError) {
        return channel_code;
    }

    if (contains_any(raw_message, {"配置", "寄存器数量超出范围", "模板"})) {
        return DiagnosisErrorCode::kConfigInvalid;
    }
    if (contains_any(raw_message, {"映射失败", "解析失败", "寄存器切片越界"})) {
        return DiagnosisErrorCode::kDeviceParseFailed;
    }
    if (contains_any(raw_message, {"设备开路", "回路开路"})) {
        return DiagnosisErrorCode::kDeviceOpenCircuit;
    }
    if (contains_any(raw_message, {"设备故障"})) {
        return DiagnosisErrorCode::kDeviceFault;
    }
    if (contains_any(raw_message, {"数据无效", "设备通讯中断", "测温点故障"})) {
        return DiagnosisErrorCode::kDataInvalid;
    }
    if (contains_any(raw_message, {"轮询未启动", "轮询服务尚未初始化", "无有效启用采集目标"})) {
        return DiagnosisErrorCode::kPollingNotRunning;
    }
    return raw_message.empty() ? DiagnosisErrorCode::kNone : DiagnosisErrorCode::kUnknownError;
}

}  // namespace


// 将底层错误整理为可展示文本和诊断详情。
ReadableErrorMessage build_readable_runtime_error(const std::string& raw_message)
{
    ReadableErrorMessage readable;
    readable.detail = raw_message;

    const auto error_code = classify_runtime_error(raw_message);
    const auto& definition = diagnosis_definition(error_code);
    readable.summary = definition.message;
    if (definition.suggestion[0] != '\0') {
        readable.detail = raw_message.empty()
                              ? definition.suggestion
                              : raw_message + "；" + definition.suggestion;
    }
    return readable;
}

}  // namespace edge_controller
