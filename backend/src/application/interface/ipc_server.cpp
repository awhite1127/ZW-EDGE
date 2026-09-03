// Unix Socket IPC 服务：接受连接、限制并发和帧大小，再将已解析请求分派给业务 handler。
#include "application/interface/ipc_server.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "shared/common/filesystem_compat.h"
#include "shared/common/logger.h"
#include "application/interface/ipc_handlers.h"
#include "application/interface/ipc_json.h"
#include "application/interface/ipc_protocol.h"
#include "application/service/backend_service.h"

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace edge_controller {

namespace {


// 生成系统错误信息显示文本。
[[maybe_unused]] std::string system_error_message(const std::string& stage)
{
    return stage + "，errno=" + std::to_string(errno) + "，原因=" + std::strerror(errno);
}

constexpr int kMaxConcurrentIpcClients = 8;
constexpr int kIpcWorkerCount = 4;
constexpr auto kIpcFrameReadTimeout = std::chrono::seconds(5);
constexpr auto kSlowIpcRequestThreshold = std::chrono::milliseconds(200);
constexpr auto kSlowIpcLogWindow = std::chrono::minutes(15);
constexpr auto kSlowIpcLogEntryTtl = std::chrono::hours(2);
constexpr auto kMaxListenerRecoveryBackoff = std::chrono::milliseconds(1600);
constexpr std::size_t kMaxSlowIpcLogEntries = 256;
constexpr std::size_t kMaxSlowIpcMethodLength = 128;

// 截断过长日志文本，避免单条记录失控。
[[maybe_unused]] std::string truncate_log_text(const std::string& value)
{
    constexpr std::size_t kMaxLength = 160;
    if (value.size() <= kMaxLength) {
        return value;
    }
    return value.substr(0, kMaxLength) + "...";
}

// 按名称查找 JSON 对象字段。
const nlohmann::json* json_field(const nlohmann::json& object, const std::string& field)
{
    if (!object.is_object()) {
        return nullptr;
    }
    const auto iterator = object.find(field);
    return iterator == object.end() ? nullptr : &*iterator;
}

// 读取供日志记录的 JSON 字符串字段。
std::string json_string_field_for_log(const nlohmann::json& root, const std::string& field)
{
    const auto* value = json_field(root, field);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return value->get<std::string>();
}

struct IpcErrorLogInfo {
    std::string code;
    std::string message;
};

// 生成有界的慢日志 method；超长方法增加散列后缀，避免限频键占用大块内存。
[[maybe_unused]] std::string bounded_ipc_method(const std::string& method)
{
    const auto normalized = method.empty() ? std::string("unknown") : method;
    if (normalized.size() <= kMaxSlowIpcMethodLength) {
        return normalized;
    }
    return normalized.substr(0, kMaxSlowIpcMethodLength) +
           "#" + std::to_string(std::hash<std::string>{}(normalized));
}

// IPC 响应统一由 ipc_protocol 构造；错误响应的规范化首字段为 error，可据此低成本分类。
[[maybe_unused]] bool ipc_response_succeeded_for_log(const std::string& response_json)
{
    return response_json.compare(0, std::strlen("{\"error\":"), "{\"error\":") != 0;
}

// 仅在慢日志确定输出后解析响应，提取稳定错误码与经过截断的错误说明。
[[maybe_unused]] IpcErrorLogInfo ipc_error_for_log(const std::string& response_json)
{
    IpcErrorLogInfo info;
    const auto root = nlohmann::json::parse(response_json, nullptr, false);
    if (!root.is_object()) {
        info.code = "invalid_response";
        info.message = "后端响应格式无效";
        return info;
    }
    const auto* success = json_field(root, "success");
    if (success == nullptr || !success->is_boolean() || success->get<bool>()) {
        return info;
    }
    const auto* error = json_field(root, "error");
    if (error == nullptr || !error->is_object()) {
        info.code = "unknown_error";
        info.message = "后端返回失败";
        return info;
    }
    info.code = json_string_field_for_log(*error, "code");
    info.message = json_string_field_for_log(*error, "message");
    if (info.code.empty()) {
        info.code = "unknown_error";
    }
    if (info.message.empty()) {
        info.message = "后端返回失败";
    }
    return info;
}

#if defined(__linux__)
constexpr int kClientSocketTimeoutSeconds = 5;
constexpr int kListenPollTimeoutMilliseconds = 200;
constexpr mode_t kIpcSocketMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
constexpr mode_t kIpcSocketBindUmask = S_IXUSR | S_IXGRP | S_IRWXO;

class ScopedUmask {
public:
    // 构造 ScopedUmask 实例。
    explicit ScopedUmask(mode_t mask)
        : previous_(::umask(mask))
    {
    }

    // 构造 ScopedUmask 实例。
    ScopedUmask(const ScopedUmask&) = delete;
    // 移动赋值对象并转移其资源所有权。
    ScopedUmask& operator=(const ScopedUmask&) = delete;

    // 销毁 ScopedUmask 实例并释放相关资源。
    ~ScopedUmask()
    {
        ::umask(previous_);
    }

private:
    mode_t previous_{0};
};

class ScopedFd {
public:
    explicit ScopedFd(int fd)
        : fd_(fd)
    {
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ~ScopedFd()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    int get() const { return fd_; }

    int release()
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_{-1};
};

// 生成套接字模式显示文本。
std::string socket_mode_text(mode_t mode)
{
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "%04o", static_cast<unsigned int>(mode & 0777));
    return buffer;
}

// 解析输入并写入结构化结果。
bool parse_gid_value(const char* raw_value, gid_t* gid)
{
    if (raw_value == nullptr || raw_value[0] == '\0' || gid == nullptr) {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(raw_value, &end, 10);
    if (errno != 0 || end == raw_value || *end != '\0') {
        return false;
    }

    *gid = static_cast<gid_t>(parsed);
    return true;
}

// 解析 IPC 套接字应归属的系统用户组。
bool resolve_ipc_socket_group(gid_t* gid, std::string* group_label)
{
    const auto* configured_group = std::getenv("EDGE_IPC_SOCKET_GROUP");
    if (configured_group != nullptr && configured_group[0] != '\0') {
        errno = 0;
        const auto* group = ::getgrnam(configured_group);
        if (group == nullptr) {
            const std::string reason = errno == 0 ? "用户组不存在" : std::strerror(errno);
            Logger::warn(
                "IPC socket 用户组设置失败：EDGE_IPC_SOCKET_GROUP=" + std::string(configured_group) +
                "，原因=" + reason + "。socket 已创建，但权限可能影响 Web 连接。");
            return false;
        }
        if (gid != nullptr) {
            *gid = group->gr_gid;
        }
        if (group_label != nullptr) {
            *group_label = std::string(configured_group) + "(gid=" + std::to_string(group->gr_gid) + ")";
        }
        return true;
    }

    const auto* sudo_gid = std::getenv("SUDO_GID");
    if (sudo_gid == nullptr || sudo_gid[0] == '\0') {
        return false;
    }

    gid_t parsed_gid = 0;
    if (!parse_gid_value(sudo_gid, &parsed_gid)) {
        Logger::warn(
            "IPC socket 用户组回退失败：SUDO_GID=" + std::string(sudo_gid) +
            " 解析失败。socket 已创建，但权限可能影响普通用户 Web 连接。");
        return false;
    }

    if (gid != nullptr) {
        *gid = parsed_gid;
    }
    if (group_label != nullptr) {
        *group_label = "SUDO_GID=" + std::string(sudo_gid);
    }
    return true;
}

// 设置 IPC 套接字的属组与访问权限。
void configure_ipc_socket_permissions(const std::string& socket_path)
{
    gid_t target_gid = 0;
    std::string group_label;
    std::string applied_group_label;
    if (resolve_ipc_socket_group(&target_gid, &group_label)) {
        if (::chown(socket_path.c_str(), static_cast<uid_t>(-1), target_gid) < 0) {
            Logger::warn(
                "设置 IPC socket 所属组失败：路径=" + socket_path + "，目标组=" + group_label +
                "，" + system_error_message("chown 失败") + "。socket 已创建，但权限可能影响 Web 连接。");
        } else {
            applied_group_label = group_label;
            Logger::info("IPC socket 所属组已设置：路径=" + socket_path + "，目标组=" + group_label);
        }
    }

    if (::chmod(socket_path.c_str(), kIpcSocketMode) < 0) {
        Logger::warn(
            "设置 IPC socket 权限失败：路径=" + socket_path + "，目标权限=" + socket_mode_text(kIpcSocketMode) +
            "，" + system_error_message("chmod 失败") + "。socket 已创建，但权限可能影响 Web 连接。");
        return;
    }

    Logger::info(
        "IPC socket 权限已设置：路径=" + socket_path + "，权限=" + socket_mode_text(kIpcSocketMode) +
        (applied_group_label.empty() ? "" : "，所属组=" + applied_group_label));
}

// 设置套接字超时。
StatusCode set_socket_timeout(int fd, int timeout_seconds, std::string* error_message)
{
    timeval timeout{};
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;

    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("设置 IPC 接收超时失败");
        }
        return StatusCode::kIoError;
    }

    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("设置 IPC 发送超时失败");
        }
        return StatusCode::kIoError;
    }

    return StatusCode::kOk;
}

