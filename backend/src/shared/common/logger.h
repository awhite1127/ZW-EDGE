// 提供带时间与级别的线程安全日志输出，并支持运行时调整最低日志级别。
// 边界：保持低依赖、无业务状态，供上层单向复用。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "shared/common/enums.h"
#include "shared/common/time_utils.h"

namespace edge_controller {

// 简易控制台日志器。
class Logger {
public:
    // 设置级别。
    static void set_level(LogLevel level)
    {
        min_level_.store(level, std::memory_order_relaxed);
    }

    // 记录调试日志。
    static void debug(const std::string& message)
    {
        log(LogLevel::kDebug, "调试", message);
    }

    // 判断指定日志级别是否已启用。
    static bool is_enabled(LogLevel level)
    {
        return static_cast<int>(level) >= static_cast<int>(min_level_.load(std::memory_order_relaxed));
    }

    // 判断调试日志是否启用。
    static bool debug_enabled()
    {
        return is_enabled(LogLevel::kDebug);
    }

    // 记录信息日志。
    static void info(const std::string& message)
    {
        log(LogLevel::kInfo, "信息", message);
    }

    // 记录警告日志。
    static void warn(const std::string& message)
    {
        log(LogLevel::kWarn, "警告", message);
    }

    // 写入已经由调用方按稳定业务键完成限频的警告，避免再次按动态正文判重。
    static void warn_rate_limited(const std::string& message)
    {
        log(LogLevel::kWarn, "警告", message, false);
    }

    // 记录错误日志。
    static void error(const std::string& message)
    {
        log(LogLevel::kError, "错误", message);
    }

private:
    // 按级别写入日志；内部加锁避免多线程输出交错。
    static void log(
        LogLevel level,
        const char* level_name,
        const std::string& message,
        bool suppress_repeated = true)
    {
        if (!is_enabled(level)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (suppress_repeated &&
            (level == LogLevel::kError || level == LogLevel::kWarn) &&
            should_suppress_repeated_locked(message)) {
            return;
        }

        auto& output = (level == LogLevel::kError || level == LogLevel::kWarn) ? std::cerr : std::cout;
        output << "[" << time_utils::local_time_string() << "] "
               << "[" << level_name << "] "
               << message << std::endl;
    }

    struct RepeatedLogState {
        std::chrono::steady_clock::time_point last_emit{};
        std::uint64_t suppressed_count{0};
    };

    // 在持锁状态下判断是否应抑制短时间内的重复日志。
    static bool should_suppress_repeated_locked(const std::string& message)
    {
        constexpr auto kInterval = std::chrono::minutes(15);
        const auto now = std::chrono::steady_clock::now();
        // 在持锁状态下清理过期的重复日志记录。
        prune_repeated_logs_locked(now, message);
        auto& state = repeated_logs_[message];
        if (state.last_emit.time_since_epoch().count() == 0) {
            state.last_emit = now;
            return false;
        }
        if (now - state.last_emit < kInterval) {
            ++state.suppressed_count;
            return true;
        }
        if (state.suppressed_count > 0) {
            auto& output = std::cerr;
            output << "[" << time_utils::local_time_string() << "] "
                   << "[警告] "
                   << "重复错误仍在发生，过去 15 分钟内已抑制 "
                   << state.suppressed_count
                   << " 次："
                   << message
                   << std::endl;
            state.suppressed_count = 0;
            state.last_emit = now;
            return true;
        }
        state.last_emit = now;
        return false;
    }

    // 在持锁状态下清理过期的重复日志记录。
    static void prune_repeated_logs_locked(
        std::chrono::steady_clock::time_point now,
        const std::string& incoming_message)
    {
        constexpr std::size_t kMaxRepeatedLogEntries = 1024;
        constexpr auto kEntryTtl = std::chrono::hours(2);
        if (repeated_logs_.size() < kMaxRepeatedLogEntries ||
            repeated_logs_.find(incoming_message) != repeated_logs_.end()) {
            return;
        }

        for (auto iterator = repeated_logs_.begin(); iterator != repeated_logs_.end();) {
            if (now - iterator->second.last_emit >= kEntryTtl) {
                iterator = repeated_logs_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        if (repeated_logs_.size() >= kMaxRepeatedLogEntries) {
            repeated_logs_.clear();
            std::cerr << "[" << time_utils::local_time_string() << "] "
                      << "[警告] "
                      << "重复日志抑制表已达到上限，已清理旧状态以保护长期运行内存"
                      << std::endl;
        }
    }

    inline static std::atomic<LogLevel> min_level_{LogLevel::kInfo};
    inline static std::mutex mutex_{};
    inline static std::unordered_map<std::string, RepeatedLogState> repeated_logs_{};
};

}  // namespace edge_controller
