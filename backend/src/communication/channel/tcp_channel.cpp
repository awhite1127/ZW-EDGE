// TCP 通道实现：负责非阻塞建连、超时读写和断线恢复，并向上层暴露统一通道接口。
#include "communication/channel/tcp_channel.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <utility>

#include "communication/channel/bounded_address_resolver.h"
#include "shared/common/format_utils.h"
#include "shared/common/logger.h"
#include "shared/common/time_utils.h"

#if defined(__linux__)
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace edge_controller {

namespace {

// 获取当前毫秒时间戳。
TimestampMs now_ms()
{
    return time_utils::system_now_ms();
}

#if defined(__linux__)
// 将超时时间限制在有效范围内。
int normalized_timeout_ms(std::uint32_t timeout_ms, int fallback_ms)
{
    return static_cast<int>(timeout_ms == 0 ? fallback_ms : timeout_ms);
}

// 将系统错误码转换为可读文本。
std::string errno_message(int error_number)
{
    return std::strerror(error_number);
}

// 关闭文件描述符。
void close_fd(int* fd)
{
    if (fd != nullptr && *fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

// 在绝对截止时间前分段等待文件描述符，允许停机请求在 100ms 内打断阻塞等待。
bool wait_for_fd_until(
    int fd,
    bool writable,
    TimestampMs deadline_ms,
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* close_requested,
    int wake_fd,
    StatusCode* status,
    std::string* error_message)
{
    constexpr TimestampMs kCancellationPollMs = 20;
    while (true) {
        if ((cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) ||
            (close_requested != nullptr && close_requested->load(std::memory_order_acquire))) {
            if (status != nullptr) {
                *status = StatusCode::kInvalidState;
            }
            if (error_message != nullptr) {
                *error_message = "TCP 操作已取消";
            }
            return false;
        }

        const auto now_ms = time_utils::steady_now_ms();
        if (now_ms >= deadline_ms) {
            if (status != nullptr) {
                *status = StatusCode::kTimeout;
            }
            if (error_message != nullptr) {
                *error_message = writable ? "TCP 发送等待超时" : "TCP 响应超时";
            }
            return false;
        }

        const auto remaining_ms = deadline_ms - now_ms;
        const bool needs_cancel_poll =
            cancel_requested != nullptr || (close_requested != nullptr && wake_fd < 0);
        const auto wait_ms = needs_cancel_poll
            ? std::min(remaining_ms, kCancellationPollMs)
            : remaining_ms;
        pollfd descriptors[2]{};
        descriptors[0].fd = fd;
        descriptors[0].events = (writable ? POLLOUT : POLLIN) | POLLERR | POLLHUP;
        nfds_t descriptor_count = 1;
        if (wake_fd >= 0) {
            descriptors[1].fd = wake_fd;
            descriptors[1].events = POLLIN;
            descriptor_count = 2;
        }

        int ready = 0;
        do {
            ready = ::poll(
                descriptors,
                descriptor_count,
                static_cast<int>(std::max<TimestampMs>(wait_ms, 1U)));
        } while (ready < 0 && errno == EINTR);
        if (ready > 0) {
            if (descriptor_count == 2 && (descriptors[1].revents & POLLIN) != 0) {
                continue;
            }
            const short expected_event = writable ? POLLOUT : POLLIN;
            if ((descriptors[0].revents & expected_event) != 0) {
                return true;
            }
            if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                if (status != nullptr) {
                    *status = StatusCode::kIoError;
                }
                if (error_message != nullptr) {
                    *error_message = "TCP socket 在等待期间断开";
                }
                return false;
            }
            continue;
        }
        if (ready == 0) {
            continue;
        }
        if (status != nullptr) {
            *status = StatusCode::kIoError;
        }
        if (error_message != nullptr) {
            *error_message = std::string("TCP socket 等待失败: ") + errno_message(errno);
        }
        return false;
    }
}
#endif

}  // namespace

TcpChannel::TcpChannel(
    ChannelConfig config,
    channel_internal::AddressResolverFunction resolver)
    : config_(std::move(config)), resolver_(resolver)
{
    status_.channel_id = config_.channel_id;
    status_.configured = true;
    status_.enabled = config_.enabled;
    status_.device_path = endpoint();
    status_.status = config_.enabled ? "closed" : "disabled";
    status_.last_change_time_ms = now_ms();
    status_.diagnosis = make_normal_diagnosis(
        DiagnosisLevel::kChannel,
        config_.channel_id,
        endpoint(),
        0);
}

TcpChannel::~TcpChannel()
{
    close();
}

// 打开 TCP 通道连接。
StatusCode TcpChannel::open()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_locked(nullptr);
}

// 解析TCP远端地址列表。
StatusCode TcpChannel::open_locked(const std::atomic<bool>* cancel_requested)
{
    if (!config_.enabled) {
        status_.enabled = false;
        status_.opened = false;
        status_.status = "disabled";
        status_.last_change_time_ms = now_ms();
        return StatusCode::kInvalidState;
    }

    if (is_open_locked()) {
        return StatusCode::kOk;
    }

    status_.device_path = endpoint();

    if (config_.tcp_host.empty()) {
        set_error_locked("TCP host 未配置", DiagnosisErrorCode::kTcpConnectFailed);
        return StatusCode::kInvalidArgument;
    }
    if (config_.tcp_port == 0) {
        set_error_locked("TCP port 非法", DiagnosisErrorCode::kTcpConnectFailed);
        return StatusCode::kInvalidArgument;
    }

#if !defined(__linux__)
    (void)cancel_requested;
    set_error_locked("TCP 通道仅支持 Linux POSIX socket", DiagnosisErrorCode::kTcpConnectFailed);
    return StatusCode::kInvalidState;
#else
    // DNS 与后续所有候选地址建连共享一个总截止时间，解析不能额外延长连接阶段。
    const int connect_timeout_ms = normalized_timeout_ms(config_.connect_timeout_ms, 3000);
    const auto connect_deadline_ms =
        time_utils::steady_now_ms() + static_cast<TimestampMs>(connect_timeout_ms);
    const auto resolution = channel_internal::resolve_addresses_until(
        config_.tcp_host,
        std::to_string(config_.tcp_port),
        connect_deadline_ms,
        cancel_requested,
        &close_requested_,
        close_wakeup_.native_handle(),
        resolver_);
    if (!is_ok(resolution.status)) {
        if (resolution.status == StatusCode::kInvalidState) {
            return resolution.status;
        }
        set_error_locked(
            resolution.error_message.empty() ? "解析 TCP 目标失败" : resolution.error_message,
            resolution.status == StatusCode::kTimeout
                ? DiagnosisErrorCode::kTcpConnectTimeout
                : DiagnosisErrorCode::kTcpConnectFailed);
        return resolution.status;
    }

    std::string last_error = "TCP 连接失败";
    bool timed_out = false;

    for (const auto& address : resolution.addresses) {
        if ((cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) ||
            close_requested_.load(std::memory_order_acquire)) {
            set_error_locked("TCP 连接已取消", DiagnosisErrorCode::kTcpConnectFailed);
            return StatusCode::kInvalidState;
        }
        if (time_utils::steady_now_ms() >= connect_deadline_ms) {
            timed_out = true;
            last_error = "TCP 连接超时";
            break;
        }
        // 在 socket 创建时原子设置非阻塞和 CLOEXEC，避免多线程 fork/exec 窗口泄漏通信 FD。
        int candidate_fd = ::socket(
            address.family,
            address.socket_type | SOCK_NONBLOCK | SOCK_CLOEXEC,
            address.protocol);
        if (candidate_fd < 0) {
            last_error = "创建 TCP socket 失败: " + errno_message(errno);
            continue;
        }

        const auto connect_status = ::connect(
            candidate_fd,
            reinterpret_cast<const sockaddr*>(address.address.data()),
            static_cast<socklen_t>(address.address.size()));
        if (connect_status != 0 && errno != EINPROGRESS) {
            last_error = "TCP 连接失败: " + errno_message(errno);
            close_fd(&candidate_fd);
            continue;
        }

        if (connect_status != 0) {
            StatusCode wait_status = StatusCode::kOk;
            std::string wait_error;
            if (!wait_for_fd_until(
                    candidate_fd,
                    true,
                    connect_deadline_ms,
                    cancel_requested,
                    &close_requested_,
                    close_wakeup_.native_handle(),
                    &wait_status,
                    &wait_error)) {
                if (wait_status == StatusCode::kInvalidState) {
                    close_fd(&candidate_fd);
                    return wait_status;
                }
                timed_out = wait_status == StatusCode::kTimeout;
                last_error = timed_out ? "TCP 连接超时" : wait_error;
                close_fd(&candidate_fd);
                continue;
            }

            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (::getsockopt(candidate_fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 ||
                socket_error != 0) {
                last_error = "TCP 连接失败: " + errno_message(socket_error == 0 ? errno : socket_error);
                close_fd(&candidate_fd);
                continue;
            }
        }

        fd_ = candidate_fd;
        status_.enabled = true;
        status_.opened = true;
        status_.status = "online";
        status_.last_open_time_ms = now_ms();
        status_.last_change_time_ms = status_.last_open_time_ms;
        status_.consecutive_error_count = 0;
        status_.diagnosis = make_normal_diagnosis(
            DiagnosisLevel::kChannel,
            config_.channel_id,
            endpoint(),
            status_.last_open_time_ms);
        status_.last_error_message.clear();
        last_terminal_error_message_.clear();
        if (Logger::debug_enabled()) {
            Logger::debug("TCP 通道已连接: " + config_.channel_id + "，目标=" + endpoint());
        }
        return StatusCode::kOk;
    }

    set_error_locked(
        last_error,
        timed_out ? DiagnosisErrorCode::kTcpConnectTimeout : DiagnosisErrorCode::kTcpConnectFailed);
    return timed_out ? StatusCode::kTimeout : StatusCode::kIoError;
#endif
}

// 关闭 TCP 通道连接。
void TcpChannel::close()
{
    std::lock_guard<std::mutex> close_lock(close_mutex_);
    close_requested_.store(true, std::memory_order_release);
    close_wakeup_.signal();
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
    close_wakeup_.reset();
    close_requested_.store(false, std::memory_order_release);
}

// 在持锁状态下关闭。
void TcpChannel::close_locked()
{
#if defined(__linux__)
    close_fd(&fd_);
#endif

    if (status_.opened) {
        status_.opened = false;
        status_.last_close_time_ms = now_ms();
        status_.last_change_time_ms = status_.last_close_time_ms;
        status_.status = config_.enabled ? "closed" : "disabled";
        if (Logger::debug_enabled()) {
            Logger::debug("TCP 通道已关闭: " + config_.channel_id);
        }
        return;
    }

    if (!config_.enabled) {
        status_.status = "disabled";
    } else if (status_.status.empty()) {
        status_.status = "closed";
    }
}

// 判断 TCP 通道是否已连接。
bool TcpChannel::is_open() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_locked();
}

// 在持锁状态下判断通道是否已打开。
bool TcpChannel::is_open_locked() const
{
    return status_.opened;
}

// 保持通道接口一致，TCP 通道无需额外 flush。
StatusCode TcpChannel::flush()
{
    return StatusCode::kOk;
}

// 返回通道底层文件描述符；内部加锁保护句柄状态。
int TcpChannel::native_handle() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_;
}

