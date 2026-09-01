// 定义存储、协议、服务和 IPC 之间统一传递的状态码及判定辅助。
// 边界：保持低依赖、无业务状态，供上层单向复用。

#pragma once

namespace edge_controller {

// 简单后端流程里统一使用的返回码。
enum class StatusCode {
    kOk = 0,
    kInvalidArgument = 1,
    kInvalidState = 2,
    kNotFound = 3,
    kTimeout = 4,
    kIoError = 5,
    kProtocolError = 6,
    kInternalError = 7,
    kConflict = 8,
};

// 将枚举值转换为字符串。
inline const char* to_string(StatusCode code)
{
    switch (code) {
    case StatusCode::kOk:
        return "ok";
    case StatusCode::kInvalidArgument:
        return "invalid_argument";
    case StatusCode::kInvalidState:
        return "invalid_state";
    case StatusCode::kNotFound:
        return "not_found";
    case StatusCode::kTimeout:
        return "timeout";
    case StatusCode::kIoError:
        return "io_error";
    case StatusCode::kProtocolError:
        return "protocol_error";
    case StatusCode::kInternalError:
        return "internal_error";
    case StatusCode::kConflict:
        return "conflict";
    default:
        return "unknown";
    }
}

// 判断状态码是否表示成功。
inline bool is_ok(StatusCode code)
{
    return code == StatusCode::kOk;
}

}  // namespace edge_controller
