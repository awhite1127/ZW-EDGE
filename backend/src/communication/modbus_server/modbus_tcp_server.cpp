#include "communication/modbus_server/modbus_tcp_server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "shared/common/logger.h"
#include "shared/common/time_utils.h"
#include "communication/modbus_server/modbus_register_bank.h"

namespace edge_controller {
namespace {

constexpr std::size_t kMbapHeaderSize = 7;
constexpr std::size_t kMaximumAduSize = 260;
constexpr std::size_t kMaximumReceiveBufferSize = 8 * 1024;
constexpr std::size_t kMaximumSendBufferSize = 64 * 1024;
constexpr auto kWarningLogInterval = std::chrono::seconds(5);

class WarningRateLimiter {
public:
    void warn(const std::string& message)
    {
        const auto now = std::chrono::steady_clock::now();
        if (last_log_at_ == std::chrono::steady_clock::time_point{} ||
            now - last_log_at_ >= kWarningLogInterval) {
            auto output = message;
            if (suppressed_count_ > 0) {
                output += "（此前已抑制 " + std::to_string(suppressed_count_) + " 条同类警告）";
            }
            Logger::warn(output);
            last_log_at_ = now;
            suppressed_count_ = 0;
            return;
        }
        ++suppressed_count_;
    }

private:
    std::chrono::steady_clock::time_point last_log_at_{};
    std::uint64_t suppressed_count_{0};
};

// 将套接字错误码转换为可读文本。
std::string socket_error(const std::string& operation)
{
    return operation + "：" + std::strerror(errno);
}

// 将套接字设置为非阻塞模式。
bool set_nonblocking(int fd)
{
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// 按大端字节序读取 16 位无符号整数。
std::uint16_t read_u16_be(const std::uint8_t* input)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[0]) << 8U) |
        static_cast<std::uint16_t>(input[1]));
}

// 按大端字节序追加 16 位无符号整数。
void append_u16_be(std::vector<std::uint8_t>* output, std::uint16_t value)
{
    output->push_back(static_cast<std::uint8_t>(value >> 8U));
    output->push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

struct ClientSession {
    int fd{-1};
    std::string remote_ip;
    std::uint16_t remote_port{0};
    std::vector<std::uint8_t> receive_buffer;
    std::vector<std::uint8_t> send_buffer;
    std::size_t send_offset{0};
    TimestampMs connected_at_ms{0};
    TimestampMs last_activity_at_ms{0};
    std::chrono::steady_clock::time_point last_activity_steady{};
    // 对端关闭写方向后仍可从本端读取响应；EOF 只停止继续收包，待完整 ADU 处理且
    // send_buffer 排空后才关闭连接。
    bool read_eof{false};
};

}  // namespace

// 构造 ModbusTcpServer 并保存运行依赖。
ModbusTcpServer::~ModbusTcpServer()
{
    stop();
}

