// Linux 网络运行时适配：读取链路状态并执行受控 DHCP/static 切换，外部命令均有超时和结果校验。
#include "service/backend_service.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/filesystem_compat.h"

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;
#endif

#include "common/logger.h"
#include "platform/linux_child_process.h"
#include "service/backend_service_internal.h"
#include "service/linux_process_identity.h"
#include "service/network_config_policy.h"
#include "service/network_command_runner.h"

namespace edge_controller {

namespace {

// 解析网络子进程 PID 文件目录；正式 systemd 环境会显式注入该路径。
[[maybe_unused]] std::string controller_run_directory()
{
    const auto* value = std::getenv("EDGE_CONTROLLER_RUN_DIR");
    return value != nullptr && value[0] != '\0' ? value : "/opt/edge-controller/run";
}

// 读取文本文件。
[[maybe_unused]] std::string read_text_file(const edge::fs::path& path)
{
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    std::string value;
    std::getline(stream, value);
    return backend_internal::trim_copy(value);
}

// 读取网口的管理态（IFF_UP），区别于可能因无载波而显示 down 的 operstate。
[[maybe_unused]] std::optional<bool> read_interface_admin_up(const std::string& interface_name)
{
#if !defined(__linux__)
    (void)interface_name;
    return std::nullopt;
#else
    const auto flags_text = read_text_file(
        edge::fs::path("/sys/class/net") / interface_name / "flags");
    if (flags_text.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t parsed = 0;
        const auto flags = std::stoul(flags_text, &parsed, 0);
        if (parsed != flags_text.size()) {
            return std::nullopt;
        }
        return (flags & static_cast<unsigned long>(IFF_UP)) != 0;
    } catch (...) {
        return std::nullopt;
    }
#endif
}

// 将网卡链路状态转换为显示文本。
std::string link_state_text(const std::string& link_state)
{
    if (link_state == "connected") {
        return "已连接";
    }
    if (link_state == "disconnected") {
        return "未连接";
    }
    if (link_state == "not_found") {
        return "网口不存在";
    }
    return "未知";
}

// 将路由表十六进制值转换为 IPv4 地址。
[[maybe_unused]] std::string ipv4_from_route_hex(const std::string& hex_value)
{
    if (hex_value.empty()) {
        return {};
    }

    unsigned long value = 0;
    try {
        value = std::stoul(hex_value, nullptr, 16);
    } catch (...) {
        return {};
    }

    char buffer[32]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%lu.%lu.%lu.%lu",
        value & 0xFFUL,
        (value >> 8U) & 0xFFUL,
        (value >> 16U) & 0xFFUL,
        (value >> 24U) & 0xFFUL);
    return buffer;
}

// 从路由表读取默认网关。
[[maybe_unused]] std::string read_default_gateway(const std::string& interface_name)
{
    std::ifstream stream("/proc/net/route");
    if (!stream) {
        return {};
    }

    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string iface;
        std::string destination;
        std::string gateway;
        std::string flags;
        if (!(row >> iface >> destination >> gateway >> flags)) {
            continue;
        }
        if (iface != interface_name || destination != "00000000") {
            continue;
        }
        return ipv4_from_route_hex(gateway);
    }
    return {};
}

std::string read_default_route_interface()
{
    std::ifstream stream("/proc/net/route");
    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string interface_name;
        std::string destination;
        std::string gateway;
        std::string flags;
        if (row >> interface_name >> destination >> gateway >> flags && destination == "00000000") {
            return interface_name;
        }
    }
    return {};
}

// 读取网口当前 IPv4 地址。
[[maybe_unused]] std::string read_ipv4_address(const std::string& interface_name)
{
#if defined(__linux__)
    ifaddrs* addrs = nullptr;
    if (::getifaddrs(&addrs) != 0 || addrs == nullptr) {
        return {};
    }

    std::string result;
    for (auto* item = addrs; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_name == nullptr) {
            continue;
        }
        if (interface_name != item->ifa_name || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        char buffer[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) != nullptr) {
            result = buffer;
            break;
        }
    }

    ::freeifaddrs(addrs);
    return result;
#else
    (void)interface_name;
    return {};
#endif
}

// 将 IPv4 子网掩码转换为前缀长度。
[[maybe_unused]] int netmask_to_prefix(const std::string& netmask)
{
    std::uint32_t mask = 0;
    if (!backend_internal::parse_ipv4_address(netmask, &mask)) {
        return -1;
    }

    int prefix = 0;
    bool zero_seen = false;
    for (int bit = 31; bit >= 0; --bit) {
        const bool one = ((mask >> static_cast<unsigned int>(bit)) & 1U) != 0U;
        if (one && zero_seen) {
            return -1;
        }
        if (one) {
            ++prefix;
        } else {
            zero_seen = true;
        }
    }
    if (prefix <= 0 || prefix >= 32) {
        return -1;
    }
    return prefix;
}

#if defined(__linux__)
// 从套接字地址结构计算子网前缀长度。
[[maybe_unused]] int sockaddr_netmask_prefix(const sockaddr* address)
{
    if (address == nullptr || address->sa_family != AF_INET) {
        return -1;
    }
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    const std::uint32_t mask = ntohl(ipv4->sin_addr.s_addr);
    int prefix = 0;
    bool zero_seen = false;
    for (int bit = 31; bit >= 0; --bit) {
        const bool one = ((mask >> static_cast<unsigned int>(bit)) & 1U) != 0U;
        if (one && zero_seen) {
            return -1;
        }
        if (one) {
            ++prefix;
        } else {
            zero_seen = true;
        }
    }
    return prefix;
}
#endif

// 读取网口当前 IPv4 CIDR 配置。
[[maybe_unused]] std::string read_ipv4_cidr(const std::string& interface_name)
{
#if defined(__linux__)
    ifaddrs* addrs = nullptr;
    if (::getifaddrs(&addrs) != 0 || addrs == nullptr) {
        return {};
    }

    std::string result;
    for (auto* item = addrs; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_name == nullptr) {
            continue;
        }
        if (interface_name != item->ifa_name || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        char buffer[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
        const auto prefix = sockaddr_netmask_prefix(item->ifa_netmask);
        if (::inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) != nullptr && prefix >= 0) {
            result = std::string(buffer) + "/" + std::to_string(prefix);
            break;
        }
    }

    ::freeifaddrs(addrs);
    return result;
#else
    (void)interface_name;
    return {};
#endif
}