// 发送 Modbus TCP 请求并读取响应。
StatusCode TcpChannel::transceive(
    const std::vector<std::uint8_t>& request,
    int timeout_ms,
    std::vector<std::uint8_t>* response,
    const ChannelTraceContext& trace_context,
    const std::atomic<bool>* cancel_requested,
    const ChannelResponseValidator& response_validator)
{
    (void)trace_context;
    std::lock_guard<std::mutex> lock(mutex_);

    if (response == nullptr) {
        set_error_locked("响应输出参数为空", DiagnosisErrorCode::kConfigInvalid);
        return StatusCode::kInvalidArgument;
    }
    response->clear();
    if ((cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) ||
        close_requested_.load(std::memory_order_acquire)) {
        return StatusCode::kInvalidState;
    }

    if (request.empty()) {
        set_error_locked("TCP 请求 ADU 为空", DiagnosisErrorCode::kConfigInvalid);
        return StatusCode::kInvalidArgument;
    }

    if (!is_open_locked()) {
        const auto open_status = open_locked(cancel_requested);
        if (!is_ok(open_status)) {
            return open_status;
        }
    }

#if !defined(__linux__)
    (void)request;
    (void)timeout_ms;
    (void)cancel_requested;
    (void)response_validator;
    set_error_locked("TCP 收发仅支持 Linux POSIX socket", DiagnosisErrorCode::kTcpConnectFailed);
    return StatusCode::kInvalidState;
#else
    if (Logger::debug_enabled()) {
        Logger::debug("通道 " + config_.channel_id + " 发送 Modbus TCP 请求，目标=" +
                      endpoint() + "，ADU=" + format_utils::bytes_to_hex(request));
    }

    const int response_timeout_ms = std::max(timeout_ms, 1);
    const auto send_deadline_ms =
        time_utils::steady_now_ms() + static_cast<TimestampMs>(response_timeout_ms);
    const auto send_status = send_all_locked(request, send_deadline_ms, cancel_requested);
    if (!is_ok(send_status)) {
        return send_status;
    }
    status_.last_send_time_ms = now_ms();

    const auto response_deadline_ms =
        time_utils::steady_now_ms() + static_cast<TimestampMs>(response_timeout_ms);
    std::vector<std::uint8_t> header;
    const auto header_status =
        read_exact_until_locked(7, response_deadline_ms, &header, cancel_requested);
    if (!is_ok(header_status)) {
        response->insert(response->end(), header.begin(), header.end());
        return header_status;
    }

    const auto mbap_length =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[4]) << 8U) |
                                   static_cast<std::uint16_t>(header[5]));
    if (mbap_length == 0 || mbap_length > 253) {
        *response = header;
        set_error_locked("MBAP Length 异常", DiagnosisErrorCode::kMbapLengthInvalid);
        return StatusCode::kProtocolError;
    }

    *response = header;
    if (mbap_length > 1) {
        std::vector<std::uint8_t> tail;
        const auto tail_status = read_exact_until_locked(
            static_cast<std::size_t>(mbap_length - 1U),
            response_deadline_ms,
            &tail,
            cancel_requested);
        if (!is_ok(tail_status)) {
            response->insert(response->end(), tail.begin(), tail.end());
            return tail_status;
        }
        response->insert(response->end(), tail.begin(), tail.end());
    }

    status_.last_receive_time_ms = now_ms();
    status_.consecutive_error_count = 0;
    status_.diagnosis = make_normal_diagnosis(
        DiagnosisLevel::kChannel,
        config_.channel_id,
        endpoint(),
        status_.last_receive_time_ms);
    status_.last_error_message.clear();
    status_.status = "online";
    status_.last_change_time_ms = now_ms();
    last_terminal_error_message_.clear();
    if (Logger::debug_enabled()) {
        Logger::debug("通道 " + config_.channel_id + " 收到 Modbus TCP 响应，目标=" +
                      endpoint() + "，ADU=" + format_utils::bytes_to_hex(*response));
    }
    if (response_validator) {
        bool connection_reusable = true;
        const auto validation_status = response_validator(*response, &connection_reusable);
        if (!connection_reusable) {
            // 验证和条件关闭仍在同一次通道事务锁内，避免旧请求误关后续请求使用的连接。
            close_locked();
        }
        return validation_status;
    }
    return StatusCode::kOk;
