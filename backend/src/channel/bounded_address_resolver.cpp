#include "channel/bounded_address_resolver.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "common/time_utils.h"
#include "platform/linux_child_process.h"

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace edge_controller::channel_internal {

#if defined(__linux__)
namespace {

constexpr std::uint32_t kWireMagic = 0x45435253U;
constexpr std::uint32_t kMaxAddressCount = 64U;
constexpr std::uint32_t kMaxAddressBytes = 256U;
constexpr std::uint32_t kMaxErrorBytes = 1024U;
constexpr std::size_t kMaxPayloadBytes = 64U * 1024U;
constexpr TimestampMs kCancellationPollMs = 20U;

struct WireHeader {
    std::uint32_t magic{kWireMagic};
    std::int32_t resolver_status{0};
    std::uint32_t address_count{0};
    std::uint32_t error_size{0};
};

struct WireAddressHeader {
    std::int32_t family{0};
    std::int32_t socket_type{0};
    std::int32_t protocol{0};
    std::uint32_t address_size{0};
};

bool write_all(int fd, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < size) {
        const auto write_size = ::write(fd, bytes + written, size - written);
        if (write_size > 0) {
            written += static_cast<std::size_t>(write_size);
            continue;
        }
        if (write_size < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

[[noreturn]] void run_resolver_child(
    int output_fd,
    const std::string& host,
    const std::string& service,
    AddressResolverFunction resolver)
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* addresses = nullptr;
    const auto resolver_function = resolver == nullptr ? &::getaddrinfo : resolver;
    const int resolver_status =
        resolver_function(host.c_str(), service.c_str(), &hints, &addresses);

    WireHeader header;
    header.resolver_status = resolver_status;
    if (resolver_status != 0) {
        const char* resolver_error = ::gai_strerror(resolver_status);
        const auto error_size = resolver_error == nullptr
            ? 0U
            : static_cast<std::uint32_t>(std::min<std::size_t>(
                  std::strlen(resolver_error), kMaxErrorBytes));
        header.error_size = error_size;
        const bool write_ok = write_all(output_fd, &header, sizeof(header)) &&
            (error_size == 0 || write_all(output_fd, resolver_error, error_size));
        if (addresses != nullptr) {
            ::freeaddrinfo(addresses);
        }
        ::close(output_fd);
        _exit(write_ok ? 0 : 125);
    }

    for (auto* address = addresses; address != nullptr && header.address_count < kMaxAddressCount;
         address = address->ai_next) {
        if (address->ai_addr != nullptr && address->ai_addrlen > 0 &&
            address->ai_addrlen <= kMaxAddressBytes) {
            ++header.address_count;
        }
    }

    bool write_ok = write_all(output_fd, &header, sizeof(header));
    std::uint32_t written_address_count = 0;
    for (auto* address = addresses; write_ok && address != nullptr;
         address = address->ai_next) {
        if (address->ai_addr == nullptr || address->ai_addrlen == 0 ||
            address->ai_addrlen > kMaxAddressBytes) {
            continue;
        }
        WireAddressHeader address_header;
        address_header.family = address->ai_family;
        address_header.socket_type = address->ai_socktype;
        address_header.protocol = address->ai_protocol;
        address_header.address_size = static_cast<std::uint32_t>(address->ai_addrlen);
        write_ok = write_all(output_fd, &address_header, sizeof(address_header)) &&
            write_all(output_fd, address->ai_addr, address_header.address_size);
        ++written_address_count;
        if (written_address_count >= header.address_count) {
            break;
        }
    }
    if (addresses != nullptr) {
        ::freeaddrinfo(addresses);
    }
    ::close(output_fd);
    _exit(write_ok ? 0 : 125);
}

int spawn_resolver_helper(
    const std::string& host,
    const std::string& service,
    const int pipe_fds[2],
    pid_t* child_pid)
{
    if (pipe_fds == nullptr || child_pid == nullptr) {
        return EINVAL;
    }

    posix_spawn_file_actions_t actions{};
    int status = ::posix_spawn_file_actions_init(&actions);
    if (status != 0) {
        return status;
    }

    // file actions 按顺序执行。先关闭父端读描述符，再把写端固定到 helper 协议描述符；
    // 其它 controller 描述符依赖 CLOEXEC，并在 exec 后由 helper 再做一次防御性清理。
    status = ::posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    if (status == 0) {
        status = ::posix_spawn_file_actions_adddup2(
            &actions, pipe_fds[1], kAddressResolverHelperOutputFd);
    }
    if (status == 0 && pipe_fds[1] != kAddressResolverHelperOutputFd) {
        status = ::posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }

    posix_spawnattr_t attributes{};
    bool attributes_initialized = false;
    if (status == 0) {
        status = ::posix_spawnattr_init(&attributes);
        attributes_initialized = status == 0;
    }
    if (status == 0) {
        status = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (status == 0) {
        status = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    }

    if (status == 0) {
        const auto* configured_helper = std::getenv("EDGE_CONTROLLER_RESOLVER_HELPER");
        const std::string executable = configured_helper != nullptr && configured_helper[0] != '\0'
            ? configured_helper
            : "/proc/self/exe";
        std::string helper_argument = kAddressResolverHelperArgument;
        char* arguments[] = {
            const_cast<char*>(executable.c_str()),
            helper_argument.data(),
            const_cast<char*>(host.c_str()),
            const_cast<char*>(service.c_str()),
            nullptr,
        };
        status = ::posix_spawn(
            child_pid,
            executable.c_str(),
            &actions,
            &attributes,
            arguments,
            environ);
    }

    if (attributes_initialized) {
        ::posix_spawnattr_destroy(&attributes);
    }
    ::posix_spawn_file_actions_destroy(&actions);
    return status;
}

class ResolverChild final {
public:
    ResolverChild(pid_t pid, int output_fd)
        : pid_(pid), output_fd_(output_fd)
    {
    }

    ~ResolverChild()
    {
        close_output();
        terminate_and_reap();
    }

    ResolverChild(const ResolverChild&) = delete;
    ResolverChild& operator=(const ResolverChild&) = delete;

    int output_fd() const noexcept { return output_fd_; }

    void close_output() noexcept
    {
        if (output_fd_ >= 0) {
            ::close(output_fd_);
            output_fd_ = -1;
        }
    }

    bool reap_if_exited(int* child_status)
    {
        if (pid_ <= 0) {
            return true;
        }
        while (true) {
            int status = 0;
            const auto waited = ::waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                pid_ = -1;
                if (child_status != nullptr) {
                    *child_status = status;
                }
                return true;
            }
            if (waited == 0) {
                return false;
            }
            if (errno == EINTR) {
                continue;
            }
            pid_ = -1;
            return true;
        }
    }

    void terminate_and_reap() noexcept
    {
        if (pid_ <= 0) {
            return;
        }
        if (::kill(-pid_, SIGKILL) != 0 && errno == ESRCH) {
            ::kill(pid_, SIGKILL);
        }
        int ignored_status = 0;
        while (::waitpid(pid_, &ignored_status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
    int output_fd_{-1};
};

bool cancellation_requested(
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* stop_requested)
{
    return (cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) ||
        (stop_requested != nullptr && stop_requested->load(std::memory_order_acquire));
}

template <typename Value>
bool consume_value(const std::vector<std::uint8_t>& payload, std::size_t* offset, Value* value)
{
    if (offset == nullptr || value == nullptr || *offset > payload.size() ||
        payload.size() - *offset < sizeof(Value)) {
        return false;
    }
    std::memcpy(value, payload.data() + *offset, sizeof(Value));
    *offset += sizeof(Value);
    return true;
}

AddressResolutionResult parse_payload(const std::vector<std::uint8_t>& payload)
{
    AddressResolutionResult result;
    std::size_t offset = 0;
    WireHeader header;
    if (!consume_value(payload, &offset, &header) || header.magic != kWireMagic ||
        header.address_count > kMaxAddressCount || header.error_size > kMaxErrorBytes) {
        result.status = StatusCode::kProtocolError;
        result.error_message = "DNS 解析子进程返回了无效数据";
        return result;
    }

    if (header.resolver_status != 0) {
        if (payload.size() - offset < header.error_size) {
            result.status = StatusCode::kProtocolError;
            result.error_message = "DNS 解析子进程错误信息不完整";
            return result;
        }
        result.status = StatusCode::kIoError;
        result.error_message = "解析 TCP 目标失败";
        if (header.error_size > 0) {
            result.error_message += ": " + std::string(
                reinterpret_cast<const char*>(payload.data() + offset), header.error_size);
        }
        return result;
    }

    result.addresses.reserve(header.address_count);
    for (std::uint32_t index = 0; index < header.address_count; ++index) {
        WireAddressHeader address_header;
        if (!consume_value(payload, &offset, &address_header) ||
            address_header.address_size == 0 || address_header.address_size > kMaxAddressBytes ||
            payload.size() - offset < address_header.address_size) {
            result.status = StatusCode::kProtocolError;
            result.error_message = "DNS 解析子进程地址数据不完整";
            result.addresses.clear();
            return result;
        }
        ResolvedSocketAddress address;
        address.family = address_header.family;
        address.socket_type = address_header.socket_type;
        address.protocol = address_header.protocol;
        address.address.assign(
            payload.begin() + static_cast<std::ptrdiff_t>(offset),
            payload.begin() + static_cast<std::ptrdiff_t>(offset + address_header.address_size));
        offset += address_header.address_size;
        result.addresses.push_back(std::move(address));
    }
    if (result.addresses.empty()) {
        result.status = StatusCode::kNotFound;
        result.error_message = "TCP 目标未解析到可用地址";
        return result;
    }
    result.status = StatusCode::kOk;
    return result;
}

// 数字 IP 不需要 DNS/NSS，直接在当前进程构造地址，避免断线重连时反复 fork。
bool try_resolve_numeric_addresses(
    const std::string& host,
    const std::string& service,
    AddressResolutionResult* result)
{
    if (result == nullptr) {
        return false;
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

    addrinfo* addresses = nullptr;
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    AddressResolutionResult numeric_result;
    for (auto* address = addresses;
         address != nullptr && numeric_result.addresses.size() < kMaxAddressCount;
         address = address->ai_next) {
        if (address->ai_addr == nullptr || address->ai_addrlen == 0 ||
            address->ai_addrlen > kMaxAddressBytes) {
            continue;
        }
        ResolvedSocketAddress resolved;
        resolved.family = address->ai_family;
        resolved.socket_type = address->ai_socktype;
        resolved.protocol = address->ai_protocol;
        const auto* begin = reinterpret_cast<const std::uint8_t*>(address->ai_addr);
        resolved.address.assign(begin, begin + address->ai_addrlen);
        numeric_result.addresses.push_back(std::move(resolved));
    }
    ::freeaddrinfo(addresses);

    if (numeric_result.addresses.empty()) {
        numeric_result.status = StatusCode::kNotFound;
        numeric_result.error_message = "TCP 数字目标未生成可用地址";
    } else {
        numeric_result.status = StatusCode::kOk;
    }
    *result = std::move(numeric_result);
    return true;
}

}  // namespace

[[noreturn]] void run_address_resolver_helper(
    const std::string& host,
    const std::string& service,
    int output_fd)
{
    // 此入口只会在 posix_spawn/exec 后执行；此时已是单线程的新进程映像，可以安全
    // 进入 libc/NSS。关闭意外继承的描述符，避免慢 DNS 阻碍主进程资源释放。
    const int descriptor_limit = linux_child_process_internal::descriptor_limit_before_fork();
    linux_child_process_internal::close_inherited_descriptors(output_fd, descriptor_limit);
    run_resolver_child(output_fd, host, service, nullptr);
}
#endif

AddressResolutionResult resolve_addresses_until(
    const std::string& host,
    const std::string& service,
    TimestampMs deadline_ms,
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* stop_requested,
    int wake_fd,
    AddressResolverFunction resolver)
{
#if !defined(__linux__)
    (void)host;
    (void)service;
    (void)deadline_ms;
    (void)cancel_requested;
    (void)stop_requested;
    (void)wake_fd;
    (void)resolver;
    AddressResolutionResult result;
    result.status = StatusCode::kInvalidState;
    result.error_message = "当前平台不支持 POSIX 地址解析";
    return result;
#else
    AddressResolutionResult result;
    if (cancellation_requested(cancel_requested, stop_requested)) {
        result.status = StatusCode::kInvalidState;
        result.error_message = "DNS 解析已取消";
        return result;
    }
    if (time_utils::steady_now_ms() >= deadline_ms) {
        result.status = StatusCode::kTimeout;
        result.error_message = "DNS 解析超时";
        return result;
    }

    // 注入解析器用于可取消性测试，不可被数字 IP 快速路径绕过。
    if (resolver == nullptr && try_resolve_numeric_addresses(host, service, &result)) {
        return result;
    }

    int pipe_fds[2]{};
    if (linux_child_process_internal::create_cloexec_pipe(pipe_fds) != 0) {
        result.status = StatusCode::kIoError;
        result.error_message = "创建 DNS 解析管道失败: " + std::string(std::strerror(errno));
        return result;
    }
    const int read_flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    if (read_flags < 0 || ::fcntl(pipe_fds[0], F_SETFL, read_flags | O_NONBLOCK) != 0) {
        const int pipe_error = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        result.status = StatusCode::kIoError;
        result.error_message = "设置 DNS 解析管道失败: " + std::string(std::strerror(pipe_error));
        return result;
    }

    pid_t child_pid = -1;
    bool process_group_created = false;
    if (resolver == nullptr) {
        // 生产路径必须 exec 到全新的进程映像后再进入 getaddrinfo/NSS。posix_spawn
        // 同时创建独立进程组，保留原有 deadline 到期时整体终止的语义。
        const int spawn_error = spawn_resolver_helper(host, service, pipe_fds, &child_pid);
        if (spawn_error != 0) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            result.status = StatusCode::kIoError;
            result.error_message =
                "启动 DNS 解析 helper 失败: " + std::string(std::strerror(spawn_error));
            return result;
        }
        process_group_created = true;
    } else {
        // 注入函数仅供进程隔离/取消测试使用，不能跨 exec 传递函数指针。正式调用方
        // 始终走上面的 posix_spawn 路径。
        int descriptor_limit = linux_child_process_internal::descriptor_limit_before_fork();
        descriptor_limit = std::max(descriptor_limit, pipe_fds[1] + 1);
        child_pid = ::fork();
        if (child_pid < 0) {
            const int fork_error = errno;
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            result.status = StatusCode::kIoError;
            result.error_message =
                "启动测试 DNS 解析子进程失败: " + std::string(std::strerror(fork_error));
            return result;
        }
        if (child_pid == 0) {
            ::close(pipe_fds[0]);
            linux_child_process_internal::close_inherited_descriptors(
                pipe_fds[1], descriptor_limit);
            if (::setpgid(0, 0) != 0) {
                ::close(pipe_fds[1]);
                _exit(126);
            }
            run_resolver_child(pipe_fds[1], host, service, resolver);
        }
    }

    ::close(pipe_fds[1]);
    if (!process_group_created &&
        ::setpgid(child_pid, child_pid) != 0 && errno != EACCES && errno != ESRCH) {
        const int group_error = errno;
        ResolverChild child(child_pid, pipe_fds[0]);
        child.close_output();
        child.terminate_and_reap();
        result.status = StatusCode::kIoError;
        result.error_message =
            "创建 DNS 解析进程组失败: " + std::string(std::strerror(group_error));
        return result;
    }
    ResolverChild child(child_pid, pipe_fds[0]);
    std::vector<std::uint8_t> payload;
    bool output_eof = false;
    int child_status = 0;
    bool child_reaped = false;

    while (true) {
        if (cancellation_requested(cancel_requested, stop_requested)) {
            child.close_output();
            child.terminate_and_reap();
            result.status = StatusCode::kInvalidState;
            result.error_message = "DNS 解析已取消";
            return result;
        }
        const auto now = time_utils::steady_now_ms();
        if (now >= deadline_ms) {
            child.close_output();
            child.terminate_and_reap();
            result.status = StatusCode::kTimeout;
            result.error_message = "DNS 解析超时";
            return result;
        }

        if (!output_eof) {
            std::uint8_t buffer[4096];
            while (true) {
                const auto read_size = ::read(child.output_fd(), buffer, sizeof(buffer));
                if (read_size > 0) {
                    if (payload.size() + static_cast<std::size_t>(read_size) > kMaxPayloadBytes) {
                        child.close_output();
                        child.terminate_and_reap();
                        result.status = StatusCode::kProtocolError;
                        result.error_message = "DNS 解析子进程输出超过限制";
                        return result;
                    }
                    payload.insert(payload.end(), buffer, buffer + read_size);
                    continue;
                }
                if (read_size == 0) {
                    output_eof = true;
                    child.close_output();
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    child.close_output();
                    child.terminate_and_reap();
                    result.status = StatusCode::kIoError;
                    result.error_message = "读取 DNS 解析结果失败: " + std::string(std::strerror(errno));
                    return result;
                }
                break;
            }
        }

        if (!child_reaped) {
            child_reaped = child.reap_if_exited(&child_status);
        }
        if (output_eof && child_reaped) {
            if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
                result.status = StatusCode::kIoError;
                result.error_message = "DNS 解析子进程异常退出";
                return result;
            }
            return parse_payload(payload);
        }

        const auto remaining = deadline_ms - time_utils::steady_now_ms();
        TimestampMs wait_ms = remaining;
        if (cancel_requested != nullptr || (stop_requested != nullptr && wake_fd < 0)) {
            wait_ms = std::min(wait_ms, kCancellationPollMs);
        }
        if (output_eof) {
            // 管道关闭与子进程 _exit 之间存在极短窗口；不可在此睡到整个 DNS 截止时间。
            wait_ms = std::min<TimestampMs>(wait_ms, 10U);
        }
        wait_ms = std::max<TimestampMs>(wait_ms, 1U);
        const int poll_timeout = static_cast<int>(std::min<TimestampMs>(wait_ms, INT_MAX));

        pollfd descriptors[2]{};
        nfds_t descriptor_count = 0;
        if (!output_eof) {
            descriptors[descriptor_count].fd = child.output_fd();
            descriptors[descriptor_count].events = POLLIN | POLLHUP | POLLERR;
            ++descriptor_count;
        }
        if (wake_fd >= 0) {
            descriptors[descriptor_count].fd = wake_fd;
            descriptors[descriptor_count].events = POLLIN;
            ++descriptor_count;
        }
        int poll_status = 0;
        do {
            poll_status = ::poll(descriptors, descriptor_count, poll_timeout);
        } while (poll_status < 0 && errno == EINTR);
        if (poll_status < 0) {
            child.close_output();
            child.terminate_and_reap();
            result.status = StatusCode::kIoError;
            result.error_message = "等待 DNS 解析结果失败: " + std::string(std::strerror(errno));
            return result;
        }
    }
#endif
}

}  // namespace edge_controller::channel_internal
