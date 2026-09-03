// Linux fork 子进程的描述符隔离辅助；仅在父进程预先取得上限，child 中只调用异步信号安全系统调用。
#pragma once

#include <algorithm>
#include <cerrno>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace edge_controller::linux_child_process_internal {

#if defined(__linux__)

// 必须在 fork 前调用；避免 child 在多线程进程快照中进入 libc 资源查询路径。
inline int descriptor_limit_before_fork() noexcept
{
    int descriptor_limit = 1024;
    rlimit limits{};
    if (::getrlimit(RLIMIT_NOFILE, &limits) == 0) {
        constexpr rlim_t kReasonableDescriptorLimit = 1024U * 1024U;
        const auto limit = limits.rlim_cur == RLIM_INFINITY
            ? kReasonableDescriptorLimit
            : std::min(limits.rlim_cur, kReasonableDescriptorLimit);
        descriptor_limit = static_cast<int>(std::max<rlim_t>(limit, 4U));
    }
    return descriptor_limit;
}

// 原子创建 CLOEXEC 管道，避免多线程进程中另一个并发 fork/exec 继承短命管道端点。
inline int create_cloexec_pipe(int pipe_fds[2]) noexcept
{
    if (pipe_fds == nullptr) {
        errno = EINVAL;
        return -1;
    }
#if defined(SYS_pipe2)
    if (::syscall(SYS_pipe2, pipe_fds, O_CLOEXEC) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    if (::pipe(pipe_fds) != 0) return -1;
    if (::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
        ::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) == 0) {
        return 0;
    }
    const int pipe_error = errno;
    (void)::close(pipe_fds[0]);
    (void)::close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    errno = pipe_error;
    return -1;
}

// fork 后、exec/受控工作前调用；保留标准输入输出及一个显式管道，其余 controller FD 全部关闭。
inline void close_inherited_descriptors(
    int preserved_fd,
    int descriptor_limit) noexcept
{
#if defined(SYS_close_range)
    bool close_range_supported = true;
    if (preserved_fd > STDERR_FILENO) {
        if (preserved_fd > STDERR_FILENO + 1 &&
            ::syscall(
                SYS_close_range,
                static_cast<unsigned int>(STDERR_FILENO + 1),
                static_cast<unsigned int>(preserved_fd - 1),
                0) != 0) {
            close_range_supported = false;
        }
        if (close_range_supported &&
            ::syscall(
                SYS_close_range,
                static_cast<unsigned int>(preserved_fd + 1),
                ~0U,
                0) != 0) {
            close_range_supported = false;
        }
    } else if (::syscall(
                   SYS_close_range,
                   static_cast<unsigned int>(STDERR_FILENO + 1),
                   ~0U,
                   0) != 0) {
        close_range_supported = false;
    }
    if (close_range_supported) return;
#endif

    descriptor_limit = std::max(descriptor_limit, preserved_fd + 1);
    for (int descriptor = STDERR_FILENO + 1; descriptor < descriptor_limit; ++descriptor) {
        if (descriptor != preserved_fd) {
            (void)::close(descriptor);
        }
    }
}

#endif

}  // namespace edge_controller::linux_child_process_internal