#endif
}

#if defined(__linux__)
// 在持锁状态下发送全部。
StatusCode TcpChannel::send_all_locked(
    const std::vector<std::uint8_t>& request,
    TimestampMs deadline_ms,
    const std::atomic<bool>* cancel_requested)
{
    std::size_t written = 0;
    while (written < request.size()) {
        StatusCode wait_status = StatusCode::kOk;
        std::string wait_error;
        if (!wait_for_fd_until(
                fd_,
                true,
                deadline_ms,
                cancel_requested,
                &close_requested_,
                close_wakeup_.native_handle(),
                &wait_status,
                &wait_error)) {
            if (wait_status == StatusCode::kInvalidState) {
                if (written > 0) {
                    // 已发送部分请求后不能复用此连接，否则迟到响应会污染下一次 MBAP 帧。
                    close_locked();
                }
                return wait_status;
            }
            set_error_locked(
                wait_error,
                DiagnosisErrorCode::kTcpSendFailed);
            return wait_status;
        }

        const auto send_size = ::send(fd_, request.data() + written, request.size() - written, MSG_NOSIGNAL);
        if (send_size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            set_error_locked("TCP 发送失败: " + errno_message(errno), DiagnosisErrorCode::kTcpSendFailed);
            return StatusCode::kIoError;
        }
        if (send_size == 0) {
            set_error_locked("TCP 发送返回 0 字节", DiagnosisErrorCode::kTcpSendFailed);
            return StatusCode::kIoError;
        }
        written += static_cast<std::size_t>(send_size);
    }
    return StatusCode::kOk;
}