// 读取精确数据。
StatusCode read_exact(
    int fd,
    std::uint8_t* buffer,
    std::size_t length,
    const std::string& stage,
    const std::chrono::steady_clock::time_point deadline,
    std::string* error_message)
{
    std::size_t received = 0;
    while (received < length) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (error_message != nullptr) {
                *error_message = stage + "：整帧读取超时";
            }
            return StatusCode::kTimeout;
        }
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining_ms <= 0) remaining_ms = 1;
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const auto poll_result = ::poll(&descriptor, 1, static_cast<int>(remaining_ms));
        if (poll_result == 0) {
            if (error_message != nullptr) {
                *error_message = stage + "：整帧读取超时";
            }
            return StatusCode::kTimeout;
        }
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            if (error_message != nullptr) {
                *error_message = system_error_message(stage + "：等待可读事件失败");
            }
            return StatusCode::kIoError;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            if (error_message != nullptr) {
                *error_message = stage + "：socket 状态异常";
            }
            return StatusCode::kIoError;
        }
        // UDS 读可能短读，必须循环直到读满协议帧或明确失败。
        const auto read_size = ::read(fd, buffer + received, length - received);
        if (read_size == 0) {
            if (error_message != nullptr) {
                *error_message = stage + "：对端已关闭连接";
            }
            return StatusCode::kIoError;
        }
        if (read_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (error_message != nullptr) {
                    *error_message = stage + "：超时";
                }
                return StatusCode::kTimeout;
            }
            if (error_message != nullptr) {
                *error_message = system_error_message(stage);
            }
            return StatusCode::kIoError;
        }
        received += static_cast<std::size_t>(read_size);
    }

    return StatusCode::kOk;
}

// 写入精确数据。
StatusCode write_exact(int fd, const std::uint8_t* buffer, std::size_t length, const std::string& stage, std::string* error_message)
{
    std::size_t sent = 0;
    while (sent < length) {
        // send 同样可能短写；MSG_NOSIGNAL 避免客户端断开时把进程打断。
        const auto write_size = ::send(fd, buffer + sent, length - sent, MSG_NOSIGNAL);
        if (write_size == 0) {
            if (error_message != nullptr) {
                *error_message = stage + "：对端已关闭连接";
            }
            return StatusCode::kIoError;
        }
        if (write_size < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (error_message != nullptr) {
                    *error_message = stage + "：超时";
                }
                return StatusCode::kTimeout;
            }
            if (error_message != nullptr) {
                *error_message = system_error_message(stage);
            }
            return StatusCode::kIoError;
        }
        sent += static_cast<std::size_t>(write_size);
    }

    return StatusCode::kOk;
}
#endif

}  // namespace

// 构造 BackendIpcServer 实例。
BackendIpcServer::BackendIpcServer(BackendService* backend_service, std::string socket_path)
    : backend_service_(backend_service),
      socket_path_(std::move(socket_path))
{
}

// 销毁 BackendIpcServer 实例并释放相关资源。
BackendIpcServer::~BackendIpcServer()
{
    stop();
}

// 启动 IPC 监听线程和客户端 worker。
StatusCode BackendIpcServer::start()
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const auto current_state = state_.load();
    if (current_state == BackendIpcServerState::kStarting ||
        current_state == BackendIpcServerState::kRunning ||
        current_state == BackendIpcServerState::kRecovering) {
        return StatusCode::kOk;
    }
    if (backend_service_ == nullptr) {
        Logger::error("IPC 服务缺少后端服务实例");
        return StatusCode::kInvalidState;
    }

#if !defined(__linux__)
    Logger::error("IPC 服务仅支持 Linux Unix Domain Socket");
    return StatusCode::kInvalidState;
#else
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_completed_ = false;
        start_status_ = StatusCode::kInvalidState;
        start_error_message_.clear();
        terminal_status_ = StatusCode::kOk;
        terminal_error_message_.clear();
    }

    stop_requested_.store(false);
    running_.store(false);
    state_.store(BackendIpcServerState::kStarting);
    {
        std::lock_guard<std::mutex> lock(slow_ipc_log_mutex_);
        slow_ipc_logs_.clear();
    }

    try {
        // server_loop 负责 bind/listen，start() 会等待它完成启动结果，避免调用方误判 IPC 已就绪。
        worker_ = std::thread(&BackendIpcServer::server_loop, this);
    } catch (const std::system_error& error) {
        Logger::error("创建 IPC 服务线程失败：" + std::string(error.what()));
        state_.store(BackendIpcServerState::kFailed);
        return StatusCode::kInternalError;
    }

    StatusCode start_status = StatusCode::kInternalError;
    std::string start_error_message;
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        start_cv_.wait(lock, [this]() { return start_completed_; });
        start_status = start_status_;
        start_error_message = start_error_message_;
    }

    if (!is_ok(start_status)) {
        stop_requested_.store(true);
        close_listen_socket();
        shutdown_all_client_sockets();
        if (worker_.joinable()) {
            worker_.join();
        }
        remove_socket_file();
        release_instance_lock();
        state_.store(BackendIpcServerState::kFailed);
        if (!start_error_message.empty()) {
            Logger::error("IPC 服务启动失败：" + start_error_message);
        }
        return start_status;
    }

    return StatusCode::kOk;