#if defined(__linux__)
// 读取指定进程的命令行参数。
std::vector<std::string> read_process_arguments(pid_t pid)
{
    std::ifstream stream("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!stream) {
        return {};
    }
    const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::vector<std::string> arguments;
    std::size_t begin = 0;
    while (begin < content.size()) {
        const auto end = content.find('\0', begin);
        arguments.push_back(content.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return arguments;
}

// 判断 DHCP 客户端进程是否绑定指定网口。
bool udhcpc_matches_interface(const std::vector<std::string>& arguments, const std::string& interface_name)
{
    if (arguments.empty()) {
        return false;
    }
    const auto slash = arguments[0].find_last_of('/');
    const auto executable = slash == std::string::npos ? arguments[0] : arguments[0].substr(slash + 1);
    if (executable != "udhcpc") {
        return false;
    }
    bool has_interface_option = false;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if ((arguments[index] == "-i" || arguments[index] == "--interface") &&
            index + 1 < arguments.size()) {
            has_interface_option = true;
            return arguments[index + 1] == interface_name;
        }
        if (arguments[index].rfind("--interface=", 0) == 0) {
            has_interface_option = true;
            return arguments[index] == "--interface=" + interface_name;
        }
    }
    return !has_interface_option && interface_name == kDefaultNetworkInterfaceName;
}

// 查找正在运行的 udhcpc 进程。
std::vector<process_identity_internal::ProcessIdentity> find_udhcpc_processes(
    const std::string& interface_name)
{
    std::vector<process_identity_internal::ProcessIdentity> processes;
    std::error_code error;
    for (const auto& entry : edge::fs::directory_iterator("/proc", error)) {
        if (error) {
            break;
        }
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        try {
            const auto pid = static_cast<pid_t>(std::stol(name));
            if (pid > 1 && udhcpc_matches_interface(read_process_arguments(pid), interface_name)) {
                const auto identity = process_identity_internal::capture_process_identity(pid);
                // 捕获 starttime 后再次验证命令行，避免扫描期间发生 exec/PID 切换。
                if (identity.has_value() &&
                    udhcpc_matches_interface(read_process_arguments(pid), interface_name)) {
                    processes.push_back(*identity);
                }
            }
        } catch (...) {
        }
    }
    return processes;
}

// 判断指定网口的 DHCP 客户端是否正在运行。
bool dhcp_client_running(const std::string& interface_name)
{
    return !find_udhcpc_processes(interface_name).empty();
}

// 状态页轮询只短时复用探测结果；配置应用、停止和回滚仍直接扫描并核对进程身份。
bool dhcp_client_running_for_status(const std::string& interface_name)
{
    struct Entry {
        bool running{false};
        std::chrono::steady_clock::time_point checked_at{};
    };
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, Entry> cache;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto found = cache.find(interface_name);
        if (found != cache.end() && now - found->second.checked_at < std::chrono::seconds(15)) {
            return found->second.running;
        }
    }
    const bool running = dhcp_client_running(interface_name);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[interface_name] = Entry{running, now};
    }
    return running;
}

// 解析并返回对应文件路径。
std::string dhcp_pid_file(const std::string& interface_name)
{
    return (edge::fs::path(controller_run_directory()) / ("udhcpc-" + interface_name + ".pid")).string();
}