// 在持锁状态下读取指定长度的数据，直至截止时间。
StatusCode TcpChannel::read_exact_until_locked(
    std::size_t expected_size,
    TimestampMs deadline_ms,
    std::vector<std::uint8_t>* response,
    const std::atomic<bool>* cancel_requested)
{
    if (response == nullptr) {
        return StatusCode::kInvalidArgument;
    }
    response->clear();
    response->reserve(expected_size);

    while (response->size() < expected_size) {
        const auto now_steady_ms = time_utils::steady_now_ms();
        if (now_steady_ms >= deadline_ms) {
            set_error_locked("TCP 响应超时", DiagnosisErrorCode::kTcpResponseTimeout);
            return StatusCode::kTimeout;
        }

        StatusCode wait_status = StatusCode::kOk;
        std::string wait_error;
        if (!wait_for_fd_until(
                fd_,
                false,
                deadline_ms,
                cancel_requested,
                &close_requested_,
                close_wakeup_.native_handle(),
                &wait_status,
                &wait_error)) {
            if (wait_status == StatusCode::kInvalidState) {
                // read_exact 仅在完整请求发出后调用，取消后必须丢弃当前连接。
                close_locked();
                return wait_status;
            }
            set_error_locked(
                wait_status == StatusCode::kTimeout ? "TCP 响应超时" : wait_error,
                wait_status == StatusCode::kTimeout ? DiagnosisErrorCode::kTcpResponseTimeout : DiagnosisErrorCode::kChannelIoError);
            return wait_status;
        }

        std::uint8_t buffer[512];
        const auto wanted = std::min<std::size_t>(sizeof(buffer), expected_size - response->size());
        const auto read_size = ::recv(fd_, buffer, wanted, 0);
        if (read_size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            set_error_locked("TCP 接收失败: " + errno_message(errno), DiagnosisErrorCode::kChannelIoError);
            return StatusCode::kIoError;
        }
        if (read_size == 0) {
            set_error_locked("TCP 远端关闭连接", DiagnosisErrorCode::kTcpRemoteClosed);
            return StatusCode::kIoError;
        }
        response->insert(response->end(), buffer, buffer + read_size);
    }
    return StatusCode::kOk;
}
#else
// 在持锁状态下发送全部。
StatusCode TcpChannel::send_all_locked(
    const std::vector<std::uint8_t>&,
    TimestampMs,
    const std::atomic<bool>*)
{
    return StatusCode::kInvalidState;
}