#endif
}

// 停止 IPC 服务并关闭所有连接。
void BackendIpcServer::stop()
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const auto current_state = state_.load();
    if (current_state == BackendIpcServerState::kStopped && !worker_.joinable()) {
        return;
    }
    state_.store(BackendIpcServerState::kStopping);
    stop_requested_.store(true);
    start_cv_.notify_all();
    shutdown_listen_socket();
    shutdown_all_client_sockets();

    if (worker_.joinable()) {
        worker_.join();
    }

    close_listen_socket();

#if defined(__linux__)
    remove_socket_file();
    release_instance_lock();
#endif

    running_.store(false);
    state_.store(BackendIpcServerState::kStopped);
}

// 判断 IPC 服务是否正在运行。
bool BackendIpcServer::is_running() const
{
    return running_.load();
}

// 返回 IPC 监听生命周期状态。
BackendIpcServerState BackendIpcServer::state() const
{
    return state_.load();
}

// 返回 IPC 监听终态错误。
StatusCode BackendIpcServer::terminal_status(std::string* error_message) const
{
    if (state_.load() != BackendIpcServerState::kFailed) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return StatusCode::kOk;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (error_message != nullptr) {
        *error_message = terminal_error_message_;
    }
    return terminal_status_;
}

// 返回 IPC socket 路径。
const std::string& BackendIpcServer::socket_path() const
{
    return socket_path_;
}

// 通过真实 UDS 请求/响应探测 IPC 服务，避免仅凭路径存在误判就绪。
StatusCode BackendIpcServer::probe(
    const std::string& socket_path,
    std::chrono::milliseconds timeout,
    std::string* error_message)
{
#if !defined(__linux__)
    (void)socket_path;
    (void)timeout;
    if (error_message != nullptr) {
        *error_message = "当前平台不支持 IPC health 探测";
    }
    return StatusCode::kInvalidState;
#else
    sockaddr_un address{};
    if (socket_path.empty() || socket_path.size() >= sizeof(address.sun_path)) {
        if (error_message != nullptr) {
            *error_message = "IPC health 探测路径为空或过长";
        }
        return StatusCode::kInvalidArgument;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds(1500);
    }

    const ScopedFd socket_fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (socket_fd.get() < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("创建 IPC health 探测 socket 失败");
        }
        return StatusCode::kIoError;
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const int original_flags = ::fcntl(socket_fd.get(), F_GETFL, 0);
    if (original_flags < 0 || ::fcntl(socket_fd.get(), F_SETFL, original_flags | O_NONBLOCK) < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("设置 IPC health 探测 socket 非阻塞失败");
        }
        return StatusCode::kIoError;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (::connect(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS) {
            if (error_message != nullptr) {
                *error_message = system_error_message("连接 IPC health 探测 socket 失败");
            }
            return StatusCode::kIoError;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptor{};
        descriptor.fd = socket_fd.get();
        descriptor.events = POLLOUT;
        const int poll_timeout = static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
        const int poll_result = ::poll(&descriptor, 1, poll_timeout);
        if (poll_result == 0) {
            if (error_message != nullptr) {
                *error_message = "连接 IPC health 探测 socket 超时";
            }
            return StatusCode::kTimeout;
        }
        if (poll_result < 0) {
            if (error_message != nullptr) {
                *error_message = system_error_message("等待 IPC health 探测连接失败");
            }
            return StatusCode::kIoError;
        }
        int socket_error = 0;
        socklen_t socket_error_length = sizeof(socket_error);
        if (::getsockopt(
                socket_fd.get(),
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &socket_error_length) < 0 ||
            socket_error != 0) {
            if (socket_error != 0) errno = socket_error;
            if (error_message != nullptr) {
                *error_message = system_error_message("完成 IPC health 探测连接失败");
            }
            return StatusCode::kIoError;
        }
    }

    if (::fcntl(socket_fd.get(), F_SETFL, original_flags) < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("恢复 IPC health 探测 socket 阻塞模式失败");
        }
        return StatusCode::kIoError;
    }
    const auto timeout_seconds = static_cast<int>(std::max<std::int64_t>(
        1,
        (timeout.count() + 999) / 1000));
    if (!is_ok(set_socket_timeout(socket_fd.get(), timeout_seconds, error_message))) {
        return StatusCode::kIoError;
    }

    const auto request = nlohmann::json{
        {"id", "edge-init-health"},
        {"method", "health_check"},
        {"params", nlohmann::json::object()},
    }.dump();
    const auto request_length = static_cast<std::uint32_t>(request.size());
    if (!is_ok(ipc_protocol::validate_payload_length(request_length, error_message))) {
        return StatusCode::kProtocolError;
    }
    const auto request_length_be = htonl(request_length);
    auto status = write_exact(
        socket_fd.get(),
        reinterpret_cast<const std::uint8_t*>(&request_length_be),
        sizeof(request_length_be),
        "写入 IPC health 探测头失败",
        error_message);
    if (!is_ok(status)) return status;
    status = write_exact(
        socket_fd.get(),
        reinterpret_cast<const std::uint8_t*>(request.data()),
        request.size(),
        "写入 IPC health 探测负载失败",
        error_message);
    if (!is_ok(status)) return status;

    std::uint32_t response_length_be = 0;
    status = read_exact(
        socket_fd.get(),
        reinterpret_cast<std::uint8_t*>(&response_length_be),
        sizeof(response_length_be),
        "读取 IPC health 探测头失败",
        deadline,
        error_message);
    if (!is_ok(status)) return status;
    const auto response_length = ntohl(response_length_be);
    status = ipc_protocol::validate_payload_length(response_length, error_message);
    if (!is_ok(status)) return status;
    std::string response_payload(response_length, '\0');
    status = read_exact(
        socket_fd.get(),
        reinterpret_cast<std::uint8_t*>(response_payload.data()),
        response_payload.size(),
        "读取 IPC health 探测负载失败",
        deadline,
        error_message);
    if (!is_ok(status)) return status;

    const auto response = nlohmann::json::parse(response_payload, nullptr, false);
    const auto* response_id = json_field(response, "id");
    const auto* success = json_field(response, "success");
    const auto* result = json_field(response, "result");
    const auto* ready = result == nullptr ? nullptr : json_field(*result, "ready");
    if (!response.is_object() || response_id == nullptr || !response_id->is_string() ||
        response_id->get<std::string>() != "edge-init-health" ||
        success == nullptr || !success->is_boolean() || !success->get<bool>() ||
        ready == nullptr || !ready->is_boolean() || !ready->get<bool>()) {
        if (error_message != nullptr) {
            *error_message = "IPC health 探测响应契约无效";
        }
        return StatusCode::kProtocolError;
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return StatusCode::kOk;
#endif
}

// 配置监听终态失败通知。
void BackendIpcServer::set_terminal_failure_callback(TerminalFailureCallback callback)
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const auto current_state = state_.load();
    if (current_state == BackendIpcServerState::kStarting ||
        current_state == BackendIpcServerState::kRunning ||
        current_state == BackendIpcServerState::kRecovering ||
        current_state == BackendIpcServerState::kStopping) {
        Logger::warn("IPC 服务运行期间拒绝修改终态失败回调");
        return;
    }
    terminal_failure_callback_ = std::move(callback);
}

