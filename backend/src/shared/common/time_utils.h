// 提供系统时钟、单调时钟、本地时间文本和毫秒时间戳等统一时间辅助。
// 边界：保持低依赖、无业务状态，供上层单向复用。

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "shared/common/types.h"

namespace edge_controller::time_utils {

// 返回当前 Unix 时间戳，单位为毫秒。
inline TimestampMs system_now_ms()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<TimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

// 返回单调递增的毫秒计时，适合用于间隔和心跳计算。
inline TimestampMs steady_now_ms()
{
    const auto now = std::chrono::steady_clock::now();
    return static_cast<TimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

// 把当前本地时间格式化成便于诊断输出的字符串。
inline std::string local_time_string()
{
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stream;
    stream << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

}  // namespace edge_controller::time_utils