// 在持锁状态下读取指定长度的数据，直至截止时间。
StatusCode TcpChannel::read_exact_until_locked(
    std::size_t,
    TimestampMs,
    std::vector<std::uint8_t>*,
    const std::atomic<bool>*)
{
    return StatusCode::kInvalidState;
}
#endif

// 获取当前组件配置。
const ChannelConfig& TcpChannel::config() const
{
    return config_;
}

// 返回 TCP 通道运行状态。
ChannelStatus TcpChannel::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

// 获取TCP远端端点描述。
std::string TcpChannel::endpoint() const
{
    if (config_.tcp_host.empty()) {
        return "未配置 TCP 目标";
    }
    return config_.tcp_host + ":" + std::to_string(config_.tcp_port);
}

// 在持锁状态下设置错误信息。
void TcpChannel::set_error_locked(
    const std::string& error_message,
    DiagnosisErrorCode error_code)
{
#if defined(__linux__)
    close_fd(&fd_);
#endif
    status_.enabled = config_.enabled;
    status_.device_path = endpoint();
    if (status_.opened) {
        status_.last_close_time_ms = now_ms();
    }
    status_.opened = false;
    status_.status = config_.enabled ? "fault" : "disabled";
    status_.last_change_time_ms = now_ms();
    ++status_.consecutive_error_count;
    status_.diagnosis = make_diagnosis(
        DiagnosisLevel::kChannel,
        config_.channel_id,
        endpoint(),
        config_.enabled ? DiagnosisRunStatus::kError : DiagnosisRunStatus::kWarning,
        error_code,
        status_.last_receive_time_ms,
        status_.last_change_time_ms,
        status_.consecutive_error_count);
    status_.last_error_message = status_.diagnosis.message;
    if (last_terminal_error_message_ != error_message) {
        last_terminal_error_message_ = error_message;
        Logger::error(
            "通道 " + config_.channel_id +
            " TCP 异常，目标=" + endpoint() +
            "，原因=" + error_message);
    } else {
        if (Logger::debug_enabled()) {
            Logger::debug(
                "通道 " + config_.channel_id +
                " TCP 异常重复发生，终端已降噪，目标=" + endpoint() +
                "，原因=" + error_message);
        }
    }
}

}  // namespace edge_controller