// 配置监听故障注入。
void BackendIpcServer::set_listener_fault_injector_for_test(ListenerFaultInjector injector)
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const auto current_state = state_.load();
    if (current_state == BackendIpcServerState::kStarting ||
        current_state == BackendIpcServerState::kRunning ||
        current_state == BackendIpcServerState::kRecovering ||
        current_state == BackendIpcServerState::kStopping) {
        Logger::warn("IPC 服务运行期间拒绝修改监听故障注入器");
        return;
    }
    listener_fault_injector_ = std::move(injector);
}

// 配置测试使用的监听恢复策略。
void BackendIpcServer::set_listener_recovery_policy_for_test(
    std::uint32_t max_consecutive_failures,
    std::chrono::milliseconds base_backoff)
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    const auto current_state = state_.load();
    if (current_state == BackendIpcServerState::kStarting ||
        current_state == BackendIpcServerState::kRunning ||
        current_state == BackendIpcServerState::kRecovering ||
        current_state == BackendIpcServerState::kStopping) {
        Logger::warn("IPC 服务运行期间拒绝修改监听恢复策略");
        return;
    }
    max_consecutive_listener_failures_ = std::max<std::uint32_t>(1, max_consecutive_failures);
    listener_recovery_base_backoff_ = std::max(std::chrono::milliseconds(1), base_backoff);
}

