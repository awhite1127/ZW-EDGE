// Linux 时间运行时：受控调用时区/NTP/hwclock 工具并维护运行摘要，不接触采集串口。
#include "infrastructure/platform/linux_time_runtime.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>

#include "shared/common/filesystem_compat.h"
#include "shared/common/logger.h"
#include "shared/common/time_utils.h"
#include "infrastructure/platform/linux_child_process.h"
#include "application/service/linux_process_identity.h"

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace edge_controller {
namespace {

// 解析受管 NTP 临时文件目录；正式 systemd 环境会显式注入该路径。
LinuxTimeRuntime::Paths default_runtime_paths()
{
    LinuxTimeRuntime::Paths paths;
    if (const auto* value = std::getenv("EDGE_CONTROLLER_RUN_DIR"); value != nullptr && value[0] != '\0') {
        paths.runtime_directory = value;
    }
    return paths;
}

using Clock = std::chrono::steady_clock;

// 去除字符串首尾空白。
std::string trim(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

// 读取文件。
std::string read_file(const edge::fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// 通过临时文件替换方式原子写入配置文件。
bool write_atomic_file(const edge::fs::path& path, const std::string& content, std::string* error)
{
    const auto temporary = edge::fs::path(path.string() + ".edge-controller.tmp");
    std::error_code ec;
    edge::fs::remove(temporary, ec);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !(stream << content) || !stream.flush()) {
            if (error != nullptr) {
                *error = "写入临时文件失败: " + temporary.string();
            }
            edge::fs::remove(temporary, ec);
            return false;
        }
    }
    edge::fs::rename(temporary, path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "原子替换文件失败: " + path.string() + "，原因=" + ec.message();
        }
        edge::fs::remove(temporary, ec);
        return false;
    }
    return true;
}

// 获取快照文件。
LinuxTimeRuntime::FileSnapshot snapshot_file(const edge::fs::path& path)
{
    LinuxTimeRuntime::FileSnapshot snapshot;
    std::error_code ec;
    snapshot.existed = edge::fs::exists(path, ec) || edge::fs::is_symlink(path, ec);
    if (!snapshot.existed) {
        return snapshot;
    }
    snapshot.was_symlink = edge::fs::is_symlink(path, ec);
    if (snapshot.was_symlink) {
        snapshot.symlink_target = edge::fs::read_symlink(path, ec).string();
    } else {
        snapshot.content = read_file(path);
    }
    return snapshot;
}

// 恢复文件。
bool restore_file(const edge::fs::path& path, const LinuxTimeRuntime::FileSnapshot& snapshot, std::string* error)
{
    std::error_code ec;
    edge::fs::remove(path, ec);
    if (!snapshot.existed) {
        return true;
    }
    if (snapshot.was_symlink) {
        edge::fs::create_symlink(snapshot.symlink_target, path, ec);
        if (ec && error != nullptr) {
            *error = "恢复软链接失败: " + path.string() + "，原因=" + ec.message();
        }
        return !ec;
    }
    return write_atomic_file(path, snapshot.content, error);
}

// 从路径中提取可执行文件名称。
std::string executable_name(const std::string& path)
{
    return edge::fs::path(path).filename().string();
}

// 解析输入并写入结构化结果。
bool parse_ipv4(const std::string& value)
{
    int segments = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('.', start);
        const auto part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part.size() > 3 ||
            !std::all_of(part.begin(), part.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            return false;
        }
        if (std::stoi(part) > 255 || (part.size() > 1 && part.front() == '0')) {
            return false;
        }
        ++segments;
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return segments == 4;
}

// 校验主机名格式是否合法。
bool is_hostname(const std::string& value)
{
    if (value.empty() || value.size() > 253 || value.front() == '.' || value.back() == '.') {
        return false;
    }
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find('.', start);
        const auto length = (end == std::string::npos ? value.size() : end) - start;
        if (length == 0 || length > 63 || value[start] == '-' || value[start + length - 1] == '-') {
            return false;
        }
        for (std::size_t i = start; i < start + length; ++i) {
            const auto ch = static_cast<unsigned char>(value[i]);
            if (!std::isalnum(ch) && ch != '-') {
                return false;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

#if defined(__linux__)
struct ProcessResult {
    int exit_code{-1};
    bool timed_out{false};
    std::string output;
};

// 运行进程。
ProcessResult run_process(const std::vector<std::string>& arguments, std::chrono::seconds timeout)
{
    ProcessResult result;
    if (arguments.empty()) {
        return result;
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    const int descriptor_limit =
        linux_child_process_internal::descriptor_limit_before_fork();
    int pipe_fds[2] = {-1, -1};
    if (linux_child_process_internal::create_cloexec_pipe(pipe_fds) != 0) {
        result.output = std::strerror(errno);
        return result;
    }
    const int pipe_flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    if (pipe_flags < 0 ||
        ::fcntl(pipe_fds[0], F_SETFL, pipe_flags | O_NONBLOCK) != 0) {
        const int pipe_error = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        result.output =
            "设置命令输出管道为非阻塞失败: " +
            std::string(std::strerror(pipe_error));
        return result;
    }
    const auto pid = ::fork();
    if (pid == 0) {
        // 命令独占一个进程组；超时时同时终止它派生的辅助进程，避免只回收直接子进程。
        ::close(pipe_fds[0]);
        if (::setpgid(0, 0) != 0 ||
            ::dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
            ::dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (pipe_fds[1] > STDERR_FILENO) {
            ::close(pipe_fds[1]);
        }
        linux_child_process_internal::close_inherited_descriptors(
            -1, descriptor_limit);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    ::close(pipe_fds[1]);
    if (pid < 0) {
        result.output = std::strerror(errno);
        ::close(pipe_fds[0]);
        return result;
    }
    // child/parent 双侧 setpgid 消除 fork 后调度先后带来的竞态；最终以实际 PGID 为准。
    while (::setpgid(pid, pid) != 0 && errno == EINTR) {
    }
    const bool process_group_ready = ::getpgid(pid) == pid;
    const auto signal_process_tree = [&](int signal_number) {
        if (process_group_ready && ::getpgid(pid) == pid) {
            if (::kill(-pid, signal_number) == 0 || errno != ESRCH) return;
        }
        (void)::kill(pid, signal_number);
    };
    const auto deadline = Clock::now() + timeout;
    int status = 0;
    while (true) {
        char buffer[512];
        const auto count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0 && result.output.size() < 8192) {
            result.output.append(buffer, static_cast<std::size_t>(count));
        }
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        if (waited < 0 && errno == ECHILD) {
            result.output += result.output.empty() ? "子进程状态不可用" : "；子进程状态不可用";
            break;
        }
        if (Clock::now() >= deadline) {
            result.timed_out = true;
            signal_process_tree(SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            // 即使直接子进程已响应 TERM，仍清理同组残留后代；直接子进程尚未 wait，PID/PGID 不会被复用。
            signal_process_tree(SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    while (true) {
        char buffer[512];
        const auto count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        if (result.output.size() < 8192) {
            result.output.append(buffer, static_cast<std::size_t>(count));
        }
    }
    ::close(pipe_fds[0]);
    result.output = trim(result.output);
    return result;
}

// 读取指定进程的命令行参数。
std::vector<std::string> proc_arguments(int pid)
{
    const auto raw = read_file("/proc/" + std::to_string(pid) + "/cmdline");
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start < raw.size()) {
        const auto end = raw.find('\0', start);
        values.push_back(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

// 读取指定进程对应的可执行文件路径。
std::string proc_executable(int pid)
{
    std::error_code ec;
    return edge::fs::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec).string();
}
#endif

// 同步状态文本。
std::string sync_state_text(const std::string& state)
{
    if (state == "synchronizing") return "正在同步";
    if (state == "synchronized") return "已同步";
    if (state == "unsynchronized") return "尚未同步";
    if (state == "taking_over") return "正在接管系统 NTP";
    if (state == "external") return "外部 NTP 正在运行";
    if (state == "takeover_failed") return "NTP 接管失败";
    if (state == "error") return "时间服务异常";
    return "NTP 已停用";
}

// 处理状态文本。
std::string process_state_text(const std::string& state)
{
    if (state == "managed") return "edge-controller 正在管理 NTP 同步";
    if (state == "taking_over") return "系统已有 ntpd 正在运行，正在尝试由 edge-controller 接管";
    if (state == "external") return "系统已有 ntpd 正在运行，edge-controller 暂未接管";
    if (state == "takeover_failed") return "系统已有 ntpd 正在运行，edge-controller 接管失败";
    return "NTP 未运行";
}

// 将进程号列表格式化为诊断文本。
std::string pid_list_text(const std::vector<int>& pids)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < pids.size(); ++index) {
        if (index > 0) stream << ",";
        stream << pids[index];
    }
    return stream.str();
}

// 提取命令输出的首个非空行。
std::string first_output_line(const std::string& output)
{
    const auto line_end = output.find_first_of("\r\n");
    return trim(output.substr(0, line_end));
}

// 清理硬件时钟输出中不稳定的秒级诊断片段。
std::string remove_hwclock_seconds_diagnostic(std::string line)
{
    const auto suffix = line.rfind(" seconds");
    if (suffix == std::string::npos || suffix + 8 != line.size()) return line;
    const auto value_begin = line.rfind(' ', suffix > 0 ? suffix - 1 : 0);
    if (value_begin == std::string::npos) return line;
    const auto value = line.substr(value_begin + 1, suffix - value_begin - 1);
    char* end = nullptr;
    (void)std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') return line;
    line.erase(value_begin);
    return trim(line);
}

// 解析输入并写入结构化结果。
bool parse_hwclock_time(const std::string& text, std::tm* parsed)
{
    if (parsed == nullptr) return false;
    const char* formats[] = {
        "%Y-%m-%d %H:%M:%S",
        "%a %b %d %H:%M:%S %Y",
        "%b %d %H:%M:%S %Y",
    };
    for (const auto* format : formats) {
        std::tm candidate{};
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        stream >> std::get_time(&candidate, format);
        if (!stream.fail()) {
            *parsed = candidate;
            return true;
        }
    }
    return false;
}

// 清理并格式化硬件时钟命令输出。
[[maybe_unused]] std::string display_hwclock_output(const std::string& output, bool utc)
{
    const auto cleaned = remove_hwclock_seconds_diagnostic(first_output_line(output));
    if (cleaned.empty()) return {};
    std::tm parsed{};
    if (!parse_hwclock_time(cleaned, &parsed)) return cleaned + (utc ? " UTC" : "");
    std::ostringstream formatted;
    formatted << std::put_time(&parsed, "%Y-%m-%d %H:%M:%S");
    if (utc) formatted << " UTC";
    return formatted.str();
}

#if defined(__linux__)
// 处理失败详情。
std::string process_failure_detail(
    const std::vector<std::string>& arguments,
    const ProcessResult& result)
{
    std::ostringstream detail;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index > 0) detail << ' ';
        detail << arguments[index];
    }
    if (result.timed_out) {
        detail << "：超时";
    } else {
        detail << "：退出码 " << result.exit_code;
    }
    if (!result.output.empty()) detail << "，" << first_output_line(result.output);
    return detail.str();
}
#endif

}  // namespace

LinuxTimeRuntime::LinuxTimeRuntime()
    : LinuxTimeRuntime(default_runtime_paths())
{
}

LinuxTimeRuntime::LinuxTimeRuntime(Paths paths)
    : paths_(std::move(paths))
{
}

// 校验服务端。
StatusCode LinuxTimeRuntime::validate_server(const std::string& raw_server, std::string* error_message)
{
    const auto server = trim(raw_server);
    const bool looks_numeric = !server.empty() &&
        std::all_of(server.begin(), server.end(), [](unsigned char ch) { return std::isdigit(ch) || ch == '.'; });
    if ((!looks_numeric && is_hostname(server)) || (looks_numeric && parse_ipv4(server))) {
        return StatusCode::kOk;
    }
    if (error_message != nullptr) {
        *error_message = "NTP 服务器必须是合法 IPv4 地址或主机名";
    }
    return StatusCode::kInvalidArgument;
}

// 校验输入参数及其业务约束。
StatusCode LinuxTimeRuntime::validate_settings_shape(const TimeSettings& settings, std::string* error_message)
{
    if (settings.timezone.empty() || settings.timezone.size() > 128) {
        if (error_message != nullptr) *error_message = "时区名称不能为空且不能超过 128 字符";
        return StatusCode::kInvalidArgument;
    }
    int server_count = 0;
    for (const auto* server : {&settings.ntp_primary, &settings.ntp_secondary}) {
        if (trim(*server).empty()) continue;
        ++server_count;
        const auto status = validate_server(*server, error_message);
        if (!is_ok(status)) return status;
    }
    if (server_count > 2) {
        if (error_message != nullptr) *error_message = "最多配置两个 NTP 服务器";
        return StatusCode::kInvalidArgument;
    }
    if (settings.ntp_enabled && server_count == 0) {
        if (error_message != nullptr) *error_message = "启用 NTP 时至少填写一个服务器";
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 判断进程命令行是否属于本服务管理的 NTP 实例。
bool LinuxTimeRuntime::is_managed_ntpd_command_line(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::string& expected_pid_file,
    const std::string& expected_config_file)
{
    if (executable_name(executable) != "ntpd") return false;
    bool pid_file_matches = false;
    bool config_file_matches = false;
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == "-p" && arguments[i + 1] == expected_pid_file) pid_file_matches = true;
        if (arguments[i] == "-c" && arguments[i + 1] == expected_config_file) config_file_matches = true;
    }
    return pid_file_matches && config_file_matches;
}

// 校验手动设置的毫秒时间戳。
StatusCode LinuxTimeRuntime::validate_manual_epoch_ms(TimestampMs epoch_ms, std::string* error_message)
{
    constexpr TimestampMs kMin = 1577836800000ULL;  // 2020-01-01T00:00:00Z
    constexpr TimestampMs kMaxExclusive = 4133980800000ULL;  // 2101-01-01T00:00:00Z
    if (epoch_ms < kMin || epoch_ms >= kMaxExclusive) {
        if (error_message != nullptr) *error_message = "手动时间必须在 2020 年至 2100 年范围内";
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 检测系统初始时区。
std::string LinuxTimeRuntime::detect_initial_timezone() const
{
    const auto timezone_text = trim(read_file(paths_.timezone_path));
    if (!timezone_text.empty() && is_ok(validate_timezone(timezone_text, nullptr))) {
        return timezone_text;
    }
    std::error_code ec;
    const auto localtime = edge::fs::path(paths_.localtime_path);
    if (edge::fs::is_symlink(localtime, ec)) {
        auto target = edge::fs::read_symlink(localtime, ec);
        if (target.is_relative()) target = localtime.parent_path() / target;
        const auto relative = edge::fs_relative_path_within(paths_.zoneinfo_directory, target, ec).generic_string();
        if (!ec) {
            if (!relative.empty() && relative != "." && is_ok(validate_timezone(relative, nullptr))) {
                return relative;
            }
        }
    }
    return kDefaultTimezone;
}

// 根据系统运行态生成已应用时间设置快照。
TimeSettings LinuxTimeRuntime::snapshot_applied_settings(const TimeSettings& known_settings) const
{
    auto applied = known_settings;
    applied.timezone = detect_initial_timezone();
    applied.ntp_enabled = managed_ntpd_running();
    return applied;
}

// 校验时区名称和对应系统文件。
StatusCode LinuxTimeRuntime::validate_timezone(const std::string& timezone, std::string* error_message) const
{
    const edge::fs::path relative(timezone);
    if (relative.empty() || relative.is_absolute()) {
        if (error_message != nullptr) *error_message = "时区名称无效";
        return StatusCode::kInvalidArgument;
    }
    for (const auto& part : relative) {
        if (part == ".." || part == "." || part.empty()) {
            if (error_message != nullptr) *error_message = "时区名称包含非法路径片段";
            return StatusCode::kInvalidArgument;
        }
    }
    std::error_code ec;
    const auto root = edge::fs_canonical_existing(paths_.zoneinfo_directory, ec);
    if (ec) {
        if (error_message != nullptr) {
            *error_message = timezone == kDefaultTimezone
                                 ? "板端缺少 Asia/Shanghai 时区文件"
                                 : "时区数据库不可用: " + ec.message();
        }
        return StatusCode::kIoError;
    }
    const auto target = edge::fs_canonical_existing(root / relative, ec);
    if (ec || !edge::fs::is_regular_file(target, ec)) {
        if (error_message != nullptr) {
            *error_message = timezone == kDefaultTimezone
                                 ? "板端缺少 Asia/Shanghai 时区文件"
                                 : "板端不存在该 IANA 时区: " + timezone;
        }
        return StatusCode::kInvalidArgument;
    }
    if (!edge::fs_path_is_within(root, target, ec)) {
        if (error_message != nullptr) *error_message = "时区目标超出 /usr/share/zoneinfo";
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 校验设置。
StatusCode LinuxTimeRuntime::validate_settings(const TimeSettings& settings, std::string* error_message) const
{
    const auto shape = validate_settings_shape(settings, error_message);
    return is_ok(shape) ? validate_timezone(settings.timezone, error_message) : shape;
}

// 应用系统时区设置。
StatusCode LinuxTimeRuntime::apply_timezone(const std::string& timezone, std::string* error_message)
{
    const auto validation = validate_timezone(timezone, error_message);
    if (!is_ok(validation)) return validation;
    const auto localtime = edge::fs::path(paths_.localtime_path);
    const auto timezone_file = edge::fs::path(paths_.timezone_path);
    const auto localtime_snapshot = snapshot_file(localtime);
    const auto timezone_snapshot = snapshot_file(timezone_file);
    const auto temporary_link = edge::fs::path(localtime.string() + ".edge-controller.tmp");
    std::error_code ec;
    edge::fs::remove(temporary_link, ec);
    edge::fs::create_symlink(edge::fs::path(paths_.zoneinfo_directory) / timezone, temporary_link, ec);
    if (!ec) edge::fs::rename(temporary_link, localtime, ec);
    if (ec) {
        edge::fs::remove(temporary_link, ec);
        if (error_message != nullptr) *error_message = "原子更新 /etc/localtime 失败: " + ec.message();
        return StatusCode::kIoError;
    }
    std::string write_error;
    if (!write_atomic_file(timezone_file, timezone + "\n", &write_error)) {
        std::string restore_error;
        restore_file(localtime, localtime_snapshot, &restore_error);
        restore_file(timezone_file, timezone_snapshot, &restore_error);
        if (error_message != nullptr) *error_message = write_error + (restore_error.empty() ? "" : "；恢复失败: " + restore_error);
        return StatusCode::kIoError;
    }
#if defined(__linux__)
    // Application 启动时已固定以 /etc/localtime 为唯一时区来源；这里只刷新 libc 缓存，
    // 避免运行期 setenv 与并发 fork/getenv 争用全局 environ。
    ::tzset();
#endif
    // 时区运行态变化后不继续展示变更前的 RTC 文本。
    invalidate_rtc_cache();
    return StatusCode::kOk;
}

// 返回受管 NTP 进程的 PID 文件路径。
std::string LinuxTimeRuntime::pid_file_path() const { return (edge::fs::path(paths_.runtime_directory) / "ntpd.pid").string(); }
// 返回受管 NTP 进程的配置文件路径。
std::string LinuxTimeRuntime::ntp_config_path() const { return (edge::fs::path(paths_.runtime_directory) / "ntpd.conf").string(); }

// 写入受管 NTP 进程配置。
StatusCode LinuxTimeRuntime::write_ntp_config(const TimeSettings& settings, std::string* error_message) const
{
    std::error_code ec;
    edge::fs::create_directories(paths_.runtime_directory, ec);
    if (ec) {
        if (error_message != nullptr) *error_message = "创建 NTP 运行目录失败: " + ec.message();
        return StatusCode::kIoError;
    }
    std::string content = "driftfile " + (edge::fs::path(paths_.runtime_directory) / "ntp.drift").string() + "\n";
    if (!trim(settings.ntp_primary).empty()) content += "server " + trim(settings.ntp_primary) + " iburst\n";
    if (!trim(settings.ntp_secondary).empty()) content += "server " + trim(settings.ntp_secondary) + " iburst\n";
    return write_atomic_file(ntp_config_path(), content, error_message) ? StatusCode::kOk : StatusCode::kIoError;
}

// 判断受管 NTP 进程是否正在运行，并可返回进程号。
bool LinuxTimeRuntime::managed_ntpd_running(int* output_pid) const
{
#if defined(__linux__)
    const auto text = trim(read_file(pid_file_path()));
    if (text.empty()) return false;
    char* end = nullptr;
    const auto value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value <= 1) return false;
    const auto pid = static_cast<int>(value);
    if (!is_managed_ntpd_command_line(
            proc_executable(pid), proc_arguments(pid), pid_file_path(), ntp_config_path())) return false;
    if (output_pid != nullptr) *output_pid = pid;
    return true;
#else
    (void)output_pid;
    return false;
#endif
}

// 枚举不受本服务管理的外部 NTP 进程。
std::vector<int> LinuxTimeRuntime::external_ntpd_pids() const
{
    std::vector<int> pids;
#if defined(__linux__)
    int managed_pid = 0;
    const bool has_managed_pid = managed_ntpd_running(&managed_pid);
    std::error_code ec;
    for (const auto& item : edge::fs::directory_iterator("/proc", ec)) {
        const auto name = item.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isdigit(ch); })) continue;
        const auto pid = std::atoi(name.c_str());
        const auto executable = proc_executable(pid);
        if (executable_name(executable) != "ntpd") continue;
        // 只有 PID 文件与 /proc 双重核对通过的那个进程才算受管实例；
        // 即使参数相同，PID 文件丢失后的遗留进程也必须按冲突处理，避免重复启动。
        if ((!has_managed_pid || pid != managed_pid) && pid > 1) pids.push_back(pid);
    }
#endif
    std::sort(pids.begin(), pids.end());
    return pids;
}

// 判断是否存在外部 NTP 进程。
bool LinuxTimeRuntime::external_ntpd_running() const
{
    return !external_ntpd_pids().empty();
}

bool LinuxTimeRuntime::external_ntpd_running_for_status() const
{
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(external_ntpd_cache_mutex_);
        if (external_ntpd_cache_initialized_ &&
            now - external_ntpd_cache_checked_at_ < std::chrono::seconds(15)) {
            return external_ntpd_cache_running_;
        }
    }
    const bool running = external_ntpd_running();
    {
        std::lock_guard<std::mutex> lock(external_ntpd_cache_mutex_);
        external_ntpd_cache_initialized_ = true;
        external_ntpd_cache_running_ = running;
        external_ntpd_cache_checked_at_ = now;
    }
    return running;
}

// 停止不受本服务管理的外部 NTP 进程。
StatusCode LinuxTimeRuntime::stop_external_ntpd(
    const std::vector<int>& pids,
    std::string* error_message) const
{
#if defined(__linux__)
    using process_identity_internal::SignalResult;
    for (const auto pid : pids) {
        if (executable_name(proc_executable(pid)) != "ntpd") continue;
        const auto identity = process_identity_internal::capture_process_identity(pid);
        // 捕获 starttime 后再次核对可执行文件，避免 /proc 扫描与信号发送之间发生 PID 复用。
        if (!identity.has_value() ||
            executable_name(proc_executable(pid)) != "ntpd" ||
            !process_identity_internal::same_process_identity(*identity)) {
            continue;
        }
        const auto signal_result = process_identity_internal::signal_same_process(*identity, SIGTERM);
        if (signal_result == SignalResult::kPermissionDenied || signal_result == SignalResult::kError) {
            if (error_message != nullptr) {
                *error_message = "系统已有 ntpd 正在运行，edge-controller 无法接管，请检查系统启动项（PID=" +
                                 std::to_string(pid) + "，停止失败：进程身份校验或信号发送失败）";
            }
            return StatusCode::kIoError;
        }
    }

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (external_ntpd_pids().empty()) return StatusCode::kOk;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto remaining = external_ntpd_pids();
    for (const auto pid : remaining) {
        if (executable_name(proc_executable(pid)) != "ntpd") continue;
        const auto identity = process_identity_internal::capture_process_identity(pid);
        if (!identity.has_value() ||
            executable_name(proc_executable(pid)) != "ntpd" ||
            !process_identity_internal::same_process_identity(*identity)) {
            continue;
        }
        const auto signal_result = process_identity_internal::signal_same_process(*identity, SIGKILL);
        if (signal_result == SignalResult::kPermissionDenied || signal_result == SignalResult::kError) {
            if (error_message != nullptr) {
                *error_message = "强制停止外部 ntpd 失败（PID=" + std::to_string(pid) +
                                 "）：进程身份校验或信号发送失败";
            }
            return StatusCode::kIoError;
        }
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (external_ntpd_pids().empty()) return StatusCode::kOk;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const auto still_running = external_ntpd_pids();
    if (error_message != nullptr) {
        *error_message = "系统已有 ntpd 正在运行，edge-controller 无法接管，请检查系统启动项（PID=" +
                         pid_list_text(still_running) + "）";
    }
    return StatusCode::kIoError;
#else
    (void)pids;
    if (error_message != nullptr) *error_message = "当前平台不支持 Linux ntpd 接管";
    return StatusCode::kInvalidState;
#endif
}

// 停止外部 NTP 进程，将时间同步控制权交给本服务。
StatusCode LinuxTimeRuntime::take_over_external_ntpd(std::string* error_message)
{
    const auto pids = external_ntpd_pids();
    if (pids.empty()) {
        std::lock_guard<std::mutex> state_lock(mutex_);
        ntpd_takeover_failed_ = false;
        return StatusCode::kOk;
    }
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        ntpd_takeover_in_progress_ = true;
        ntpd_takeover_failed_ = false;
    }
    Logger::warn("检测到外部 ntpd，开始由 edge-controller 接管，PID=" + pid_list_text(pids));
    const auto status = stop_external_ntpd(pids, error_message);
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        ntpd_takeover_in_progress_ = false;
        ntpd_takeover_failed_ = !is_ok(status);
    }
    if (is_ok(status)) {
        Logger::info("外部 ntpd 已停止，edge-controller 将启动受管 NTP 实例");
    } else {
        Logger::error(error_message == nullptr || error_message->empty()
                          ? "外部 ntpd 停止失败，edge-controller 无法接管"
                          : *error_message);
    }
    return status;
}

// 启动本服务管理的 NTP 进程。
StatusCode LinuxTimeRuntime::start_managed_ntpd(const TimeSettings& settings, std::string* error_message)
{
    const auto takeover_status = take_over_external_ntpd(error_message);
    if (!is_ok(takeover_status)) return takeover_status;
    const auto verify_exclusive_instance = [this, error_message]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto external_pids = external_ntpd_pids();
        if (external_pids.empty()) return StatusCode::kOk;
        std::string ignored;
        stop_managed_ntpd(&ignored);
        {
            std::lock_guard<std::mutex> state_lock(mutex_);
            ntpd_takeover_failed_ = true;
        }
        if (error_message != nullptr) {
            *error_message = "系统启动项重新启动了外部 ntpd，edge-controller 无法接管并独占管理 NTP，请检查系统启动项（PID=" +
                             pid_list_text(external_pids) + "）";
        }
        return StatusCode::kIoError;
    };
    if (managed_ntpd_running()) return verify_exclusive_instance();
    const auto config_status = write_ntp_config(settings, error_message);
    if (!is_ok(config_status)) return config_status;
#if defined(__linux__)
    const auto result = run_process(
        {paths_.ntpd_path, "-4", "-g", "-p", pid_file_path(), "-c", ntp_config_path()},
        std::chrono::seconds(8));
    if (result.timed_out || result.exit_code != 0) {
        if (error_message != nullptr) *error_message = result.timed_out ? "启动 ntpd 超时" : "启动 ntpd 失败: " + result.output;
        return StatusCode::kIoError;
    }
    for (int i = 0; i < 20; ++i) {
        if (managed_ntpd_running()) return verify_exclusive_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (error_message != nullptr) *error_message = "ntpd 已返回成功，但未发现受管 PID";
    return StatusCode::kIoError;
#else
    if (error_message != nullptr) *error_message = "当前平台不支持 Linux ntpd 管理";
    return StatusCode::kInvalidState;
#endif
}

// 停止本服务管理的 NTP 进程。
StatusCode LinuxTimeRuntime::stop_managed_ntpd(std::string* error_message)
{
#if defined(__linux__)
    using process_identity_internal::SignalResult;
    int pid = 0;
    if (!managed_ntpd_running(&pid)) {
        std::error_code ec;
        edge::fs::remove(pid_file_path(), ec);
        return StatusCode::kOk;
    }
    const auto identity = process_identity_internal::capture_process_identity(pid);
    int verified_pid = 0;
    if (!identity.has_value() ||
        !managed_ntpd_running(&verified_pid) ||
        verified_pid != pid ||
        !process_identity_internal::same_process_identity(*identity)) {
        if (!managed_ntpd_running()) {
            std::error_code ec;
            edge::fs::remove(pid_file_path(), ec);
            return StatusCode::kOk;
        }
        if (error_message != nullptr) {
            *error_message = "停止受管 ntpd 失败：进程身份在校验期间发生变化";
        }
        return StatusCode::kIoError;
    }
    const auto term_result = process_identity_internal::signal_same_process(*identity, SIGTERM);
    if (term_result == SignalResult::kPermissionDenied || term_result == SignalResult::kError) {
        if (error_message != nullptr) {
            *error_message = "停止受管 ntpd 失败：进程身份校验或信号发送失败";
        }
        return StatusCode::kIoError;
    }
    for (int i = 0; i < 50; ++i) {
        if (!process_identity_internal::same_process_identity(*identity)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (process_identity_internal::same_process_identity(*identity)) {
        const auto kill_result = process_identity_internal::signal_same_process(*identity, SIGKILL);
        if (kill_result == SignalResult::kPermissionDenied || kill_result == SignalResult::kError) {
            if (error_message != nullptr) {
                *error_message = "强制停止受管 ntpd 失败：进程身份校验或信号发送失败";
            }
            return StatusCode::kIoError;
        }
        for (int i = 0; i < 10 && process_identity_internal::same_process_identity(*identity); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (process_identity_internal::same_process_identity(*identity)) {
        if (error_message != nullptr) {
            *error_message = "停止受管 ntpd 超时：目标进程在强制终止后仍存在";
        }
        return StatusCode::kIoError;
    }
    // 若守护进程在停止期间又写入了新的受管 PID，保留新 pidfile 并让上层报告独占冲突。
    if (managed_ntpd_running()) {
        if (error_message != nullptr) {
            *error_message = "停止受管 ntpd 后检测到新的受管实例，请检查系统启动项";
        }
        return StatusCode::kIoError;
    }
    std::error_code ec;
    edge::fs::remove(pid_file_path(), ec);
    return StatusCode::kOk;
#else
    (void)error_message;
    return StatusCode::kOk;
#endif
}

// 根据配置协调受管与外部 NTP 进程。
StatusCode LinuxTimeRuntime::reconcile_ntpd(const TimeSettings& settings, std::string* error_message)
{
    if (settings.ntp_enabled) return start_managed_ntpd(settings, error_message);
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        ntpd_takeover_in_progress_ = false;
        ntpd_takeover_failed_ = false;
    }
    return stop_managed_ntpd(error_message);
}

// 应用设置。
StatusCode LinuxTimeRuntime::apply_settings(
    const TimeSettings& settings,
    const TimeSettings& previous_settings,
    std::string* warning_message,
    std::string* error_message)
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (warning_message != nullptr) warning_message->clear();
    const auto validation = validate_settings(settings, error_message);
    if (!is_ok(validation)) return validation;
    const auto timezone_status = apply_timezone(settings.timezone, error_message);
    if (!is_ok(timezone_status)) return timezone_status;
    const bool servers_changed = settings.ntp_enabled && previous_settings.ntp_enabled &&
        (settings.ntp_primary != previous_settings.ntp_primary || settings.ntp_secondary != previous_settings.ntp_secondary);
    if (servers_changed) {
        const auto stop_status = stop_managed_ntpd(error_message);
        if (!is_ok(stop_status)) {
            std::string rollback_error;
            apply_timezone(previous_settings.timezone, &rollback_error);
            {
                std::lock_guard<std::mutex> state_lock(mutex_);
                last_error_message_ = error_message == nullptr ? "重启 ntpd 前停止旧实例失败" : *error_message;
            }
            return stop_status;
        }
    }
    const auto ntp_status = reconcile_ntpd(settings, error_message);
    if (is_ok(ntp_status)) {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_.clear();
        return StatusCode::kOk;
    }
    const auto original_error = error_message == nullptr ? std::string("应用 NTP 配置失败") : *error_message;
    std::string rollback_error;
    apply_timezone(previous_settings.timezone, &rollback_error);
    reconcile_ntpd(previous_settings, &rollback_error);
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_ = original_error;
    }
    if (error_message != nullptr) *error_message = original_error + (rollback_error.empty() ? "" : "；运行态回退失败: " + rollback_error);
    return ntp_status;
}

// 启动时恢复并应用持久化时间设置。
StatusCode LinuxTimeRuntime::restore_on_startup(const TimeSettings& settings, std::string* error_message)
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    const auto validation = validate_settings(settings, error_message);
    if (!is_ok(validation)) return validation;
    const auto timezone_status = apply_timezone(settings.timezone, error_message);
    if (!is_ok(timezone_status)) return timezone_status;
    // 崩溃后遗留的受管实例也要按 SQLite 配置重建，确保服务器列表已生效且仍为单实例。
    if (settings.ntp_enabled && managed_ntpd_running()) {
        const auto stop_status = stop_managed_ntpd(error_message);
        if (!is_ok(stop_status)) return stop_status;
    }
    const auto ntp_status = reconcile_ntpd(settings, error_message);
    if (!is_ok(ntp_status)) {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_ = error_message == nullptr ? "恢复 NTP 运行态失败" : *error_message;
    } else {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_.clear();
    }
    return ntp_status;
}

// 将系统时间写入硬件时钟。
StatusCode LinuxTimeRuntime::write_rtc(std::string* warning_message) const
{
#if defined(__linux__)
    const std::vector<std::vector<std::string>> attempts = {
        {paths_.hwclock_path, "-w", "-u"},
        {paths_.hwclock_path, "-w"},
        {paths_.hwclock_path, "--systohc", "--utc"},
    };
    std::vector<std::string> failures;
    for (const auto& arguments : attempts) {
        const auto result = run_process(arguments, std::chrono::seconds(8));
        if (!result.timed_out && result.exit_code == 0) {
            invalidate_rtc_cache();
            return StatusCode::kOk;
        }
        failures.push_back(process_failure_detail(arguments, result));
        if (result.timed_out || result.exit_code == 127) break;
    }
    invalidate_rtc_cache();
    std::string detail;
    for (const auto& failure : failures) {
        if (!detail.empty()) detail += "；";
        detail += failure;
    }
    Logger::warn("RTC 写入失败：" + detail);
    if (warning_message != nullptr) *warning_message = "系统时间已校准，但写入 RTC 失败，请检查 hwclock 和 RTC 设备";
    return StatusCode::kIoError;
#else
    if (warning_message != nullptr) *warning_message = "当前平台不支持写入 RTC";
    return StatusCode::kInvalidState;
#endif
}

// 读取硬件时钟时间。
std::string LinuxTimeRuntime::read_rtc_time(bool* available) const
{
    // 整个探测过程持有独立缓存锁：并发状态请求只会启动一个 hwclock，
    // 其他系统时间字段不依赖探测成功与否。
    std::lock_guard<std::mutex> cache_lock(rtc_cache_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto cache_interval = rtc_cache_available_
                                    ? paths_.rtc_probe_interval
                                    : paths_.rtc_failure_retry_interval;
    if (rtc_cache_initialized_ && now - rtc_cache_checked_at_ < cache_interval) {
        if (available != nullptr) *available = rtc_cache_available_;
        return rtc_cache_time_text_;
    }

    rtc_cache_initialized_ = true;
    rtc_cache_available_ = false;
    rtc_cache_time_text_.clear();
    rtc_cache_checked_at_ = now;
    if (available != nullptr) *available = false;
    std::error_code ec;
    bool found = false;
    for (const auto& path : paths_.rtc_paths) {
        if (edge::fs::exists(path, ec)) { found = true; break; }
    }
    const std::string device_diagnostic = found
                                              ? std::string{}
                                              : "未发现常用 RTC 设备节点，继续由 hwclock 自动探测；";
#if defined(__linux__)
    struct ReadAttempt {
        std::vector<std::string> arguments;
        bool utc{false};
    };
    const std::vector<ReadAttempt> attempts = {
        {{paths_.hwclock_path, "-r", "-u"}, true},
        {{paths_.hwclock_path, "-r"}, false},
        {{paths_.hwclock_path, "--show", "--utc"}, true},
    };
    std::vector<std::string> failures;
    for (const auto& attempt : attempts) {
        const auto result = run_process(attempt.arguments, std::chrono::seconds(5));
        if (result.exit_code == 0 && !result.timed_out) {
            const auto display_text = display_hwclock_output(result.output, attempt.utc);
            if (!display_text.empty()) {
                rtc_cache_available_ = true;
                rtc_cache_time_text_ = display_text;
                if (available != nullptr) *available = true;
                return rtc_cache_time_text_;
            }
            failures.push_back("hwclock 返回空的时间文本");
        } else {
            failures.push_back(process_failure_detail(attempt.arguments, result));
            if (result.timed_out || result.exit_code == 127) break;
        }
    }
    std::string detail;
    for (const auto& failure : failures) {
        if (!detail.empty()) detail += "；";
        detail += failure;
    }
    Logger::warn("RTC 探测失败，" + device_diagnostic + detail + "；将在 " +
                 std::to_string(paths_.rtc_failure_retry_interval.count()) + " 秒后重试");
#endif
    return {};
}

// 使硬件时钟运行状态缓存失效。
void LinuxTimeRuntime::invalidate_rtc_cache() const
{
    std::lock_guard<std::mutex> cache_lock(rtc_cache_mutex_);
    rtc_cache_initialized_ = false;
    rtc_cache_available_ = false;
    rtc_cache_time_text_.clear();
}

// 获取当前运行状态。
TimeRuntimeStatus LinuxTimeRuntime::runtime_status(const TimeSettings& settings) const
{
    TimeRuntimeStatus status;
    bool synchronizing = false;
    bool takeover_in_progress = false;
    bool takeover_failed = false;
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        status.last_sync_time_ms = last_sync_time_ms_;
        status.last_error_message = last_error_message_;
        synchronizing = synchronizing_;
        takeover_in_progress = ntpd_takeover_in_progress_;
        takeover_failed = ntpd_takeover_failed_;
    }
    status.current_time_ms = time_utils::system_now_ms();
    // 运行态报告当前实际应用时区，配置值由 get_time_settings 单独返回。
    status.timezone = detect_initial_timezone();
    status.ntp_enabled = settings.ntp_enabled;
    const bool managed_running = managed_ntpd_running();
    const bool external_running = external_ntpd_running_for_status();
    status.ntp_process_running = managed_running || external_running;
    if (takeover_in_progress) {
        status.ntp_process_state = "taking_over";
    } else if (external_running &&
               (takeover_failed || status.last_error_message.find("无法接管") != std::string::npos)) {
        status.ntp_process_state = "takeover_failed";
    } else if (external_running) {
        status.ntp_process_state = "external";
    } else if (managed_running) {
        status.ntp_process_state = "managed";
    } else {
        status.ntp_process_state = "stopped";
    }
    status.ntp_process_state_text = process_state_text(status.ntp_process_state);
    const auto seconds = static_cast<std::time_t>(status.current_time_ms / 1000);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &seconds);
#else
    localtime_r(&seconds, &local_tm);
#endif
    std::ostringstream current;
    current << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    status.current_time_text = current.str();
    char offset_buffer[16]{};
    std::string offset;
    if (std::strftime(offset_buffer, sizeof(offset_buffer), "%z", &local_tm) > 0) {
        offset = offset_buffer;
        if (offset.size() == 5) offset.insert(3, ":");
    }
    status.utc_offset_text = offset.empty() ? "UTC" : "UTC" + offset;
    status.rtc_time_text = read_rtc_time(&status.rtc_available);
    if (!settings.ntp_enabled) {
        status.sync_state = "disabled";
    } else if (takeover_in_progress) {
        status.sync_state = "taking_over";
    } else if (external_running) {
        status.sync_state = status.ntp_process_state == "takeover_failed" ? "takeover_failed" : "external";
    } else if (synchronizing) {
        status.sync_state = "synchronizing";
    } else if (!status.last_error_message.empty() || !status.ntp_process_running) {
        status.sync_state = "error";
    } else {
#if defined(__linux__)
        timex tx{};
        const auto state = ::adjtimex(&tx);
        status.sync_state = (state != TIME_ERROR && (tx.status & STA_UNSYNC) == 0) ? "synchronized" : "unsynchronized";
#else
        status.sync_state = "unsynchronized";
#endif
    }
    status.sync_state_text = sync_state_text(status.sync_state);
    return status;
}

// 立即执行一次网络时间同步。
StatusCode LinuxTimeRuntime::sync_now(
    const TimeSettings& settings,
    TimeSyncResult* result,
    std::chrono::seconds timeout,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) *error_message = "缺少立即同步结果输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    *result = {};
    if (!settings.ntp_enabled) {
        if (error_message != nullptr) *error_message = "NTP 未启用，不能执行立即同步";
        return StatusCode::kInvalidState;
    }
    const auto validation = validate_settings(settings, error_message);
    if (!is_ok(validation)) return validation;
    const auto takeover_status = take_over_external_ntpd(error_message);
    if (!is_ok(takeover_status)) {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_ = error_message == nullptr
                                  ? "系统已有 ntpd 正在运行，edge-controller 无法接管，请检查系统启动项"
                                  : *error_message;
        return takeover_status;
    }
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        synchronizing_ = true;
    }
    std::string stop_error;
    const auto stop_status = stop_managed_ntpd(&stop_error);
    if (!is_ok(stop_status)) {
        {
            std::lock_guard<std::mutex> state_lock(mutex_);
            synchronizing_ = false;
            last_error_message_ = stop_error;
        }
        if (error_message != nullptr) *error_message = stop_error;
        return stop_status;
    }
    const auto config_status = write_ntp_config(settings, error_message);
    if (!is_ok(config_status)) {
        {
            std::lock_guard<std::mutex> state_lock(mutex_);
            synchronizing_ = false;
            last_error_message_ = error_message == nullptr ? "写入 NTP 运行配置失败" : *error_message;
        }
        std::string restart_error;
        start_managed_ntpd(settings, &restart_error);
        return config_status;
    }
#if defined(__linux__)
    const auto process = run_process(
        {paths_.ntpd_path, "-4", "-g", "-q", "-p", pid_file_path(), "-c", ntp_config_path()}, timeout);
    StatusCode sync_status = StatusCode::kOk;
    std::string sync_error;
    if (process.timed_out) {
        sync_status = StatusCode::kIoError;
        sync_error = "NTP 立即同步超时，已终止校时进程";
    } else if (process.exit_code != 0) {
        sync_status = StatusCode::kIoError;
        sync_error = "NTP 立即同步失败" + (process.output.empty() ? std::string{} : ": " + process.output);
    }
#else
    (void)timeout;
    StatusCode sync_status = StatusCode::kInvalidState;
    std::string sync_error = "当前平台不支持 Linux NTP 同步";
#endif
    std::string restart_error;
    const auto restart_status = start_managed_ntpd(settings, &restart_error);
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        synchronizing_ = false;
    }
    if (!is_ok(sync_status)) {
        {
            std::lock_guard<std::mutex> state_lock(mutex_);
            last_error_message_ = sync_error;
        }
        if (!restart_error.empty()) sync_error += "；恢复后台 ntpd 失败: " + restart_error;
        if (error_message != nullptr) *error_message = sync_error;
        return sync_status;
    }
    const auto synchronized_at = time_utils::system_now_ms();
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_sync_time_ms_ = synchronized_at;
        last_error_message_.clear();
    }
    result->synchronized = true;
    result->sync_time_ms = synchronized_at;
    result->message = "系统时间同步成功";
    result->rtc_written = is_ok(write_rtc(&result->warning_message));
    if (!is_ok(restart_status)) {
        if (!result->warning_message.empty()) result->warning_message += "；";
        result->warning_message += "时间已同步，但恢复后台 ntpd 失败: " + restart_error;
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_ = "恢复后台 ntpd 失败: " + restart_error;
    }
    return StatusCode::kOk;
}

