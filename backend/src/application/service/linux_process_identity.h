// Linux 进程身份辅助：用 PID + /proc starttime 抵御 PID 复用，并优先通过 pidfd 发信号。
#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace edge_controller::process_identity_internal {

struct ProcessIdentity {
    std::int64_t pid{-1};
    std::uint64_t start_time_ticks{0};
};

enum class SignalResult {
    kSignaled,
    kAlreadyExited,
    kIdentityChanged,
    kPermissionDenied,
    kError,
};

// /proc/<pid>/stat 的第 22 字段为进程启动 tick；comm 允许包含空格和括号，
// 因此必须从最后一个右括号后再按字段号解析。
inline std::optional<std::uint64_t> parse_linux_process_start_time(const std::string& stat)
{
    const auto command_end = stat.rfind(')');
    if (command_end == std::string::npos || command_end + 1 >= stat.size()) {
        return std::nullopt;
    }
    std::istringstream stream(stat.substr(command_end + 1));
    std::string field_value;
    for (int field_number = 3; field_number <= 22; ++field_number) {
        if (!(stream >> field_value)) {
            return std::nullopt;
        }
        if (field_number == 22) {
            try {
                std::size_t parsed = 0;
                const auto value = std::stoull(field_value, &parsed, 10);
                if (parsed != field_value.size()) return std::nullopt;
                return static_cast<std::uint64_t>(value);
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

#if defined(__linux__)

inline std::optional<std::uint64_t> read_linux_process_start_time(pid_t pid)
{
    if (pid <= 1) return std::nullopt;
    std::ifstream stream("/proc/" + std::to_string(pid) + "/stat");
    if (!stream) return std::nullopt;
    std::string stat;
    std::getline(stream, stat);
    return parse_linux_process_start_time(stat);
}

inline std::optional<ProcessIdentity> capture_process_identity(pid_t pid)
{
    const auto start_time = read_linux_process_start_time(pid);
    if (!start_time.has_value()) return std::nullopt;
    return ProcessIdentity{static_cast<std::int64_t>(pid), *start_time};
}

inline bool same_process_identity(const ProcessIdentity& identity)
{
    // starttime 在系统启动后的第一个时钟 tick 内可以合法为 0，不能把它当作无效哨兵。
    if (identity.pid <= 1) return false;
    const auto current = read_linux_process_start_time(static_cast<pid_t>(identity.pid));
    return current.has_value() && *current == identity.start_time_ticks;
}

inline SignalResult signal_same_process(const ProcessIdentity& identity, int signal_number)
{
    if (!same_process_identity(identity)) return SignalResult::kIdentityChanged;
    const auto pid = static_cast<pid_t>(identity.pid);

#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    errno = 0;
    const int pid_fd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (pid_fd >= 0) {
        // pidfd 固定指向打开时的进程对象；再次校验后即使 PID 随后复用也不会误杀。
        if (!same_process_identity(identity)) {
            ::close(pid_fd);
            return SignalResult::kIdentityChanged;
        }
        const int send_result = static_cast<int>(
            ::syscall(SYS_pidfd_send_signal, pid_fd, signal_number, nullptr, 0));
        const int send_error = errno;
        ::close(pid_fd);
        if (send_result == 0) return SignalResult::kSignaled;
        if (send_error == ESRCH) return SignalResult::kAlreadyExited;
        if (send_error == EPERM) return SignalResult::kPermissionDenied;
        return SignalResult::kError;
    }
    if (errno != ENOSYS && errno != EINVAL && errno != EPERM) {
        return errno == ESRCH ? SignalResult::kAlreadyExited : SignalResult::kError;
    }
#endif

    // 旧内核回退：信号前再次校验 starttime，把 PID 复用窗口压缩到单次系统调用之间。
    if (!same_process_identity(identity)) return SignalResult::kIdentityChanged;
    if (::kill(pid, signal_number) == 0) return SignalResult::kSignaled;
    if (errno == ESRCH) return SignalResult::kAlreadyExited;
    if (errno == EPERM) return SignalResult::kPermissionDenied;
    return SignalResult::kError;
}

#endif

}  // namespace edge_controller::process_identity_internal