// 创建、绑定并发布监听 socket。
StatusCode BackendIpcServer::open_listen_socket(std::string* error_message)
{
#if !defined(__linux__)
    if (error_message != nullptr) *error_message = "当前平台不支持 IPC 服务";
    return StatusCode::kInvalidState;
#else
    sockaddr_un address{};
    if (socket_path_.empty() || socket_path_.size() >= sizeof(address.sun_path)) {
        if (error_message != nullptr) {
            *error_message =
                "IPC socket 路径为空或过长：长度=" + std::to_string(socket_path_.size()) +
                "，最大允许=" + std::to_string(sizeof(address.sun_path) - 1);
        }
        return StatusCode::kInvalidArgument;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
    const edge::fs::path socket_path(socket_path_);
    if (socket_path.has_parent_path()) {
        std::error_code ec;
        edge::fs::create_directories(socket_path.parent_path(), ec);
        if (ec) {
            if (error_message != nullptr) {
                *error_message =
                    "创建 IPC 套接字目录失败：" + socket_path.parent_path().string() + "，原因=" + ec.message();
            }
            return StatusCode::kIoError;
        }
    }

    const auto lock_status = acquire_instance_lock(error_message);
    if (!is_ok(lock_status)) {
        return lock_status;
    }

    struct stat existing_path{};
    if (::lstat(socket_path_.c_str(), &existing_path) == 0) {
        if (!S_ISSOCK(existing_path.st_mode)) {
            if (error_message != nullptr) {
                *error_message = "IPC 路径已存在且不是 socket，拒绝覆盖：" + socket_path_;
            }
            return StatusCode::kInvalidState;
        }

        // 只清理由内核明确判定为无监听者的陈旧 UDS；不得让第二个进程 unlink 正在服务的实例。
        const ScopedFd existing_socket_probe(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (existing_socket_probe.get() < 0) {
            if (error_message != nullptr) {
                *error_message = system_error_message("创建现有 IPC socket 探测连接失败");
            }
            return StatusCode::kIoError;
        }
        const int probe_flags = ::fcntl(existing_socket_probe.get(), F_GETFL, 0);
        if (probe_flags < 0 ||
            ::fcntl(existing_socket_probe.get(), F_SETFL, probe_flags | O_NONBLOCK) < 0) {
            if (error_message != nullptr) {
                *error_message = system_error_message("设置现有 IPC socket 探测连接失败");
            }
            return StatusCode::kIoError;
        }

        int connect_error = 0;
        if (::connect(
                existing_socket_probe.get(),
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0) {
            connect_error = 0;
        } else {
            connect_error = errno;
            if (connect_error == EINPROGRESS) {
                pollfd descriptor{};
                descriptor.fd = existing_socket_probe.get();
                descriptor.events = POLLOUT;
                const int poll_result = ::poll(&descriptor, 1, 200);
                if (poll_result > 0) {
                    socklen_t error_length = sizeof(connect_error);
                    if (::getsockopt(
                            existing_socket_probe.get(),
                            SOL_SOCKET,
                            SO_ERROR,
                            &connect_error,
                            &error_length) < 0) {
                        connect_error = errno;
                    }
                } else if (poll_result == 0) {
                    connect_error = ETIMEDOUT;
                } else {
                    connect_error = errno;
                }
            }
        }
        if (connect_error == 0 || (connect_error != ECONNREFUSED && connect_error != ENOENT)) {
            if (error_message != nullptr) {
                *error_message =
                    connect_error == 0
                        ? "IPC socket 已有活动监听者，拒绝并发启动：" + socket_path_
                        : "无法安全确认现有 IPC socket 已陈旧，拒绝删除：" + socket_path_ +
                              "，errno=" + std::to_string(connect_error) +
                              "，原因=" + std::strerror(connect_error);
            }
            return StatusCode::kInvalidState;
        }

        // 探测与删除之间再次比对 inode，避免路径被另一个进程替换后误删新监听者。
        struct stat current_path{};
        if (::lstat(socket_path_.c_str(), &current_path) == 0) {
            if (current_path.st_dev != existing_path.st_dev || current_path.st_ino != existing_path.st_ino) {
                if (error_message != nullptr) {
                    *error_message = "IPC socket 在陈旧探测期间已被替换，拒绝删除：" + socket_path_;
                }
                return StatusCode::kInvalidState;
            }
            if (::unlink(socket_path_.c_str()) < 0) {
                if (error_message != nullptr) {
                    *error_message = system_error_message("删除陈旧 IPC 套接字失败");
                }
                return StatusCode::kIoError;
            }
        } else if (errno != ENOENT) {
            if (error_message != nullptr) {
                *error_message = system_error_message("复核陈旧 IPC 套接字失败");
            }
            return StatusCode::kIoError;
        }
    } else if (errno != ENOENT) {
        if (error_message != nullptr) {
            *error_message = system_error_message("检查 IPC 套接字路径失败");
        }
        return StatusCode::kIoError;
    }

    int listen_fd = -1;
    const int socket_error = injected_listener_error("socket");
    if (socket_error != 0) {
        errno = socket_error;
    } else {
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    }
    if (listen_fd < 0) {
        if (error_message != nullptr) *error_message = system_error_message("创建 IPC 套接字失败");
        return StatusCode::kIoError;
    }
    ScopedFd pending_listen_fd(listen_fd);

    int bind_result = -1;
    const int bind_error = injected_listener_error("bind");
    if (bind_error != 0) {
        errno = bind_error;
    } else {
        ScopedUmask socket_umask(kIpcSocketBindUmask);
        bind_result = ::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    if (bind_result < 0) {
        if (error_message != nullptr) *error_message = system_error_message("绑定 IPC 套接字失败");
        return StatusCode::kIoError;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        socket_file_owned_ = true;
    }
    configure_ipc_socket_permissions(socket_path_);

    int listen_result = -1;
    const int listen_error = injected_listener_error("listen");
    if (listen_error != 0) {
        errno = listen_error;
    } else {
        listen_result = ::listen(listen_fd, 16);
    }
    if (listen_result < 0) {
        if (error_message != nullptr) *error_message = system_error_message("监听 IPC 套接字失败");
        return StatusCode::kIoError;
    }

    int listen_flags = -1;
    int nonblocking_result = -1;
    const int fcntl_error = injected_listener_error("fcntl");
    if (fcntl_error != 0) {
        errno = fcntl_error;
    } else {
        listen_flags = ::fcntl(listen_fd, F_GETFL, 0);
        if (listen_flags >= 0) {
            nonblocking_result = ::fcntl(listen_fd, F_SETFL, listen_flags | O_NONBLOCK);
        }
    }
    if (listen_flags < 0 || nonblocking_result < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("设置 IPC 监听套接字为非阻塞模式失败");
        }
        return StatusCode::kIoError;
    }

    // 只有 socket 已完整完成 bind/listen/nonblock 后才发布给 stop()。
    // 若恢复期间 stop 已开始，本地 RAII fd 会在返回时关闭，避免 stop 关闭旧 fd 后
    // 同号描述符被其他线程复用、而恢复线程继续在错误对象上执行 bind/listen/fcntl。
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_.load()) {
            if (error_message != nullptr) {
                *error_message = "IPC 监听发布前收到停止请求";
            }
            return StatusCode::kInvalidState;
        }
        listen_fd_ = pending_listen_fd.release();
    }
    if (error_message != nullptr) error_message->clear();
    return StatusCode::kOk;
#endif
}

// 使用独立 lock 文件覆盖“一个进程刚 bind、尚未 listen，另一个进程误判为 stale 并 unlink”的窗口。
StatusCode BackendIpcServer::acquire_instance_lock(std::string* error_message)
{
#if !defined(__linux__)
    (void)error_message;
    return StatusCode::kInvalidState;
#else
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (instance_lock_fd_ >= 0) return StatusCode::kOk;
    }

    int open_flags = O_RDWR | O_CREAT | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    open_flags |= O_NOFOLLOW;
#endif
    const std::string lock_path = socket_path_ + ".lock";
    const int lock_fd = ::open(lock_path.c_str(), open_flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    if (lock_fd < 0) {
        if (error_message != nullptr) {
            *error_message = system_error_message("打开 IPC 实例锁失败");
        }
        return StatusCode::kIoError;
    }
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        const int lock_error = errno;
        ::close(lock_fd);
        if (error_message != nullptr) {
            *error_message =
                lock_error == EWOULDBLOCK || lock_error == EAGAIN
                    ? "IPC socket 已由另一个后端实例持有：" + socket_path_
                    : "获取 IPC 实例锁失败：errno=" + std::to_string(lock_error) +
                          "，原因=" + std::strerror(lock_error);
        }
        return lock_error == EWOULDBLOCK || lock_error == EAGAIN
            ? StatusCode::kInvalidState
            : StatusCode::kIoError;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stop_requested_.load()) {
            (void)::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
            if (error_message != nullptr) {
                *error_message = "IPC 实例锁发布前收到停止请求";
            }
            return StatusCode::kInvalidState;
        }
        if (instance_lock_fd_ >= 0) {
            (void)::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
            return StatusCode::kOk;
        }
        instance_lock_fd_ = lock_fd;
    }
    return StatusCode::kOk;
#endif
}

void BackendIpcServer::release_instance_lock()
{
#if defined(__linux__)
    int lock_fd = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        lock_fd = instance_lock_fd_;
        instance_lock_fd_ = -1;
    }
    if (lock_fd >= 0) {
        (void)::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
    }
#endif
}

// 关闭监听 fd 并删除本实例拥有的 socket 文件。
void BackendIpcServer::reset_listen_socket()
{
    close_listen_socket();
    remove_socket_file();
}

// 等待有限指数退避，stop() 可通过条件变量提前唤醒。
bool BackendIpcServer::wait_for_recovery_backoff(std::uint32_t consecutive_failures)
{
    auto delay = listener_recovery_base_backoff_;
    for (std::uint32_t index = 1; index < consecutive_failures; ++index) {
        delay = std::min(kMaxListenerRecoveryBackoff, delay * 2);
    }
    std::unique_lock<std::mutex> lock(state_mutex_);
    start_cv_.wait_for(lock, delay, [this]() { return stop_requested_.load(); });
    return !stop_requested_.load();
}

// 发布监听终态失败并唤醒 Application；清理工作由 server_loop 随后完成。
void BackendIpcServer::report_terminal_failure(StatusCode status, const std::string& error_message)
{
    TerminalFailureCallback callback;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        terminal_status_ = status;
        terminal_error_message_ = error_message;
        callback = terminal_failure_callback_;
    }
    running_.store(false);
    state_.store(BackendIpcServerState::kFailed);
    stop_requested_.store(true);
    start_cv_.notify_all();
    Logger::error("IPC 监听恢复失败，服务进入终态：" + error_message);
    if (callback != nullptr) {
        try {
            callback(status, error_message);
        } catch (const std::exception& error) {
            Logger::error("IPC 终态失败回调异常：" + std::string(error.what()));
        } catch (...) {
            Logger::error("IPC 终态失败回调发生未知异常");
        }
    }
}