// 停止指定网口上的 DHCP 客户端。
StatusCode stop_dhcp_clients(const std::string& interface_name, std::string* error_message)
{
    const auto processes = find_udhcpc_processes(interface_name);
    for (const auto& process : processes) {
        const auto signal_result = process_identity_internal::signal_same_process(process, SIGTERM);
        if (signal_result == process_identity_internal::SignalResult::kPermissionDenied ||
            signal_result == process_identity_internal::SignalResult::kError) {
            if (error_message != nullptr) {
                *error_message = "停止 DHCP 客户端失败：进程身份校验或信号发送失败";
            }
            return StatusCode::kIoError;
        }
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        bool any_running = false;
        for (const auto& process : processes) {
            if (process_identity_internal::same_process_identity(process)) {
                any_running = true;
                break;
            }
        }
        if (!any_running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (const auto& process : processes) {
        // 若原 PID 已退出并被复用，starttime 不匹配，signal_same_process 会跳过。
        const auto signal_result = process_identity_internal::signal_same_process(process, SIGKILL);
        if (signal_result == process_identity_internal::SignalResult::kPermissionDenied ||
            signal_result == process_identity_internal::SignalResult::kError) {
            if (error_message != nullptr) {
                *error_message = "强制停止 DHCP 客户端失败：进程身份校验或信号发送失败";
            }
            return StatusCode::kIoError;
        }
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        const bool any_running = std::any_of(
            processes.begin(),
            processes.end(),
            [](const auto& process) {
                return process_identity_internal::same_process_identity(process);
            });
        if (!any_running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto remaining_processes = find_udhcpc_processes(interface_name);
    if (!remaining_processes.empty()) {
        if (error_message != nullptr) {
            *error_message =
                "停止 DHCP 客户端失败：停止期间检测到仍在运行或重新启动的匹配进程";
        }
        return StatusCode::kIoError;
    }

    // 不在 stop 路径无条件删除 pidfile。外部 supervisor 可能刚刚重启了
    // udhcpc 并写入新的进程身份；删除它会让后续停止操作失去可靠目标。
    return StatusCode::kOk;
}

enum class DetachedStartupMessageType : std::uint32_t {
    kDetachedPid = 1,
    kError = 2,
};

struct DetachedStartupMessage {
    std::uint32_t type{0};
    std::int32_t value{0};
    std::uint64_t start_time_ticks{0};
};

static_assert(sizeof(DetachedStartupMessage) == 16, "detached startup wire message must remain fixed");

// 消息小于 PIPE_BUF；正常情况下单次 write 原子完成，循环仍处理 EINTR/短写。
bool write_detached_startup_message(
    int fd,
    DetachedStartupMessageType type,
    std::int32_t value,
    std::uint64_t start_time_ticks = 0) noexcept
{
    const DetachedStartupMessage message{
        static_cast<std::uint32_t>(type), value, start_time_ticks};
    const auto* data = reinterpret_cast<const std::uint8_t*>(&message);
    std::size_t written = 0;
    while (written < sizeof(message)) {
        const auto count = ::write(fd, data + written, sizeof(message) - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

// fork 后不能使用 ifstream/string 读取身份；仅用异步信号安全系统调用解析 /proc/self/stat。
bool read_self_start_time_ticks(std::uint64_t* start_time_ticks) noexcept
{
    if (start_time_ticks == nullptr) {
        errno = EINVAL;
        return false;
    }
    const int stat_fd = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (stat_fd < 0) {
        return false;
    }
    char stat[4096]{};
    std::size_t size = 0;
    while (size < sizeof(stat)) {
        const auto count = ::read(stat_fd, stat + size, sizeof(stat) - size);
        if (count > 0) {
            size += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        const int read_error = errno;
        (void)::close(stat_fd);
        errno = read_error;
        return false;
    }
    (void)::close(stat_fd);

    std::size_t command_end = size;
    while (command_end > 0 && stat[command_end - 1] != ')') {
        --command_end;
    }
    if (command_end == 0) {
        errno = EPROTO;
        return false;
    }
    std::size_t cursor = command_end;
    for (int field_number = 3; field_number <= 22; ++field_number) {
        while (cursor < size && stat[cursor] == ' ') {
            ++cursor;
        }
        if (cursor >= size) {
            errno = EPROTO;
            return false;
        }
        const auto field_begin = cursor;
        while (cursor < size && stat[cursor] != ' ') {
            ++cursor;
        }
        if (field_number != 22) {
            continue;
        }
        std::uint64_t value = 0;
        if (field_begin == cursor) {
            errno = EPROTO;
            return false;
        }
        for (auto index = field_begin; index < cursor; ++index) {
            const unsigned char character = static_cast<unsigned char>(stat[index]);
            if (character < '0' || character > '9') {
                errno = EPROTO;
                return false;
            }
            const auto digit = static_cast<std::uint64_t>(character - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                errno = ERANGE;
                return false;
            }
            value = value * 10U + digit;
        }
        *start_time_ticks = value;
        return true;
    }
    errno = EPROTO;
    return false;
}

// 启动协议管道不能占用 0/1/2；否则后续把标准流重定向到 /dev/null 会覆盖写端。
bool move_descriptor_above_standard_streams(int* fd) noexcept
{
    if (fd == nullptr || *fd < 0) {
        errno = EBADF;
        return false;
    }
    if (*fd > STDERR_FILENO) {
        return true;
    }
    int moved_fd = -1;
#if defined(F_DUPFD_CLOEXEC)
    moved_fd = ::fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    moved_fd = ::fcntl(*fd, F_DUPFD, STDERR_FILENO + 1);
    if (moved_fd >= 0 && ::fcntl(moved_fd, F_SETFD, FD_CLOEXEC) != 0) {
        const int move_error = errno;
        (void)::close(moved_fd);
        errno = move_error;
        moved_fd = -1;
    }
#endif
    if (moved_fd < 0) {
        return false;
    }
    (void)::close(*fd);
    *fd = moved_fd;
    return true;
}

// execvp 不在 POSIX 要求的 async-signal-safe 集合中。在 fork 前展开 PATH，
// child 中只依次调用 execve，同时保留 execvp 对 EACCES 和 ENOEXEC 的主要语义。
std::vector<std::string> executable_candidates(const std::string& executable)
{
    if (executable.find('/') != std::string::npos) {
        return {executable};
    }

    const char* configured_path = std::getenv("PATH");
    const std::string search_path =
        configured_path == nullptr ? std::string{"/bin:/usr/bin"} : configured_path;
    std::vector<std::string> candidates;
    std::size_t segment_begin = 0;
    while (true) {
        const auto separator = search_path.find(':', segment_begin);
        const auto segment = search_path.substr(
            segment_begin,
            separator == std::string::npos
                ? std::string::npos
                : separator - segment_begin);
        candidates.push_back(
            segment.empty() ? executable : segment + "/" + executable);
        if (separator == std::string::npos) {
            break;
        }
        segment_begin = separator + 1;
    }
    return candidates;
}

// fork 后只执行异步信号安全的 execve 和标量运算；所有路径、argv/envp 均由父进程准备。
int execute_prepared_candidates(
    char* const* candidates,
    std::size_t candidate_count,
    char* const* command_argv,
    char** shell_argv,
    char* const* environment) noexcept
{
    int exec_error = ENOENT;
    bool saw_permission_error = false;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        char* const executable = candidates[index];
        ::execve(executable, command_argv, environment);
        exec_error = errno;
        if (exec_error == ENOEXEC) {
            shell_argv[1] = executable;
            ::execve("/bin/sh", shell_argv, environment);
            return errno;
        }
        if (exec_error == EACCES) {
            saw_permission_error = true;
            continue;
        }
        if (exec_error == ENOENT || exec_error == ENOTDIR) {
            continue;
        }
        return exec_error;
    }
    return saw_permission_error ? EACCES : exec_error;
}

// 第二次 fork 发生在多线程父进程的 child 快照内。直接使用 Linux
// fork syscall，避免 libc fork 再次调用继承的 pthread_atfork handler。
pid_t fork_detached_child_without_atfork() noexcept
{
#if defined(SYS_fork)
    return static_cast<pid_t>(::syscall(SYS_fork));
#elif defined(SYS_clone)
    return static_cast<pid_t>(::syscall(
        SYS_clone,
        static_cast<unsigned long>(SIGCHLD),
        nullptr,
        nullptr,
        nullptr,
        0UL));
#else
#error "Linux detached startup requires a raw fork or clone syscall"
#endif
}

// 启动脱离当前进程的后台命令；只等待 setsid/exec 握手，不等待命令运行结束。
StatusCode start_detached_process_impl(
    const std::vector<std::string>& args,
    std::string* error_message,
    const network_runtime_internal::DetachedProcessOptions& options)
{
    if (args.empty()) {
        if (error_message != nullptr) {
            *error_message = "DHCP 客户端命令为空";
        }
        return StatusCode::kInvalidArgument;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    auto executable_candidate_paths = executable_candidates(args.front());
    std::vector<char*> executable_candidate_argv;
    executable_candidate_argv.reserve(executable_candidate_paths.size());
    for (auto& candidate : executable_candidate_paths) {
        executable_candidate_argv.push_back(candidate.data());
    }
    std::vector<char*> shell_argv;
    shell_argv.reserve(args.size() + 2);
    shell_argv.push_back(const_cast<char*>("sh"));
    shell_argv.push_back(nullptr);  // child 中在 ENOEXEC 路径填入脚本路径。
    for (std::size_t index = 1; index < args.size(); ++index) {
        shell_argv.push_back(const_cast<char*>(args[index].c_str()));
    }
    shell_argv.push_back(nullptr);
    char* const* const command_argv = argv.data();
    char* const* const executable_candidate_data = executable_candidate_argv.data();
    const std::size_t executable_candidate_count = executable_candidate_argv.size();
    char** const shell_argv_data = shell_argv.data();
    char* const* const child_environment = ::environ;
    const int descriptor_limit =
        linux_child_process_internal::descriptor_limit_before_fork();
    int error_pipe[2]{};
    if (linux_child_process_internal::create_cloexec_pipe(error_pipe) != 0) {
        if (error_message != nullptr) {
            *error_message = "创建 DHCP 启动管道失败：" + std::string(std::strerror(errno));
        }
        return StatusCode::kIoError;
    }
    if (!move_descriptor_above_standard_streams(&error_pipe[0]) ||
        !move_descriptor_above_standard_streams(&error_pipe[1])) {
        const int pipe_error = errno;
        if (error_pipe[0] >= 0) (void)::close(error_pipe[0]);
        if (error_pipe[1] >= 0) (void)::close(error_pipe[1]);
        if (error_message != nullptr) {
            *error_message =
                "迁移 DHCP 启动管道失败：" + std::string(std::strerror(pipe_error));
        }
        return StatusCode::kIoError;
    }
    const int pipe_flags = ::fcntl(error_pipe[0], F_GETFL, 0);
    if (pipe_flags < 0 ||
        ::fcntl(error_pipe[0], F_SETFL, pipe_flags | O_NONBLOCK) != 0) {
        const int pipe_error = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (error_message != nullptr) {
            *error_message =
                "设置 DHCP 启动管道失败：" + std::string(std::strerror(pipe_error));
        }
        return StatusCode::kIoError;
    }

    const auto raw_pre_exec_delay = options.pre_exec_delay_for_test.count();
    const int pre_exec_delay_ms = static_cast<int>(std::max<std::int64_t>(
        0,
        std::min<std::int64_t>(raw_pre_exec_delay, std::numeric_limits<int>::max())));
    const auto startup_timeout =
        std::max(options.startup_timeout, std::chrono::milliseconds(1));
    const auto startup_deadline = std::chrono::steady_clock::now() + startup_timeout;
    const pid_t first_child = ::fork();
    if (first_child < 0) {
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (error_message != nullptr) {
            *error_message = "启动 DHCP 客户端失败：" + std::string(std::strerror(errno));
        }
        return StatusCode::kIoError;
    }
    if (first_child == 0) {
        ::close(error_pipe[0]);
        linux_child_process_internal::close_inherited_descriptors(
            error_pipe[1], descriptor_limit);
        if (::setpgid(0, 0) != 0) {
            const int child_error = errno;
            (void)write_detached_startup_message(
                error_pipe[1], DetachedStartupMessageType::kError, child_error);
            _exit(126);
        }
        const pid_t detached_child = fork_detached_child_without_atfork();
        if (detached_child < 0) {
            const int child_error = errno;
            (void)write_detached_startup_message(
                error_pipe[1], DetachedStartupMessageType::kError, child_error);
            _exit(1);
        }
        if (detached_child > 0) {
            _exit(0);
        }
        std::uint64_t detached_start_time_ticks = 0;
        if (!read_self_start_time_ticks(&detached_start_time_ticks)) {
            const int child_error = errno == 0 ? EIO : errno;
            (void)write_detached_startup_message(
                error_pipe[1], DetachedStartupMessageType::kError, child_error);
            _exit(126);
        }
        if (!write_detached_startup_message(
                error_pipe[1],
                DetachedStartupMessageType::kDetachedPid,
                static_cast<std::int32_t>(::getpid()),
                detached_start_time_ticks)) {
            _exit(126);
        }
        if (::setsid() < 0) {
            const int child_error = errno;
            (void)write_detached_startup_message(
                error_pipe[1], DetachedStartupMessageType::kError, child_error);
            _exit(126);
        }
        if (pre_exec_delay_ms > 0) {
            int wait_result = 0;
            do {
                wait_result = ::poll(nullptr, 0, pre_exec_delay_ms);
            } while (wait_result < 0 && errno == EINTR);
        }
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd < 0 || ::dup2(null_fd, STDIN_FILENO) < 0 ||
            ::dup2(null_fd, STDOUT_FILENO) < 0 ||
            ::dup2(null_fd, STDERR_FILENO) < 0) {
            const int child_error = errno;
            (void)write_detached_startup_message(
                error_pipe[1], DetachedStartupMessageType::kError, child_error);
            if (null_fd >= 0 && null_fd > STDERR_FILENO) (void)::close(null_fd);
            _exit(126);
        }
        if (null_fd > STDERR_FILENO) {
            ::close(null_fd);
        }
        const int exec_error = execute_prepared_candidates(
            executable_candidate_data,
            executable_candidate_count,
            command_argv,
            shell_argv_data,
            child_environment);
        (void)write_detached_startup_message(
            error_pipe[1], DetachedStartupMessageType::kError, exec_error);
        _exit(127);
    }

    ::close(error_pipe[1]);
    while (::setpgid(first_child, first_child) != 0 && errno == EINTR) {
    }

    constexpr std::size_t kMaxStartupWireBytes = sizeof(DetachedStartupMessage) * 8U;
    std::vector<std::uint8_t> wire_bytes;
    wire_bytes.reserve(kMaxStartupWireBytes);
    std::optional<pid_t> detached_pid;
    std::optional<process_identity_internal::ProcessIdentity> detached_identity;
    int child_error = 0;
    int pipe_error = 0;
    bool pipe_eof = false;
    bool protocol_error = false;

    const auto parse_wire_messages = [&]() {
        while (wire_bytes.size() >= sizeof(DetachedStartupMessage)) {
            DetachedStartupMessage message{};
            std::memcpy(&message, wire_bytes.data(), sizeof(message));
            wire_bytes.erase(
                wire_bytes.begin(),
                wire_bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(message)));
            if (message.type ==
                static_cast<std::uint32_t>(DetachedStartupMessageType::kDetachedPid)) {
                const auto reported_pid = static_cast<pid_t>(message.value);
                if (reported_pid <= 1 ||
                    (detached_pid.has_value() && *detached_pid != reported_pid)) {
                    protocol_error = true;
                    return;
                }
                detached_pid = reported_pid;
                const process_identity_internal::ProcessIdentity reported_identity{
                    static_cast<std::int64_t>(reported_pid), message.start_time_ticks};
                if (detached_identity.has_value() &&
                    detached_identity->start_time_ticks != reported_identity.start_time_ticks) {
                    protocol_error = true;
                    return;
                }
                detached_identity = reported_identity;
            } else if (
                message.type == static_cast<std::uint32_t>(DetachedStartupMessageType::kError)) {
                if (message.start_time_ticks != 0) {
                    protocol_error = true;
                    return;
                }
                child_error = message.value > 0 ? message.value : EIO;
            } else {
                protocol_error = true;
                return;
            }
        }
    };

    const auto drain_startup_pipe = [&]() {
        std::uint8_t buffer[64]{};
        while (!pipe_eof && !protocol_error && pipe_error == 0) {
            std::size_t read_size = sizeof(buffer);
            if (options.max_wire_read_bytes_for_test > 0) {
                read_size = std::min(read_size, options.max_wire_read_bytes_for_test);
            }
            const auto count = ::read(error_pipe[0], buffer, read_size);
            if (count > 0) {
                if (wire_bytes.size() + static_cast<std::size_t>(count) >
                    kMaxStartupWireBytes) {
                    protocol_error = true;
                    return;
                }
                wire_bytes.insert(wire_bytes.end(), buffer, buffer + count);
                parse_wire_messages();
                continue;
            }
            if (count == 0) {
                pipe_eof = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            pipe_error = errno;
            return;
        }
    };

    bool timed_out = false;
    while (!pipe_eof && pipe_error == 0 && !protocol_error) {
        drain_startup_pipe();
        if (pipe_eof || pipe_error != 0 || protocol_error) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= startup_deadline) {
            timed_out = true;
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            startup_deadline - now);
        pollfd descriptor{};
        descriptor.fd = error_pipe[0];
        descriptor.events = POLLIN | POLLHUP | POLLERR;
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(
            1,
            std::min<std::int64_t>(remaining.count(), std::numeric_limits<int>::max())));
        const int poll_result = ::poll(&descriptor, 1, wait_ms);
        if (poll_result < 0 && errno == EINTR) {
            // 返回外层重新计算绝对截止时间，持续信号不能延长启动总超时。
            continue;
        }
        if (poll_result < 0) {
            pipe_error = errno;
            break;
        }
        if (poll_result == 0) {
            // duration_cast 会向下取整；只由外层 steady_clock 检查判定总超时。
            continue;
        }
    }
    drain_startup_pipe();
    if (pipe_eof && !wire_bytes.empty()) {
        protocol_error = true;
    }

    const bool startup_failed =
        timed_out || pipe_error != 0 || protocol_error || child_error != 0;
    if (startup_failed) {
        // first_child 在 waitpid 前仍保留 PID；它建立的启动进程组不会被无关进程复用。
        (void)::kill(-first_child, SIGKILL);
        (void)::kill(first_child, SIGKILL);
        if (timed_out && pipe_error == 0 && !protocol_error) {
            // 必须在发送进程组信号后再排空一次：若 detached child 在
            // 超时前的最后一次读与 killpg 之间刚好写完身份并 setsid，
            // killpg 已无法覆盖它，但身份消息已完整地在 PIPE_BUF 管道中。
            // 反之，若信号先到，它仍在原进程组内且 SIGKILL 不会因
            // 随后的 setsid 丢失。
            drain_startup_pipe();
        }
        if (detached_identity.has_value()) {
            (void)process_identity_internal::signal_same_process(*detached_identity, SIGKILL);
        }
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(first_child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    ::close(error_pipe[0]);

    if (timed_out) {
        if (error_message != nullptr) {
            *error_message = "启动 DHCP 客户端超时，已清理启动进程";
            if (detached_pid.has_value()) {
                *error_message += "（detached PID=" + std::to_string(*detached_pid) + "）";
            }
        }
        return StatusCode::kTimeout;
    }
    if (pipe_error != 0) {
        if (error_message != nullptr) {
            *error_message =
                "读取 DHCP 启动结果失败：" + std::string(std::strerror(pipe_error));
        }
        return StatusCode::kIoError;
    }
    if (protocol_error) {
        if (error_message != nullptr) {
            *error_message = "读取 DHCP 启动结果失败：启动消息不完整或格式异常";
        }
        return StatusCode::kIoError;
    }
    if (child_error != 0) {
        if (error_message != nullptr) {
            *error_message =
                "启动 DHCP 客户端失败：" + std::string(std::strerror(child_error));
        }
        return child_error == ENOENT ? StatusCode::kNotFound : StatusCode::kIoError;
    }
    if (waited != first_child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        !pipe_eof || !detached_pid.has_value()) {
        if (detached_identity.has_value()) {
            (void)process_identity_internal::signal_same_process(*detached_identity, SIGKILL);
        }
        if (error_message != nullptr) {
            *error_message = "启动 DHCP 客户端失败：启动器子进程异常退出";
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 启动指定网口的 DHCP 客户端。
StatusCode start_dhcp_client(const std::string& interface_name, std::string* error_message)
{
    std::error_code directory_error;
    edge::fs::create_directories(controller_run_directory(), directory_error);
    if (directory_error) {
        if (error_message != nullptr) {
            *error_message = "创建 DHCP 运行目录失败：" + directory_error.message();
        }
        return StatusCode::kIoError;
    }
    return network_runtime_internal::start_detached_process(
        {"udhcpc", "-i", interface_name, "-p", dhcp_pid_file(interface_name), "-b"},
        error_message);
}
#else
// 判断指定网口的 DHCP 客户端是否正在运行。
[[maybe_unused]] bool dhcp_client_running(const std::string&) { return false; }
[[maybe_unused]] bool dhcp_client_running_for_status(const std::string&) { return false; }
#endif

}  // namespace

namespace network_runtime_internal {

StatusCode start_detached_process(
    const std::vector<std::string>& arguments,
    std::string* error_message,
    const DetachedProcessOptions& options)
{
#if !defined(__linux__)
    (void)arguments;
    (void)options;
    if (error_message != nullptr) {
        *error_message = "当前平台不支持启动 detached 系统命令";
    }
    return StatusCode::kInvalidState;
#else
    return start_detached_process_impl(arguments, error_message, options);
#endif
}

// 运行命令，并对执行时间、输出内存和整个子进程组的生命周期设置边界。
CommandResult run_command(
    const std::vector<std::string>& args,
    std::chrono::milliseconds timeout,
    std::size_t max_output_bytes)
{
    CommandResult result;
#if !defined(__linux__)
    (void)args;
    (void)timeout;
    (void)max_output_bytes;
    result.output = "当前平台不支持修改系统网口配置";
    return result;
#else
    if (args.empty()) {
        result.output = "系统命令为空";
        return result;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds(1);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    auto executable_candidate_paths = executable_candidates(args.front());
    std::vector<char*> executable_candidate_argv;
    executable_candidate_argv.reserve(executable_candidate_paths.size());
    for (auto& candidate : executable_candidate_paths) {
        executable_candidate_argv.push_back(candidate.data());
    }
    std::vector<char*> shell_argv;
    shell_argv.reserve(args.size() + 2);
    shell_argv.push_back(const_cast<char*>("sh"));
    shell_argv.push_back(nullptr);
    for (std::size_t index = 1; index < args.size(); ++index) {
        shell_argv.push_back(const_cast<char*>(args[index].c_str()));
    }
    shell_argv.push_back(nullptr);
    char* const* const command_argv = argv.data();
    char* const* const executable_candidate_data = executable_candidate_argv.data();
    const std::size_t executable_candidate_count = executable_candidate_argv.size();
    char** const shell_argv_data = shell_argv.data();
    char* const* const child_environment = ::environ;
    const int descriptor_limit =
        linux_child_process_internal::descriptor_limit_before_fork();

    int pipe_fds[2]{};
    if (linux_child_process_internal::create_cloexec_pipe(pipe_fds) != 0) {
        result.output = "创建命令输出管道失败: " + std::string(std::strerror(errno));
        return result;
    }
    const int pipe_flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    if (pipe_flags < 0 || ::fcntl(pipe_fds[0], F_SETFL, pipe_flags | O_NONBLOCK) != 0) {
        const int pipe_error = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        result.output = "设置命令输出管道失败: " + std::string(std::strerror(pipe_error));
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int fork_error = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        result.output = "启动系统命令失败: " + std::string(std::strerror(fork_error));
        return result;
    }

    if (pid == 0) {
        ::close(pipe_fds[0]);
        if (::setpgid(0, 0) != 0 || ::dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
            ::dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (pipe_fds[1] > STDERR_FILENO) {
            ::close(pipe_fds[1]);
        }
        linux_child_process_internal::close_inherited_descriptors(
            -1, descriptor_limit);
        const int exec_error = execute_prepared_candidates(
            executable_candidate_data,
            executable_candidate_count,
            command_argv,
            shell_argv_data,
            child_environment);
        _exit(exec_error == ENOENT ? 127 : 126);
    }

    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    const auto append_output = [&](const char* data, std::size_t size) {
        const auto remaining = max_output_bytes > result.output.size()
            ? max_output_bytes - result.output.size()
            : 0U;
        const auto copy_size = std::min(remaining, size);
        if (copy_size > 0) {
            result.output.append(data, copy_size);
        }
        if (copy_size < size) {
            result.output_truncated = true;
        }
    };
    const auto append_diagnostic = [&](const std::string& message) {
        if (!result.output.empty()) {
            append_output("\n", 1);
        }
        append_output(message.data(), message.size());
    };

    bool force_cleanup = false;
    if (::setpgid(pid, pid) != 0) {
        const int group_error = errno;
        // 子进程可能已经在 exec 或退出；这两个结果不表示进程组创建失败。
        if (group_error != EACCES && group_error != ESRCH) {
            append_diagnostic("创建命令进程组失败: " + std::string(std::strerror(group_error)));
            force_cleanup = true;
        }
    }

    bool pipe_eof = false;
    bool child_reaped = false;
    bool child_status_valid = false;
    int child_status = 0;

    const auto drain_output = [&]() {
        if (pipe_eof) {
            return;
        }
        char buffer[4096]{};
        std::size_t drained_bytes = 0;
        constexpr std::size_t kMaxDrainBytesPerPass = 64U * 1024U;
        while (drained_bytes < kMaxDrainBytesPerPass) {
            const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
            if (count > 0) {
                // 达到内存上限后仍继续排空管道，避免子进程被满管道反向阻塞。
                append_output(buffer, static_cast<std::size_t>(count));
                drained_bytes += static_cast<std::size_t>(count);
                continue;
            }
            if (count == 0) {
                pipe_eof = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            const int read_error = errno;
            append_diagnostic("读取命令输出失败: " + std::string(std::strerror(read_error)));
            pipe_eof = true;
            return;
        }
        // 持续输出的命令每轮最多排空固定字节数，确保主循环能按时检查总截止时间。
    };

    const auto collect_child_status = [&]() {
        if (child_reaped) {
            return;
        }
        while (true) {
            const pid_t waited = ::waitpid(pid, &child_status, WNOHANG);
            if (waited == pid) {
                child_reaped = true;
                child_status_valid = true;
                return;
            }
            if (waited == 0) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                append_diagnostic("等待系统命令结束失败: " + std::string(std::strerror(errno)));
            }
            child_reaped = true;
            return;
        }
    };

    const auto wait_for_activity = [&](std::chrono::steady_clock::time_point deadline,
                                       std::chrono::milliseconds max_slice) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return true;
        }
        auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        wait_time = std::min(wait_time, max_slice);
        const int wait_ms = static_cast<int>(std::max<std::int64_t>(1, wait_time.count()));

        int poll_result = 0;
        do {
            if (pipe_eof) {
                poll_result = ::poll(nullptr, 0, wait_ms);
            } else {
                pollfd descriptor{};
                descriptor.fd = pipe_fds[0];
                descriptor.events = POLLIN | POLLHUP | POLLERR;
                poll_result = ::poll(&descriptor, 1, wait_ms);
            }
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            append_diagnostic("轮询命令状态失败: " + std::string(std::strerror(errno)));
            return false;
        }
        return true;
    };

    const auto command_deadline = std::chrono::steady_clock::now() + timeout;
    while (!force_cleanup) {
        drain_output();
        collect_child_status();
        if (child_reaped && pipe_eof) {
            break;
        }
        if (std::chrono::steady_clock::now() >= command_deadline) {
            result.timed_out = true;
            break;
        }
        if (!wait_for_activity(command_deadline, std::chrono::milliseconds(50))) {
            force_cleanup = true;
            break;
        }
    }

    if (result.timed_out || force_cleanup) {
        const auto signal_command_group = [&](int signal_number) {
            if (::kill(-pid, signal_number) != 0 && errno == ESRCH && !child_reaped) {
                // setpgid 极早失败时仍至少终止直接子进程。
                ::kill(pid, signal_number);
            }
        };

        signal_command_group(SIGTERM);
        const auto terminate_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < terminate_deadline &&
               (!child_reaped || !pipe_eof)) {
            drain_output();
            collect_child_status();
            if (child_reaped && pipe_eof) {
                break;
            }
            wait_for_activity(terminate_deadline, std::chrono::milliseconds(25));
        }

        // 即使直接子进程已响应 TERM，也再次清理同组中可能忽略 TERM 的后代。
        signal_command_group(SIGKILL);
        const auto kill_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < kill_deadline &&
               (!child_reaped || !pipe_eof)) {
            drain_output();
            collect_child_status();
            if (child_reaped && pipe_eof) {
                break;
            }
            wait_for_activity(kill_deadline, std::chrono::milliseconds(25));
        }
        drain_output();
        collect_child_status();
        if (!child_reaped) {
            append_diagnostic("回收系统命令子进程超时");
        }
    }

    ::close(pipe_fds[0]);
    if (child_status_valid && !force_cleanup) {
        if (WIFEXITED(child_status)) {
            result.exit_code = WEXITSTATUS(child_status);
        } else if (WIFSIGNALED(child_status)) {
            result.exit_code = 128 + WTERMSIG(child_status);
        }
    }
    result.output = backend_internal::trim_copy(result.output);
    return result;
#endif
}

}  // namespace network_runtime_internal

namespace {

// 生成命令显示文本。
[[maybe_unused]] std::string command_label(const std::vector<std::string>& args)
{
    std::string label;
    for (const auto& arg : args) {
        if (!label.empty()) {
            label += " ";
        }
        label += arg;
    }
    return label;
}

// 执行单个网络配置步骤并收集诊断结果。
[[maybe_unused]] StatusCode run_network_step(
    const std::vector<std::string>& args,
    const std::string& step_name,
    std::string* error_message)
{
    const auto result = network_runtime_internal::run_command(args);
    if (result.exit_code == 0 && !result.timed_out) {
        return StatusCode::kOk;
    }

    std::string detail = result.output;
    if (result.timed_out) {
        detail = "命令执行超时，已终止命令进程组";
        if (!result.output.empty()) {
            detail += "：" + result.output;
        }
    } else if (detail.empty()) {
        detail = "退出码 " + std::to_string(result.exit_code);
    }
    if (result.output_truncated) {
        detail += "（命令输出已截断）";
    }
    if (result.exit_code == 126 || detail.find("Operation not permitted") != std::string::npos ||
        detail.find("Permission denied") != std::string::npos) {
        detail = "权限不足，无法修改系统网口配置";
    } else if (result.exit_code == 127) {
        detail = "系统命令 ip 不可用，无法修改系统网口配置";
    }

    if (error_message != nullptr) {
        *error_message = step_name + "失败：" + detail;
    }
    Logger::warn("应用网络配置步骤失败：" + step_name + "，命令=" + command_label(args) + "，错误=" + detail);
    return StatusCode::kIoError;
}

// 写入静态网络配置使用的 DNS 列表。
[[maybe_unused]] StatusCode write_static_dns(
    const std::vector<std::string>& dns_servers,
    std::string* error_message)
{
#if !defined(__linux__)
    (void)dns_servers;
    if (error_message != nullptr) {
        *error_message = "当前平台不支持写入 DNS 配置";
    }
    return StatusCode::kInvalidState;
#else
    std::ofstream stream("/etc/resolv.conf", std::ios::trunc);
    if (!stream) {
        if (error_message != nullptr) {
            *error_message = "写入 DNS 配置失败：无法打开 /etc/resolv.conf";
        }
        return StatusCode::kIoError;
    }
    for (const auto& dns_server : dns_servers) {
        stream << "nameserver " << dns_server << '\n';
    }
    stream.flush();
    if (!stream) {
        if (error_message != nullptr) {
            *error_message = "写入 DNS 配置失败：/etc/resolv.conf 写入异常";
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
#endif
}

// 读取配置文件内容。
[[maybe_unused]] std::string read_file_content(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// 恢复先前保存的配置文件内容。
[[maybe_unused]] StatusCode restore_file_content(
    const std::string& path,
    const std::string& content,
    std::string* error_message)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error_message != nullptr) {
            *error_message = "无法打开 " + path;
        }
        return StatusCode::kIoError;
    }
    stream << content;
    stream.flush();
    if (!stream) {
        if (error_message != nullptr) {
            *error_message = "写入 " + path + " 失败";
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 在应用失败时恢复先前的网络状态。
[[maybe_unused]] StatusCode restore_previous_network_state(
    const std::string& interface_name,
    const std::string& previous_cidr,
    const std::string& previous_gateway,
    const std::optional<bool>& previous_link_up,
    std::string* error_message)
{
    std::vector<std::string> failures;
    const auto restore_step = [&](const std::vector<std::string>& args, const std::string& name) {
        std::string step_error;
        if (!is_ok(run_network_step(args, name, &step_error))) {
            failures.push_back(step_error.empty() ? name + "失败" : step_error);
        }
    };

    // 即使原先没有 IPv4 地址也必须清掉本次应用留下的部分地址。
    restore_step(
        {"ip", "-4", "addr", "flush", "dev", interface_name},
        "恢复前清理网口地址");
    const bool needs_link_up_for_restore = previous_link_up.value_or(false) ||
        !previous_cidr.empty() || !previous_gateway.empty();
    if (needs_link_up_for_restore) {
        restore_step(
            {"ip", "link", "set", interface_name, "up"},
            "恢复网口启用状态");
    }
    if (!previous_cidr.empty()) {
        restore_step(
            {"ip", "-4", "addr", "add", previous_cidr, "dev", interface_name},
            "恢复原网口地址");
        if (!previous_gateway.empty()) {
            restore_step(
                {"ip", "-4", "route", "replace", "default", "via", previous_gateway,
                 "dev", interface_name},
            "恢复原默认网关");
        }
    }
    if (previous_link_up.has_value() && !*previous_link_up) {
        restore_step(
            {"ip", "link", "set", interface_name, "down"},
            "恢复网口关闭状态");
    }

    if (failures.empty()) {
        return StatusCode::kOk;
    }
    if (error_message != nullptr) {
        error_message->clear();
        for (const auto& failure : failures) {
            if (!error_message->empty()) *error_message += "；";
            *error_message += failure;
        }
    }
    return StatusCode::kIoError;
}

// 读取 Linux 当前真实网络运行状态。
NetworkRuntimeStatus read_linux_network_runtime_status(const std::string& interface_name)
{
    NetworkRuntimeStatus status;
    status.interface_name = interface_name.empty() ? kDefaultNetworkInterfaceName : interface_name;

#if !defined(__linux__)
    status.link_state = "unknown";
    status.link_state_text = link_state_text(status.link_state);
    status.message = "当前平台不支持读取 Linux 网口状态";
    return status;
#else
    const auto interface_path = edge::fs::path("/sys/class/net") / status.interface_name;
    status.interface_exists = edge::fs::exists(interface_path);
    if (!status.interface_exists) {
        status.operstate = "not_found";
        status.link_state = "not_found";
        status.link_state_text = link_state_text(status.link_state);
        status.message = "网口不存在：" + status.interface_name;
        return status;
    }

    status.operstate = read_text_file(interface_path / "operstate");
    if (status.operstate.empty()) {
        status.operstate = "unknown";
    }

    const auto carrier = read_text_file(interface_path / "carrier");
    if (carrier == "1") {
        status.link_state = "connected";
    } else if (carrier == "0") {
        status.link_state = "disconnected";
    } else if (status.operstate == "up") {
        status.link_state = "connected";
    } else if (status.operstate == "down") {
        status.link_state = "disconnected";
    } else {
        status.link_state = "unknown";
    }

    status.link_state_text = link_state_text(status.link_state);
    status.ip_address = read_ipv4_address(status.interface_name);
    status.default_gateway = read_default_gateway(status.interface_name);
    status.dhcp_client_running = dhcp_client_running_for_status(status.interface_name);
    if (status.ip_address.empty() && status.default_gateway.empty() && status.link_state == "unknown") {
        status.message = "当前网口状态不可用";
    }
    return status;
#endif
}

}  // namespace

// 获取当前系统网络接口运行状态。
NetworkRuntimeStatus BackendService::get_network_runtime_status() const
{
    std::string interface_name;
    std::string configured_mode;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        interface_name = system_config_.network_settings.interface_name;
        configured_mode = system_config_.network_settings.mode;
        if (network_config_policy::require_explicit_config() &&
            !system_config_.network_settings_explicitly_configured) {
            const auto current_interface = read_default_route_interface();
            if (!current_interface.empty()) interface_name = current_interface;
        }
    }
    auto status = read_linux_network_runtime_status(interface_name);
    status.configured_mode = configured_mode;
    return status;
}

// 将保存的网络配置应用到系统网络接口。
StatusCode BackendService::apply_network_settings(NetworkApplyResult* result, std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少网络配置应用结果输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    std::lock_guard<std::mutex> network_operation_lock(network_operation_mutex_);
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (network_config_policy::require_explicit_config() &&
            !system_config_.network_settings_explicitly_configured) {
            if (error_message != nullptr) {
                *error_message = "网络配置尚未明确确认，请使用保存并应用网络配置";
            }
            return StatusCode::kInvalidState;
        }
    }
    return apply_network_settings_internal(false, result, error_message);
}

// 后端启动时恢复持久化的网络配置模式。
StatusCode BackendService::apply_network_settings_on_startup(std::string* error_message)
{
    std::lock_guard<std::mutex> network_operation_lock(network_operation_mutex_);
    bool explicitly_configured = false;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        explicitly_configured = system_config_.network_settings_explicitly_configured;
    }
    if (network_config_policy::require_explicit_config() && !explicitly_configured) {
        if (error_message != nullptr) error_message->clear();
        Logger::info("首次安装尚未确认网络配置，保留系统当前网络状态");
        append_event(
            "info",
            "network_config",
            "startup",
            "首次安装尚未确认网络配置，保留系统当前网络状态",
            "未执行启动网络恢复，当前地址、路由、DHCP 客户端和 DNS 均保持不变",
            0);
        return StatusCode::kOk;
    }
    NetworkApplyResult result;
    return apply_network_settings_internal(true, &result, error_message);
}

// 执行网络配置应用流程，并记录每一步结果。
StatusCode BackendService::apply_network_settings_internal(
    bool startup_apply,
    NetworkApplyResult* result,
    std::string* error_message)
{
    // 调用方必须持有 network_operation_mutex_；save/apply 事务因此可覆盖失败回滚全过程。
    // 读取已持久化配置，并准备统一的成功和失败结果。
    NetworkSettings settings;
    {
        std::shared_lock<std::shared_mutex> lock(service_mutex_);
        if (!initialized_) {
            if (error_message != nullptr) {
                *error_message = "后端尚未完成初始化，无法应用网络配置";
            }
            return StatusCode::kInvalidState;
        }
        settings = system_config_.network_settings;
    }

    *result = NetworkApplyResult{};
    result->mode = settings.mode;
    result->interface_name = settings.interface_name;
    result->ip_address = settings.ip_address;

    const auto fail = [&](StatusCode status, const std::string& message) {
        result->applied = false;
        result->error_message = message;
        result->runtime_status = read_linux_network_runtime_status(settings.interface_name);
        result->runtime_status.configured_mode = settings.mode;
        result->current_ip_address = result->runtime_status.ip_address;
        if (error_message != nullptr) {
            *error_message = message;
        }
        append_event(
            "error",
            "network_config",
            settings.interface_name,
            startup_apply ? "启动时应用网络配置失败" : "应用网络配置失败",
            message,
            0);
        if (startup_apply) {
            Logger::warn("启动时应用网络配置失败：" + message);
            set_last_error("network_startup", settings.interface_name, "启动时应用网络配置失败：" + message, 0);
        } else {
            Logger::warn("应用网络配置失败：" + message);
            set_last_error("network_config", settings.interface_name, "应用网络配置失败：" + message, 0);
        }
        return status;
    };

#if !defined(__linux__)
    return fail(StatusCode::kInvalidState, "当前平台不支持修改系统网口配置");
#else
    // 校验 Linux 网口、权限和静态地址参数。
    const auto interface_path = edge::fs::path("/sys/class/net") / settings.interface_name;
    if (!edge::fs::exists(interface_path)) {
        return fail(StatusCode::kNotFound, "网口不存在：" + settings.interface_name);
    }

    if (::geteuid() != 0) {
        return fail(StatusCode::kInvalidState, "权限不足，无法修改系统网口配置");
    }

    int prefix = -1;
    if (settings.mode == kNetworkModeStatic) {
        prefix = netmask_to_prefix(settings.netmask);
        if (prefix < 0) {
            return fail(StatusCode::kInvalidArgument, "子网掩码格式不正确");
        }
    }

    // 保存当前网络状态，应用失败时用于回滚。
    const auto previous_cidr = read_ipv4_cidr(settings.interface_name);
    const auto previous_gateway = read_default_gateway(settings.interface_name);
    const auto previous_dns = read_file_content("/etc/resolv.conf");
    const auto previous_link_up = read_interface_admin_up(settings.interface_name);
    const bool previous_dhcp_running = dhcp_client_running(settings.interface_name);

    Logger::info(
        std::string(startup_apply ? "启动时开始恢复网络配置：" : "开始应用网络配置：") +
        "模式=" + settings.mode + "，网口=" + settings.interface_name);

    // 先统一启用网口并清理旧地址，再按静态或 DHCP 模式应用配置。
    std::string step_error;
    auto status = run_network_step({"ip", "link", "set", settings.interface_name, "up"}, "启用网口", &step_error);
    if (is_ok(status)) {
        status = stop_dhcp_clients(settings.interface_name, &step_error);
    }
    if (is_ok(status)) {
        status = run_network_step({"ip", "-4", "addr", "flush", "dev", settings.interface_name}, "清理网口 IPv4 地址", &step_error);
    }
    if (is_ok(status)) {
        status = run_network_step({"ip", "-4", "route", "flush", "dev", settings.interface_name}, "清理网口 IPv4 路由", &step_error);
    }

    if (is_ok(status) && settings.mode == kNetworkModeStatic) {
        const auto target_cidr = settings.ip_address + "/" + std::to_string(prefix);
        status = run_network_step(
            {"ip", "-4", "addr", "add", target_cidr, "dev", settings.interface_name},
            "设置静态 IPv4 地址",
            &step_error);
        if (is_ok(status)) {
            status = run_network_step(
                {"ip", "-4", "route", "replace", "default", "via", settings.gateway, "dev", settings.interface_name},
                "设置静态默认网关",
                &step_error);
        }
        if (is_ok(status)) {
            status = write_static_dns(settings.dns_servers, &step_error);
        }
    } else if (is_ok(status)) {
        status = start_dhcp_client(settings.interface_name, &step_error);
        if (is_ok(status)) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (!read_ipv4_address(settings.interface_name).empty()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (read_ipv4_address(settings.interface_name).empty()) {
                status = StatusCode::kTimeout;
                step_error = "DHCP 获取 IPv4 地址超时，请检查网线、路由器或 DHCP 服务";
            } else if (find_udhcpc_processes(settings.interface_name).size() != 1) {
                status = StatusCode::kInvalidState;
                step_error = "DHCP 客户端实例数量异常";
            }
        }
    }

    // 检查 DHCP 客户端数量与目标模式是否一致。
    if (is_ok(status)) {
        const auto running_clients = find_udhcpc_processes(settings.interface_name).size();
        if ((settings.mode == kNetworkModeStatic && running_clients != 0) ||
            (settings.mode == kNetworkModeDhcp && running_clients != 1)) {
            status = StatusCode::kInvalidState;
            step_error = "网络配置来源检查失败：DHCP 客户端实例数量不符合当前模式";
        }
    }

    // 任一步骤失败时恢复原地址、路由、DNS 和 DHCP 状态。
    if (!is_ok(status)) {
        if (startup_apply && settings.mode == kNetworkModeDhcp && status == StatusCode::kTimeout &&
            find_udhcpc_processes(settings.interface_name).size() == 1) {
            return fail(status, step_error);
        }
        std::vector<std::string> rollback_notes;
        std::string rollback_error;
        (void)stop_dhcp_clients(settings.interface_name, &rollback_error);

        rollback_error.clear();
        if (!is_ok(restore_previous_network_state(
                settings.interface_name,
                previous_cidr,
                previous_gateway,
                previous_link_up,
                &rollback_error))) {
            rollback_notes.push_back(rollback_error);
        }
        rollback_error.clear();
        if (!is_ok(restore_file_content("/etc/resolv.conf", previous_dns, &rollback_error))) {
            rollback_notes.push_back("恢复 DNS 失败：" + rollback_error);
        }

        auto rollback_clients = find_udhcpc_processes(settings.interface_name);
        if (previous_dhcp_running && rollback_clients.empty()) {
            rollback_error.clear();
            const auto restart_status = start_dhcp_client(settings.interface_name, &rollback_error);
            if (is_ok(restart_status)) {
                for (int attempt = 0; attempt < 10; ++attempt) {
                    rollback_clients = find_udhcpc_processes(settings.interface_name);
                    if (!rollback_clients.empty()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            if (!is_ok(restart_status)) {
                rollback_notes.push_back(
                    "恢复 DHCP 客户端失败：" +
                    (rollback_error.empty() ? std::string("启动失败") : rollback_error));
            }
        }

        rollback_clients = find_udhcpc_processes(settings.interface_name);
        const std::size_t expected_client_count = previous_dhcp_running ? 1U : 0U;
        if (rollback_clients.size() != expected_client_count) {
            rollback_notes.push_back(
                "DHCP 客户端实例未恢复到原状态，期望=" +
                std::to_string(expected_client_count) + "，实际=" +
                std::to_string(rollback_clients.size()));
        }

        std::string failure_message = step_error.empty() ? "应用网络配置失败" : step_error;
        if (!rollback_notes.empty()) {
            failure_message += "；网络状态回滚未完全恢复：";
            for (std::size_t index = 0; index < rollback_notes.size(); ++index) {
                if (index > 0) failure_message += "；";
                failure_message += rollback_notes[index];
            }
        }
        return fail(status, failure_message);
    }

    // 重新读取实际运行状态并记录应用成功事件。
    auto runtime = read_linux_network_runtime_status(settings.interface_name);
    runtime.configured_mode = settings.mode;
    result->applied = true;
    result->error_message.clear();
    result->runtime_status = runtime;
    result->current_ip_address = runtime.ip_address;
    if (settings.mode == kNetworkModeDhcp) {
        result->message = startup_apply
                              ? "启动时已恢复 DHCP 网络配置：" + settings.interface_name + " " + runtime.ip_address
                              : "网络配置已切换为 DHCP，当前地址为 http://" + runtime.ip_address + "。";
    } else {
        result->message = startup_apply
                              ? "启动时已应用静态网络配置：" + settings.interface_name + " " + settings.ip_address
                              : "网络配置已保存并应用到系统网口，请使用新的 IP 地址 http://" + settings.ip_address + " 重新访问页面。";
    }
    append_event(
        "info",
        "network_config",
        settings.interface_name,
        startup_apply ? "启动时已恢复网络配置：" + settings.mode : "网络配置已应用到系统网口",
        "模式=" + settings.mode + "，当前实际 IP=" + runtime.ip_address,
        0);
    Logger::info(
        std::string(startup_apply ? "启动时网络配置恢复成功：" : "网络配置应用成功：") +
        "模式=" + settings.mode + "，网口=" + settings.interface_name + "，当前实际 IP=" + runtime.ip_address);
    return StatusCode::kOk;
#endif
}

}  // namespace edge_controller