// 设置系统与硬件时钟并返回校时结果。
StatusCode LinuxTimeRuntime::set_manual_time(
    const TimeSettings& settings,
    TimestampMs epoch_ms,
    ManualTimeSetResult* result,
    std::string* error_message)
{
    if (result == nullptr) {
        if (error_message != nullptr) *error_message = "缺少手动校时结果输出参数";
        return StatusCode::kInvalidArgument;
    }
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    *result = {};
    if (settings.ntp_enabled) {
        if (error_message != nullptr) *error_message = "请先关闭 NTP，再手动设置系统时间";
        return StatusCode::kInvalidState;
    }
    const auto validation = validate_manual_epoch_ms(epoch_ms, error_message);
    if (!is_ok(validation)) return validation;
    result->previous_time_ms = time_utils::system_now_ms();
#if defined(__linux__)
    timespec target{};
    target.tv_sec = static_cast<time_t>(epoch_ms / 1000);
    target.tv_nsec = static_cast<long>((epoch_ms % 1000) * 1000000ULL);
    if (::clock_settime(CLOCK_REALTIME, &target) != 0) {
        if (error_message != nullptr) *error_message = "设置系统时间失败: " + std::string(std::strerror(errno));
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_ = error_message == nullptr ? "设置系统时间失败" : *error_message;
        return StatusCode::kIoError;
    }
#else
    if (error_message != nullptr) *error_message = "当前平台不支持 clock_settime";
    return StatusCode::kInvalidState;
#endif
    result->time_set = true;
    result->current_time_ms = epoch_ms;
    result->message = "系统时间设置成功";
    result->rtc_written = is_ok(write_rtc(&result->warning_message));
    {
        std::lock_guard<std::mutex> state_lock(mutex_);
        last_error_message_.clear();
    }
    return StatusCode::kOk;
}

// 关闭。
void LinuxTimeRuntime::shutdown()
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    std::string ignored;
    stop_managed_ntpd(&ignored);
}

}  // namespace edge_controller