// 返回测试注入的 errno；注入器异常视为 EIO，避免测试钩子破坏监听线程。
int BackendIpcServer::injected_listener_error(const std::string& operation)
{
    if (listener_fault_injector_ == nullptr) return 0;
    try {
        return listener_fault_injector_(operation);
    } catch (...) {
        return EIO;
    }
}

// IPC 监听主循环，接受客户端连接并在 fatal 错误后有限重建监听。
void BackendIpcServer::server_loop()
{
#if defined(__linux__)
    std::string listener_error;
    auto listener_status = open_listen_socket(&listener_error);
    if (!is_ok(listener_status)) {
        Logger::error(listener_error + "，路径=" + socket_path_);
        state_.store(BackendIpcServerState::kFailed);
        mark_start_result(listener_status, listener_error);
        reset_listen_socket();
        return;
    }

    try {
        start_client_workers();
    } catch (const std::exception& error) {
        const std::string error_message = "创建 IPC worker 线程失败：" + std::string(error.what());
        Logger::error(error_message);
        state_.store(BackendIpcServerState::kFailed);
        mark_start_result(StatusCode::kInternalError, error_message);
        reset_listen_socket();
        return;
    }

    running_.store(true);
    state_.store(BackendIpcServerState::kRunning);
    mark_start_result(StatusCode::kOk, {});
    Logger::info("IPC 服务已开始监听，UDS 路径：" + socket_path_);

    std::uint32_t consecutive_failures = 0;
    bool terminal_failure = false;
    while (!stop_requested_.load()) {
        int listen_fd = -1;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            listen_fd = listen_fd_;
        }
        std::string fatal_error;

        pollfd listen_poll{};
        listen_poll.fd = listen_fd;
        listen_poll.events = POLLIN;
        int poll_result = -1;
        const int poll_error = injected_listener_error("poll");
        if (poll_error != 0) {
            errno = poll_error;
        } else {
            poll_result = ::poll(&listen_poll, 1, kListenPollTimeoutMilliseconds);
        }
        if (stop_requested_.load()) break;
        if (poll_result == 0) {
            consecutive_failures = 0;
            continue;
        }
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            fatal_error = system_error_message("等待 IPC 客户端连接失败");
        } else if ((listen_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fatal_error = "IPC 监听套接字进入异常状态，revents=" + std::to_string(listen_poll.revents);
        } else if ((listen_poll.revents & POLLIN) == 0) {
            continue;
        }

        int client_fd = -1;
        if (fatal_error.empty()) {
            const int accept_error = injected_listener_error("accept");
            if (accept_error != 0) {
                errno = accept_error;
            } else {
                client_fd = ::accept(listen_fd, nullptr, nullptr);
            }
            if (client_fd < 0) {
                if (stop_requested_.load()) break;
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                    consecutive_failures = 0;
                    continue;
                }
                fatal_error = system_error_message("接受 IPC 客户端连接失败");
            }
        }

        if (!fatal_error.empty()) {
            running_.store(false);
            state_.store(BackendIpcServerState::kRecovering);
            reset_listen_socket();
            ++consecutive_failures;
            Logger::error(
                fatal_error + "；准备重建监听，连续失败=" + std::to_string(consecutive_failures) +
                "/" + std::to_string(max_consecutive_listener_failures_));

            while (!stop_requested_.load()) {
                if (consecutive_failures >= max_consecutive_listener_failures_) {
                    report_terminal_failure(StatusCode::kIoError, fatal_error);
                    terminal_failure = true;
                    break;
                }
                if (!wait_for_recovery_backoff(consecutive_failures)) break;

                listener_error.clear();
                listener_status = open_listen_socket(&listener_error);
                if (is_ok(listener_status)) {
                    if (stop_requested_.load()) {
                        reset_listen_socket();
                        break;
                    }
                    running_.store(true);
                    state_.store(BackendIpcServerState::kRunning);
                    Logger::info(
                        "IPC 监听已恢复，UDS 路径：" + socket_path_ +
                        "，此前连续失败=" + std::to_string(consecutive_failures));
                    break;
                }

                reset_listen_socket();
                fatal_error = listener_error.empty() ? "重建 IPC 监听失败" : listener_error;
                ++consecutive_failures;
                Logger::error(
                    fatal_error + "；连续恢复失败=" + std::to_string(consecutive_failures) +
                    "/" + std::to_string(max_consecutive_listener_failures_));
            }
            if (terminal_failure || stop_requested_.load()) break;
            continue;
        }

        consecutive_failures = 0;
        if (stop_requested_.load()) {
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            break;
        }

        std::string timeout_error;
        if (!is_ok(set_socket_timeout(client_fd, kClientSocketTimeoutSeconds, &timeout_error))) {
            Logger::error(timeout_error);
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            continue;
        }
        if (!track_client_socket(client_fd)) {
            ::shutdown(client_fd, SHUT_RDWR);
            ::close(client_fd);
            break;
        }
        if (!try_acquire_client_slot()) {
            Logger::warn("IPC 请求过多，已触发过载保护，当前并发上限=" + std::to_string(kMaxConcurrentIpcClients));
            std::string write_error;
            const auto error_json = ipc_protocol::build_error_response(
                nullptr,
                "server_busy",
                "后端 IPC 请求过多，请稍后重试");
            write_frame(client_fd, error_json, &write_error);
            close_client_socket(client_fd);
            continue;
        }
        try {
            if (!enqueue_client_socket(client_fd)) {
                close_client_socket(client_fd);
                release_client_slot();
                break;
            }
        } catch (const std::exception& error) {
            Logger::error("加入 IPC 客户端任务队列失败：" + std::string(error.what()));
            close_client_socket(client_fd);
            release_client_slot();
        }
    }

    reset_listen_socket();
    shutdown_all_client_sockets();
    stop_client_workers();
#endif

    running_.store(false);
}