// 启动 Modbus TCP 监听线程。
StatusCode ModbusTcpServer::start(
    const ModbusServerSettings& settings,
    std::shared_ptr<ModbusRegisterBank> register_bank,
    std::string* error_message)
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (io_thread_.joinable() || running_.load(std::memory_order_acquire)) {
        if (error_message != nullptr) *error_message = "Modbus TCP Server 已经启动";
        return StatusCode::kInvalidState;
    }

    auto normalized = settings;
    normalize_modbus_server_settings(&normalized);
    std::string validation_error;
    const auto validation = validate_modbus_server_settings(
        normalized, "modbus_server_settings", &validation_error);
    if (!is_ok(validation)) {
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            runtime_status_ = {};
            runtime_status_.configured_enabled = normalized.enabled;
            runtime_status_.state = "error";
            runtime_status_.listen_address = normalized.listen_address;
            runtime_status_.listen_port = normalized.listen_port;
            runtime_status_.unit_id = normalized.unit_id;
            runtime_status_.last_error_message = validation_error;
        }
        if (error_message != nullptr) *error_message = validation_error;
        return validation;
    }

    settings_ = normalized;
    std::atomic_store_explicit(&register_bank_, std::move(register_bank), std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        runtime_status_ = {};
        runtime_status_.configured_enabled = normalized.enabled;
        runtime_status_.state = normalized.enabled ? "starting" : "disabled";
        runtime_status_.listen_address = normalized.listen_address;
        runtime_status_.listen_port = normalized.listen_port;
        runtime_status_.unit_id = normalized.unit_id;
    }
    if (!normalized.enabled) return StatusCode::kOk;

    int pipe_fds[2]{-1, -1};
    if (::pipe(pipe_fds) != 0 || !set_nonblocking(pipe_fds[0]) || !set_nonblocking(pipe_fds[1])) {
        if (pipe_fds[0] >= 0) ::close(pipe_fds[0]);
        if (pipe_fds[1] >= 0) ::close(pipe_fds[1]);
        const auto message = socket_error("创建 Modbus Server 唤醒管道失败");
        set_runtime_error(message);
        if (error_message != nullptr) *error_message = message;
        return StatusCode::kIoError;
    }
    wakeup_read_fd_ = pipe_fds[0];
    wakeup_write_fd_ = pipe_fds[1];
    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        startup_complete_ = false;
        startup_status_ = StatusCode::kInvalidState;
        startup_error_.clear();
    }

    try {
        io_thread_ = std::thread(&ModbusTcpServer::io_thread_entry, this);
    } catch (const std::exception& error) {
        const auto message = "创建 Modbus TCP Server I/O 线程失败：" + std::string(error.what());
        close_wakeup_pipe();
        set_runtime_error(message);
        if (error_message != nullptr) *error_message = message;
        return StatusCode::kInternalError;
    }

    StatusCode result = StatusCode::kInternalError;
    std::string startup_error;
    {
        std::unique_lock<std::mutex> lock(startup_mutex_);
        startup_cv_.wait(lock, [this]() { return startup_complete_; });
        result = startup_status_;
        startup_error = startup_error_;
    }
    if (!is_ok(result)) {
        if (io_thread_.joinable()) io_thread_.join();
        close_wakeup_pipe();
        if (error_message != nullptr) *error_message = startup_error;
        return result;
    }
    return StatusCode::kOk;
}

// 停止 Modbus TCP 监听线程并关闭连接。
void ModbusTcpServer::stop()
{
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!io_thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(status_mutex_);
        runtime_status_.running = false;
        runtime_status_.listening = false;
        if (runtime_status_.configured_enabled && runtime_status_.state != "error") {
            runtime_status_.state = "stopped";
        } else if (!runtime_status_.configured_enabled) {
            runtime_status_.state = "disabled";
        }
        return;
    }

    if (running_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        runtime_status_.state = "stopping";
    }
    stop_requested_.store(true, std::memory_order_release);
    if (wakeup_write_fd_ >= 0) {
        const std::uint8_t byte = 1;
        const auto ignored = ::write(wakeup_write_fd_, &byte, sizeof(byte));
        (void)ignored;
    }
    io_thread_.join();
    close_wakeup_pipe();
}

// 使用新设置重启 Modbus TCP 服务。
StatusCode ModbusTcpServer::restart(
    const ModbusServerSettings& settings,
    std::shared_ptr<ModbusRegisterBank> register_bank,
    std::string* error_message)
{
    stop();
    return start(settings, std::move(register_bank), error_message);
}

// 替换寄存器寄存器库。
void ModbusTcpServer::replace_register_bank(std::shared_ptr<ModbusRegisterBank> register_bank)
{
    std::atomic_store_explicit(&register_bank_, std::move(register_bank), std::memory_order_release);
}

// 判断 Modbus TCP 服务是否正在运行。
bool ModbusTcpServer::is_running() const
{
    return running_.load(std::memory_order_acquire);
}

// 获取运行态状态。
ModbusServerRuntimeStatus ModbusTcpServer::get_runtime_status() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return runtime_status_;
}

// 设置启动结果。
void ModbusTcpServer::set_startup_result(StatusCode status, const std::string& error_message)
{
    {
        std::lock_guard<std::mutex> lock(startup_mutex_);
        if (startup_complete_) return;
        startup_status_ = status;
        startup_error_ = error_message;
        startup_complete_ = true;
    }
    startup_cv_.notify_all();
}