// 读取、处理并回复单个客户端连接。
void BackendIpcServer::handle_client_connection(int client_fd)
{
#if defined(__linux__)
    std::string request_json;
    std::string read_error;
    const auto read_status = read_frame(client_fd, &request_json, &read_error);
    if (!is_ok(read_status)) {
        if (stop_requested_.load()) {
            close_client_socket(client_fd);
            return;
        }
        if (!read_error.empty()) {
            Logger::error("读取 IPC 请求失败：" + read_error);
            std::string write_error;
            const auto error_json = ipc_protocol::build_error_response(
                nullptr,
                ipc_handlers::status_code_string(read_status),
                read_error);
            write_frame(client_fd, error_json, &write_error);
        }
        close_client_socket(client_fd);
        return;
    }

    const auto request_started = std::chrono::steady_clock::now();
    // process_request 保持同步执行，耗时监控用于定位慢 IPC 方法或后端锁竞争。
    std::string method_for_log;
    const auto response_json = process_request(request_json, &method_for_log);
    const auto elapsed = std::chrono::steady_clock::now() - request_started;
    if (elapsed >= kSlowIpcRequestThreshold) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const bool succeeded = ipc_response_succeeded_for_log(response_json);
        const auto bounded_method = bounded_ipc_method(method_for_log);
        const auto decision = take_slow_ipc_log_decision(bounded_method, succeeded);
        if (decision.emit) {
            // 被抑制请求只做固定前缀分类；仅本次确定输出时才解析响应 JSON。
            const auto error_info = succeeded ? IpcErrorLogInfo{} : ipc_error_for_log(response_json);
            std::string log_message =
                "IPC 请求处理较慢：method=" + bounded_method +
                "，耗时=" + std::to_string(elapsed_ms) +
                "ms，阈值=" + std::to_string(kSlowIpcRequestThreshold.count()) +
                "ms，结果=" + (succeeded ? "success" : "failure") +
                "，错误码=" + (succeeded ? "none" : truncate_log_text(error_info.code)) +
                "，期间已抑制=" + std::to_string(decision.suppressed_count) + "次";
            if (!succeeded && !error_info.message.empty()) {
                log_message += "，错误=" + truncate_log_text(error_info.message);
            }
            Logger::warn_rate_limited(log_message);
        }
    }
    std::string write_error;
    if (!is_ok(write_frame(client_fd, response_json, &write_error)) &&
        !write_error.empty() && !stop_requested_.load()) {
        Logger::error("写入 IPC 响应失败：" + write_error);
    }
    close_client_socket(client_fd);
#else
    (void)client_fd;
#endif
}

// 客户端 worker 循环，从队列消费连接。
void BackendIpcServer::client_worker_loop()
{
    while (true) {
        int client_fd = -1;
        {
            std::unique_lock<std::mutex> lock(client_queue_mutex_);
            client_queue_cv_.wait(lock, [this]() {
                return stop_requested_.load() || !client_queue_.empty();
            });

            if (client_queue_.empty()) {
                if (stop_requested_.load()) {
                    return;
                }
                continue;
            }

            client_fd = client_queue_.front();
            client_queue_.pop_front();
        }

        try {
            handle_client_connection(client_fd);
        } catch (const std::exception& error) {
            Logger::error("处理 IPC 客户端请求异常：" + std::string(error.what()));
            close_client_socket(client_fd);
        } catch (...) {
            Logger::error("处理 IPC 客户端请求发生未知 C++ 异常");
            close_client_socket(client_fd);
        }
        release_client_slot();
    }
}

// 创建固定数量的客户端 worker。
void BackendIpcServer::start_client_workers()
{
    {
        std::lock_guard<std::mutex> lock(client_queue_mutex_);
        if (!client_workers_.empty()) {
            return;
        }
    }

    std::vector<std::thread> workers;
    workers.reserve(kIpcWorkerCount);
    try {
        for (int index = 0; index < kIpcWorkerCount; ++index) {
            workers.emplace_back(&BackendIpcServer::client_worker_loop, this);
        }
    } catch (...) {
        stop_requested_.store(true);
        client_queue_cv_.notify_all();
        for (auto& thread : workers) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        throw;
    }

    std::lock_guard<std::mutex> lock(client_queue_mutex_);
    client_workers_.swap(workers);
}

// 停止并回收所有客户端 worker。
void BackendIpcServer::stop_client_workers()
{
    {
        std::lock_guard<std::mutex> lock(client_queue_mutex_);
        client_queue_cv_.notify_all();
    }

    std::vector<std::thread> workers;
    std::vector<int> pending_client_fds;
    {
        std::lock_guard<std::mutex> lock(client_queue_mutex_);
        workers.swap(client_workers_);
        pending_client_fds.reserve(client_queue_.size());
        while (!client_queue_.empty()) {
            pending_client_fds.push_back(client_queue_.front());
            client_queue_.pop_front();
        }
    }

    for (const auto client_fd : pending_client_fds) {
        close_client_socket(client_fd);
    }

    client_queue_cv_.notify_all();
    for (auto& thread : workers) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    active_client_tasks_.store(0);
}

// 将客户端 socket 加入待处理队列。
bool BackendIpcServer::enqueue_client_socket(int client_fd)
{
    {
        std::lock_guard<std::mutex> lock(client_queue_mutex_);
        if (stop_requested_.load()) {
            return false;
        }
        client_queue_.push_back(client_fd);
    }
    client_queue_cv_.notify_one();
    return true;
}

// 解析 IPC 请求并分发到 BackendService。
std::string BackendIpcServer::process_request(
    const std::string& request_json,
    std::string* parsed_method)
{
    if (parsed_method != nullptr) {
        *parsed_method = "unknown";
    }
    nlohmann::json root;
    std::string parse_error;
    const auto parse_status = ipc_protocol::parse_request_json(request_json, &root, &parse_error);
    if (!is_ok(parse_status)) {
        return ipc_protocol::build_error_response(
            nullptr,
            "invalid_request",
            parse_error.empty() ? "请求 JSON 非法" : parse_error);
    }

    const auto id_json = ipc_protocol::request_id_json_or_null(root);
    std::string method;
    if (!is_ok(ipc_protocol::extract_request_method(root, &method, nullptr))) {
        return ipc_protocol::build_error_response(id_json, "invalid_request", "缺少 method 字段");
    }
    if (parsed_method != nullptr) {
        *parsed_method = method;
    }

    ipc_handlers::IpcHandlerContext context{backend_service_, root, id_json, method};
    std::string response;
    if (ipc_handlers::handle_system_request(context, &response) ||
        ipc_handlers::handle_events_request(context, &response) ||
        ipc_handlers::handle_alarms_request(context, &response) ||
        ipc_handlers::handle_settings_request(context, &response) ||
        ipc_handlers::handle_modbus_server_request(context, &response) ||
        ipc_handlers::handle_collection_request(context, &response) ||
        ipc_handlers::handle_history_request(context, &response) ||
        ipc_handlers::handle_templates_request(context, &response) ||
        ipc_handlers::handle_device_command_request(context, &response)) {
        return response;
    }
    return ipc_protocol::build_error_response(id_json, "method_not_found", "不支持的 IPC 方法：" + method);
}

// 从 socket 读取长度前缀帧。
StatusCode BackendIpcServer::read_frame(int fd, std::string* payload, std::string* error_message)
{
#if !defined(__linux__)
    (void)fd;
    (void)payload;
    if (error_message != nullptr) {
        *error_message = "当前平台不支持 IPC 服务";
    }
    return StatusCode::kInvalidState;
#else
    if (payload == nullptr) {
        if (error_message != nullptr) {
            *error_message = "IPC 负载输出参数为空";
        }
        return StatusCode::kInvalidArgument;
    }

    std::uint32_t length_be = 0;
    const auto deadline = std::chrono::steady_clock::now() + kIpcFrameReadTimeout;
    // IPC 帧格式固定为 4 字节大端长度头 + JSON 负载，Go Web 客户端使用同一协议。
    const auto header_status = read_exact(
        fd,
        reinterpret_cast<std::uint8_t*>(&length_be),
        sizeof(length_be),
        "读取 IPC 头失败",
        deadline,
        error_message);
    if (!is_ok(header_status)) {
        return header_status;
    }

    const auto length = ntohl(length_be);
    const auto length_status = ipc_protocol::validate_payload_length(length, error_message);
    if (!is_ok(length_status)) {
        return length_status;
    }

    std::string data(length, '\0');
    const auto body_status = read_exact(
        fd,
        reinterpret_cast<std::uint8_t*>(data.data()),
        data.size(),
        "读取 IPC 负载失败",
        deadline,
        error_message);
    if (!is_ok(body_status)) {
        return body_status;
    }

    *payload = std::move(data);
    return StatusCode::kOk;
#endif
}

// 向 socket 写入长度前缀帧。
StatusCode BackendIpcServer::write_frame(int fd, const std::string& payload, std::string* error_message)
{
#if !defined(__linux__)
    (void)fd;
    (void)payload;
    if (error_message != nullptr) {
        *error_message = "当前平台不支持 IPC 服务";
    }
    return StatusCode::kInvalidState;
#else
    const auto length = static_cast<std::uint32_t>(payload.size());
    const auto length_status = ipc_protocol::validate_payload_length(length, error_message);
    if (!is_ok(length_status)) {
        return length_status;
    }

    const auto length_be = htonl(length);
    const auto header_status = write_exact(
        fd,
        reinterpret_cast<const std::uint8_t*>(&length_be),
        sizeof(length_be),
        "写入 IPC 头失败",
        error_message);
    if (!is_ok(header_status)) {
        return header_status;
    }

    return write_exact(
        fd,
        reinterpret_cast<const std::uint8_t*>(payload.data()),
        payload.size(),
        "写入 IPC 负载失败",
        error_message);
#endif
}

// 记录启动结果。
void BackendIpcServer::mark_start_result(StatusCode status, const std::string& error_message)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_status_ = status;
        start_error_message_ = error_message;
        start_completed_ = true;
    }
    start_cv_.notify_all();
}

// 关闭监听套接字。
void BackendIpcServer::shutdown_listen_socket()
{
#if defined(__linux__)
    // 不在 stop 调用线程释放 fd 编号；shutdown 足以唤醒 poll/accept，
    // server worker 退出并 join 后再由 close_listen_socket 完成最终回收。
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (listen_fd_ >= 0) {
        (void)::shutdown(listen_fd_, SHUT_RDWR);
    }
#endif
}

void BackendIpcServer::close_listen_socket()
{
#if defined(__linux__)
    int listen_fd = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        listen_fd = listen_fd_;
        listen_fd_ = -1;
    }

    if (listen_fd >= 0) {
        ::shutdown(listen_fd, SHUT_RDWR);
        ::close(listen_fd);
    }
#endif
}

// 登记客户端套接字。
bool BackendIpcServer::track_client_socket(int client_fd)
{
    std::lock_guard<std::mutex> lock(client_fds_mutex_);
    if (stop_requested_.load()) {
        return false;
    }
    client_fds_.insert(client_fd);
    return true;
}

// 唤醒所有客户端 socket 上的阻塞调用，socket 仍由对应 worker 关闭。
void BackendIpcServer::shutdown_all_client_sockets()
{
#if defined(__linux__)
    // 必须在集合锁内完成 shutdown。若只复制裸整数再解锁，worker 可能先
    // close，随后该 fd 被其他线程复用，停机线程就会误 shutdown 新对象。
    std::lock_guard<std::mutex> lock(client_fds_mutex_);
    for (const auto client_fd : client_fds_) {
        ::shutdown(client_fd, SHUT_RDWR);
    }
#endif
}

// 关闭客户端套接字。
void BackendIpcServer::close_client_socket(int client_fd)
{
#if defined(__linux__)
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock(client_fds_mutex_);
        const auto iterator = client_fds_.find(client_fd);
        if (iterator != client_fds_.end()) {
            client_fds_.erase(iterator);
            should_close = true;
        }
    }

    if (should_close) {
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
    }
#else
    (void)client_fd;
#endif
}

// 删除本实例创建的 Unix Domain Socket 文件。
void BackendIpcServer::remove_socket_file()
{
#if defined(__linux__)
    bool should_remove = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        should_remove = socket_file_owned_;
        socket_file_owned_ = false;
    }
    if (should_remove && !socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
#endif
}

// 尝试获取 IPC 客户端并发槽位。
bool BackendIpcServer::try_acquire_client_slot()
{
    int current = active_client_tasks_.load();
    while (current < kMaxConcurrentIpcClients) {
        if (active_client_tasks_.compare_exchange_weak(current, current + 1)) {
            return true;
        }
    }
    return false;
}

// 释放 IPC 客户端并发槽位。
void BackendIpcServer::release_client_slot()
{
    const auto previous = active_client_tasks_.fetch_sub(1);
    if (previous <= 0) {
        active_client_tasks_.store(0);
    }
}

// 使用稳定 method/结果键限制慢 IPC 日志频率，并对状态表执行 TTL 清理与容量淘汰。
BackendIpcServer::SlowIpcLogDecision BackendIpcServer::take_slow_ipc_log_decision(
    const std::string& method,
    bool succeeded)
{
    const auto now = std::chrono::steady_clock::now();
    const auto key = method + (succeeded ? "|success" : "|failure");
    std::lock_guard<std::mutex> lock(slow_ipc_log_mutex_);

    auto iterator = slow_ipc_logs_.find(key);
    if (iterator == slow_ipc_logs_.end()) {
        for (auto stale = slow_ipc_logs_.begin(); stale != slow_ipc_logs_.end();) {
            if (now - stale->second.last_seen >= kSlowIpcLogEntryTtl) {
                stale = slow_ipc_logs_.erase(stale);
            } else {
                ++stale;
            }
        }
        if (slow_ipc_logs_.size() >= kMaxSlowIpcLogEntries) {
            auto oldest = slow_ipc_logs_.begin();
            for (auto candidate = slow_ipc_logs_.begin(); candidate != slow_ipc_logs_.end(); ++candidate) {
                if (candidate->second.last_seen < oldest->second.last_seen) {
                    oldest = candidate;
                }
            }
            slow_ipc_logs_.erase(oldest);
        }
        iterator = slow_ipc_logs_.emplace(key, SlowIpcLogState{}).first;
    }

    auto& state = iterator->second;
    state.last_seen = now;
    if (state.last_emit.time_since_epoch().count() == 0 ||
        now - state.last_emit >= kSlowIpcLogWindow) {
        SlowIpcLogDecision decision;
        decision.emit = true;
        decision.suppressed_count = state.suppressed_count;
        state.last_emit = now;
        state.suppressed_count = 0;
        return decision;
    }

    ++state.suppressed_count;
    return {};
}

}  // namespace edge_controller