// 设置运行态错误。
void ModbusTcpServer::set_runtime_error(const std::string& error_message)
{
    running_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(status_mutex_);
    runtime_status_.running = false;
    runtime_status_.listening = false;
    runtime_status_.current_connections = 0;
    runtime_status_.state = "error";
    runtime_status_.last_error_message = error_message;
}

// 关闭线程唤醒管道的两端。
void ModbusTcpServer::close_wakeup_pipe()
{
    if (wakeup_read_fd_ >= 0) ::close(wakeup_read_fd_);
    if (wakeup_write_fd_ >= 0) ::close(wakeup_write_fd_);
    wakeup_read_fd_ = -1;
    wakeup_write_fd_ = -1;
}

// 运行 Modbus TCP 服务端的非阻塞网络事件循环。
void ModbusTcpServer::io_thread_entry()
{
    // 创建非阻塞监听套接字并绑定配置地址。
    int listen_fd = -1;
    std::vector<ClientSession> clients;
    try {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) throw std::runtime_error(socket_error("创建 Modbus TCP 监听 Socket 失败"));
        const int reuse = 1;
        if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            throw std::runtime_error(socket_error("设置 Modbus TCP SO_REUSEADDR 失败"));
        }
        if (!set_nonblocking(listen_fd)) {
            throw std::runtime_error(socket_error("设置 Modbus TCP 监听 Socket 非阻塞失败"));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(settings_.listen_port);
        if (::inet_pton(AF_INET, settings_.listen_address.c_str(), &address.sin_addr) != 1) {
            throw std::runtime_error("Modbus TCP 监听地址不是有效 IPv4：" + settings_.listen_address);
        }
        if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error(socket_error("绑定 Modbus TCP 监听地址失败"));
        }
        if (::listen(listen_fd, static_cast<int>(settings_.max_clients)) != 0) {
            throw std::runtime_error(socket_error("监听 Modbus TCP Socket 失败"));
        }

        running_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            runtime_status_.running = true;
            runtime_status_.listening = true;
            runtime_status_.state = "listening";
            runtime_status_.started_at_ms = time_utils::system_now_ms();
            runtime_status_.last_error_message.clear();
        }
        set_startup_result(StatusCode::kOk, {});
        Logger::info(
            "Modbus TCP Server 已监听 " + settings_.listen_address + ":" +
            std::to_string(settings_.listen_port));

        // 封装客户端关闭、异常统计和响应缓冲操作。
        auto close_client = [&](int fd, const std::string& reason) {
            for (auto iterator = clients.begin(); iterator != clients.end(); ++iterator) {
                if (iterator->fd != fd) continue;
                if (Logger::debug_enabled()) {
                    Logger::debug("Modbus TCP 客户端断开 " + iterator->remote_ip + "：" + reason);
                }
                ::close(iterator->fd);
                clients.erase(iterator);
                std::lock_guard<std::mutex> lock(status_mutex_);
                runtime_status_.current_connections = static_cast<std::uint32_t>(clients.size());
                return;
            }
        };

        WarningRateLimiter malformed_warning_limiter;
        WarningRateLimiter connection_warning_limiter;
        auto mark_malformed = [&](const std::string& message) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                ++runtime_status_.malformed_request_count;
                runtime_status_.last_error_message = message;
            }
            malformed_warning_limiter.warn(message);
        };

        auto queue_response = [&](ClientSession& client, const std::vector<std::uint8_t>& response) {
            if (client.send_offset > 0) {
                client.send_buffer.erase(client.send_buffer.begin(), client.send_buffer.begin() + client.send_offset);
                client.send_offset = 0;
            }
            const auto pending = client.send_buffer.size() - client.send_offset;
            if (response.size() > kMaximumSendBufferSize - pending) {
                mark_malformed("Modbus TCP 客户端发送缓存超过上限：" + client.remote_ip);
                return false;
            }
            client.send_buffer.insert(client.send_buffer.end(), response.begin(), response.end());
            return true;
        };

        auto make_response = [](const std::uint8_t* request, const std::vector<std::uint8_t>& pdu) {
            std::vector<std::uint8_t> response;
            response.reserve(7 + pdu.size());
            response.push_back(request[0]);
            response.push_back(request[1]);
            response.push_back(0);
            response.push_back(0);
            append_u16_be(&response, static_cast<std::uint16_t>(pdu.size() + 1U));
            response.push_back(request[6]);
            response.insert(response.end(), pdu.begin(), pdu.end());
            return response;
        };

        // 校验并处理单个 Modbus TCP ADU，生成正常或异常响应。
        auto process_adu = [&](ClientSession& client, const std::uint8_t* request, std::size_t adu_size) {
            const auto length = static_cast<std::size_t>(read_u16_be(request + 4));
            const auto pdu_size = length - 1U;
            const auto* pdu = request + kMbapHeaderSize;
            const auto function = pdu[0];
            (void)adu_size;
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                ++runtime_status_.total_requests;
                runtime_status_.last_request_time_ms = time_utils::system_now_ms();
                runtime_status_.last_client_ip = client.remote_ip;
            }

            auto exception_response = [&](std::uint8_t exception_code) {
                const auto response = make_response(request, {
                    static_cast<std::uint8_t>(function | 0x80U), exception_code});
                if (!queue_response(client, response)) return false;
                std::lock_guard<std::mutex> lock(status_mutex_);
                ++runtime_status_.exception_responses;
                return true;
            };

            if (settings_.strict_unit_id && request[6] != settings_.unit_id) {
                // 固定返回 0x0B 且不关闭连接，状态页面据此说明目标设备当前不可用。
                return exception_response(0x0B);
            }
            if (function != 0x03) {
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    ++runtime_status_.unsupported_function_count;
                }
                return exception_response(0x01);
            }
            if (pdu_size != 5U) {
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    ++runtime_status_.invalid_value_count;
                }
                return exception_response(0x03);
            }

            const std::uint32_t start_address = read_u16_be(pdu + 1);
            const std::uint32_t register_count = read_u16_be(pdu + 3);
            if (register_count == 0 || register_count > settings_.max_read_registers) {
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    ++runtime_status_.invalid_value_count;
                }
                return exception_response(0x03);
            }
            if (start_address + register_count > 65536U) {
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    ++runtime_status_.invalid_address_count;
                }
                return exception_response(0x02);
            }

            const auto bank = std::atomic_load_explicit(&register_bank_, std::memory_order_acquire);
            if (bank == nullptr) return exception_response(0x04);
            std::vector<std::uint16_t> values;
            const auto read_status = bank->read_holding_registers(
                static_cast<RegisterAddress>(start_address),
                static_cast<RegisterCount>(register_count),
                &values,
                nullptr);
            if (!is_ok(read_status)) {
                if (read_status == StatusCode::kNotFound) {
                    {
                        std::lock_guard<std::mutex> lock(status_mutex_);
                        ++runtime_status_.invalid_address_count;
                    }
                    return exception_response(0x02);
                }
                return exception_response(0x04);
            }

            std::vector<std::uint8_t> response_pdu;
            response_pdu.reserve(2U + values.size() * 2U);
            response_pdu.push_back(0x03);
            response_pdu.push_back(static_cast<std::uint8_t>(values.size() * 2U));
            for (const auto value : values) append_u16_be(&response_pdu, value);
            if (!queue_response(client, make_response(request, response_pdu))) return false;
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                ++runtime_status_.successful_requests;
            }
            return true;
        };

        // 主循环同时监听唤醒管道、服务端套接字和全部客户端。
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::vector<pollfd> poll_fds;
            poll_fds.reserve(clients.size() + 2U);
            poll_fds.push_back({wakeup_read_fd_, POLLIN, 0});
            poll_fds.push_back({listen_fd, POLLIN, 0});
            for (const auto& client : clients) {
                short events = client.read_eof ? 0 : POLLIN;
                if (client.send_offset < client.send_buffer.size()) events |= POLLOUT;
                poll_fds.push_back({client.fd, events, 0});
            }

            const auto poll_result = ::poll(poll_fds.data(), static_cast<nfds_t>(poll_fds.size()), 1000);
            if (poll_result < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(socket_error("Modbus TCP poll 失败"));
            }
            if ((poll_fds[0].revents & POLLIN) != 0) {
                std::uint8_t drain[64];
                while (::read(wakeup_read_fd_, drain, sizeof(drain)) > 0) {}
                if (stop_requested_.load(std::memory_order_acquire)) break;
            }

            if ((poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                throw std::runtime_error("Modbus TCP 监听 Socket 进入错误状态");
            }

            // 接受新连接，并执行客户端数量和非阻塞模式检查。
            if ((poll_fds[1].revents & POLLIN) != 0) {
                for (;;) {
                    sockaddr_in remote{};
                    socklen_t remote_size = sizeof(remote);
                    const auto client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&remote), &remote_size);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        connection_warning_limiter.warn(socket_error("接受 Modbus TCP 客户端失败"));
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(status_mutex_);
                        ++runtime_status_.total_connections;
                    }
                    if (clients.size() >= settings_.max_clients) {
                        connection_warning_limiter.warn("Modbus TCP 客户端数已达到上限，拒绝新连接");
                        ::close(client_fd);
                        std::lock_guard<std::mutex> lock(status_mutex_);
                        ++runtime_status_.rejected_connection_count;
                        continue;
                    }
                    if (!set_nonblocking(client_fd)) {
                        connection_warning_limiter.warn(socket_error("设置 Modbus TCP 客户端 Socket 非阻塞失败"));
                        ::close(client_fd);
                        std::lock_guard<std::mutex> lock(status_mutex_);
                        ++runtime_status_.rejected_connection_count;
                        continue;
                    }
                    char remote_ip[INET_ADDRSTRLEN]{};
                    ::inet_ntop(AF_INET, &remote.sin_addr, remote_ip, sizeof(remote_ip));
                    ClientSession client;
                    client.fd = client_fd;
                    client.remote_ip = remote_ip;
                    client.remote_port = ntohs(remote.sin_port);
                    client.connected_at_ms = time_utils::system_now_ms();
                    client.last_activity_at_ms = client.connected_at_ms;
                    client.last_activity_steady = std::chrono::steady_clock::now();
                    client.receive_buffer.reserve(kMaximumAduSize * 2U);
                    clients.push_back(std::move(client));
                    if (Logger::debug_enabled()) {
                        Logger::debug("Modbus TCP 客户端已连接：" + clients.back().remote_ip);
                    }
                    {
                        std::lock_guard<std::mutex> lock(status_mutex_);
                        runtime_status_.current_connections = static_cast<std::uint32_t>(clients.size());
                    }
                }
            }

            // 读取客户端数据、拆分完整 ADU，并发送已排队响应。
            for (std::size_t poll_index = 2; poll_index < poll_fds.size(); ++poll_index) {
                const auto fd = poll_fds[poll_index].fd;
                const auto revents = poll_fds[poll_index].revents;
                auto found = std::find_if(clients.begin(), clients.end(), [&](const ClientSession& item) {
                    return item.fd == fd;
                });
                if (found == clients.end()) continue;
                if ((revents & (POLLERR | POLLNVAL)) != 0) {
                    close_client(fd, "Socket 错误");
                    continue;
                }

                bool close_requested = false;
                std::string close_reason;
                // POLLHUP 可能与尚未读取的数据同时出现，也可能表示对端仅关闭写方向。
                // 两种情况都先 drain recv，再解析完整 ADU，不能在看到 HUP 时直接丢包。
                if (!found->read_eof && (revents & (POLLIN | POLLHUP)) != 0) {
                    std::uint8_t input[2048];
                    for (;;) {
                        const auto received = ::recv(fd, input, sizeof(input), 0);
                        if (received > 0) {
                            const auto size = static_cast<std::size_t>(received);
                            if (size > kMaximumReceiveBufferSize - found->receive_buffer.size()) {
                                mark_malformed("Modbus TCP 客户端接收缓存超过上限：" + found->remote_ip);
                                close_requested = true;
                                close_reason = "接收缓存超限";
                                break;
                            }
                            found->receive_buffer.insert(found->receive_buffer.end(), input, input + size);
                            found->last_activity_at_ms = time_utils::system_now_ms();
                            found->last_activity_steady = std::chrono::steady_clock::now();
                            continue;
                        }
                        if (received == 0) {
                            found->read_eof = true;
                        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            close_requested = true;
                            close_reason = socket_error("接收失败");
                        }
                        if (errno == EINTR && received < 0) continue;
                        break;
                    }

                    std::size_t consumed = 0;
                    while (!close_requested &&
                           found->receive_buffer.size() - consumed >= kMbapHeaderSize) {
                        const auto* request = found->receive_buffer.data() + consumed;
                        const auto protocol_id = read_u16_be(request + 2);
                        const auto length = static_cast<std::size_t>(read_u16_be(request + 4));
                        if (protocol_id != 0 || length < 2U || length > kMaximumAduSize - 6U) {
                            mark_malformed("Modbus TCP MBAP Header 非法，客户端=" + found->remote_ip);
                            close_requested = true;
                            close_reason = "MBAP Header 非法";
                            break;
                        }
                        const auto adu_size = 6U + length;
                        if (found->receive_buffer.size() - consumed < adu_size) break;
                        if (!process_adu(*found, request, adu_size)) {
                            close_requested = true;
                            close_reason = "发送缓存超限";
                            break;
                        }
                        consumed += adu_size;
                    }
                    if (consumed > 0) {
                        found->receive_buffer.erase(
                            found->receive_buffer.begin(), found->receive_buffer.begin() + consumed);
                    }
                }

                if (!close_requested && found->send_offset < found->send_buffer.size() &&
                    (revents & (POLLIN | POLLOUT | POLLHUP)) != 0) {
                    while (found->send_offset < found->send_buffer.size()) {
                        const auto* data = found->send_buffer.data() + found->send_offset;
                        const auto remaining = found->send_buffer.size() - found->send_offset;
                        const auto sent = ::send(fd, data, remaining, MSG_NOSIGNAL);
                        if (sent > 0) {
                            found->send_offset += static_cast<std::size_t>(sent);
                            found->last_activity_at_ms = time_utils::system_now_ms();
                            found->last_activity_steady = std::chrono::steady_clock::now();
                            continue;
                        }
                        if (sent < 0 && errno == EINTR) continue;
                        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                        close_requested = true;
                        close_reason = socket_error("发送失败");
                        break;
                    }
                    if (!close_requested && found->send_offset == found->send_buffer.size()) {
                        found->send_buffer.clear();
                        found->send_offset = 0;
                    }
                }
                if (close_requested) {
                    close_client(fd, close_reason);
                } else if (found->read_eof &&
                           found->send_offset == found->send_buffer.size()) {
                    close_client(fd, "客户端写方向已关闭，响应已发送完成");
                }
            }

            // 回收超过空闲时限的客户端连接。
            const auto idle_limit = std::chrono::seconds(settings_.idle_timeout_seconds);
            const auto steady_now = std::chrono::steady_clock::now();
            std::vector<int> idle_fds;
            for (const auto& client : clients) {
                if (steady_now - client.last_activity_steady >= idle_limit) idle_fds.push_back(client.fd);
            }
            for (const auto fd : idle_fds) close_client(fd, "空闲超时");
        }

        // 正常停止时关闭连接并更新运行状态。
        for (const auto& client : clients) ::close(client.fd);
        clients.clear();
        ::close(listen_fd);
        listen_fd = -1;
        running_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            runtime_status_.running = false;
            runtime_status_.listening = false;
            runtime_status_.current_connections = 0;
            runtime_status_.state = runtime_status_.configured_enabled ? "stopped" : "disabled";
        }
        Logger::info("Modbus TCP Server 已停止");
    } catch (const std::exception& error) {
        for (const auto& client : clients) ::close(client.fd);
        if (listen_fd >= 0) ::close(listen_fd);
        const auto message = std::string(error.what());
        set_runtime_error(message);
        set_startup_result(StatusCode::kIoError, message);
        Logger::error("Modbus TCP Server 异常：" + message);
    } catch (...) {
        for (const auto& client : clients) ::close(client.fd);
        if (listen_fd >= 0) ::close(listen_fd);
        const std::string message = "Modbus TCP Server I/O 线程发生未知异常";
        set_runtime_error(message);
        set_startup_result(StatusCode::kInternalError, message);
        Logger::error(message);
    }
}

}  // namespace edge_controller
