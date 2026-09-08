// 串口通道实现：封装 termios、读写超时和重连状态；上层轮询不得绕过本类直接操作 fd。
#include "communication/channel/serial_channel.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#include "shared/common/format_utils.h"
#include "shared/common/logger.h"
#include "shared/common/time_utils.h"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace edge_controller {

namespace {

#if defined(__linux__)
// 将波特率数值转换为系统串口常量。
speed_t to_baud_rate(std::uint32_t baud_rate)
{
    switch (baud_rate) {
    case 1200:
        return B1200;
    case 2400:
        return B2400;
    case 4800:
        return B4800;
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    default:
        return 0;
    }
}
#endif

// 获取当前毫秒时间戳。
TimestampMs now_ms()
{
    return time_utils::system_now_ms();
}

bool serial_operation_cancelled(
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* close_requested)
{
    return (cancel_requested != nullptr && cancel_requested->load(std::memory_order_acquire)) ||
        (close_requested != nullptr && close_requested->load(std::memory_order_acquire));
}

#if defined(__linux__)
// 计算向上取整的除法结果。
std::uint64_t ceil_div(std::uint64_t value, std::uint64_t divisor)
{
    return divisor == 0 ? value : ((value + divisor - 1) / divisor);
}

// 根据波特率和字符间隔计算 RTU 帧间等待时间。
int rtu_interval_ms(
    const ChannelConfig& config,
    std::uint32_t tenths_of_char,
    int min_ms,
    int max_ms)
{
    if (config.baud_rate == 0) {
        return min_ms;
    }

    // 按实际起始位、数据位、校验位和停止位计算字符时间，
    // 再给高波特率保留少量调度余量；整帧响应超时仍由 timeout_ms 控制。
    const auto char_time_us = ceil_div(
        static_cast<std::uint64_t>(channel_internal::serial_bits_per_character(config)) *
            1000ULL * 1000ULL,
        config.baud_rate);
    const auto interval_us = ceil_div(char_time_us * tenths_of_char, 10ULL);
    const auto interval_ms = static_cast<int>(ceil_div(interval_us, 1000ULL));
    return std::clamp(interval_ms, min_ms, max_ms);
}

constexpr TimestampMs kCancellationPollMs = 20U;

StatusCode wait_for_serial_event_until(
    int serial_fd,
    short requested_events,
    int wake_fd,
    TimestampMs deadline_ms,
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* close_requested,
    const std::string& timeout_message,
    std::string* error_message)
{
    while (true) {
        if (serial_operation_cancelled(cancel_requested, close_requested)) {
            if (error_message != nullptr) {
                *error_message = "串口操作已取消";
            }
            return StatusCode::kInvalidState;
        }
        const auto now = time_utils::steady_now_ms();
        if (now >= deadline_ms) {
            if (error_message != nullptr) {
                *error_message = timeout_message;
            }
            return StatusCode::kTimeout;
        }

        auto wait_ms = deadline_ms - now;
        if (cancel_requested != nullptr || (close_requested != nullptr && wake_fd < 0)) {
            wait_ms = std::min(wait_ms, kCancellationPollMs);
        }
        pollfd descriptors[2]{};
        descriptors[0].fd = serial_fd;
        descriptors[0].events = requested_events | POLLERR | POLLHUP;
        nfds_t descriptor_count = 1;
        if (wake_fd >= 0) {
            descriptors[1].fd = wake_fd;
            descriptors[1].events = POLLIN;
            descriptor_count = 2;
        }

        int ready = 0;
        do {
            ready = ::poll(descriptors, descriptor_count, static_cast<int>(std::max<TimestampMs>(wait_ms, 1U)));
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            if (error_message != nullptr) {
                *error_message = "等待串口 I/O 失败: " + std::string(std::strerror(errno));
            }
            return StatusCode::kIoError;
        }
        if (ready == 0) {
            continue;
        }
        if (descriptor_count == 2 && (descriptors[1].revents & POLLIN) != 0) {
            continue;
        }
        if ((descriptors[0].revents & requested_events) != 0) {
            return StatusCode::kOk;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (error_message != nullptr) {
                *error_message = "串口设备在等待 I/O 时断开";
            }
            return StatusCode::kIoError;
        }
    }
}

StatusCode wait_for_serial_delay_until(
    TimestampMs delay_deadline_ms,
    int wake_fd,
    const std::atomic<bool>* cancel_requested,
    const std::atomic<bool>* close_requested)
{
    while (true) {
        if (serial_operation_cancelled(cancel_requested, close_requested)) {
            return StatusCode::kInvalidState;
        }
        const auto now = time_utils::steady_now_ms();
        if (now >= delay_deadline_ms) {
            return StatusCode::kOk;
        }
        auto wait_ms = delay_deadline_ms - now;
        if (cancel_requested != nullptr || (close_requested != nullptr && wake_fd < 0)) {
            wait_ms = std::min(wait_ms, kCancellationPollMs);
        }
        pollfd descriptor{};
        descriptor.fd = wake_fd;
        descriptor.events = POLLIN;
        int ready = 0;
        do {
            ready = ::poll(wake_fd >= 0 ? &descriptor : nullptr, wake_fd >= 0 ? 1U : 0U,
                           static_cast<int>(std::max<TimestampMs>(wait_ms, 1U)));
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {
            return StatusCode::kIoError;
        }
    }
}
#endif

// 返回通讯报文值，空值时使用兜底文本。
std::string trace_value(const std::string& value, const std::string& fallback)
{
    return value.empty() ? fallback : value;
}

// 根据收发方向生成 RTU 通讯报文日志前缀。
std::string rtu_trace_prefix(const ChannelConfig& config, const ChannelTraceContext& context)
{
    const auto channel_id = trace_value(context.channel_id, config.channel_id);
    const auto master_id = trace_value(context.master_id, "-");
    const auto serial_device = trace_value(context.target, config.device_path);
    std::string prefix = " channel_id=" + channel_id +
           " master_id=" + master_id +
           " serial_device=" + serial_device;
    if (!context.device_id.empty()) prefix += " device_id=" + context.device_id;
    if (!context.block_key.empty()) prefix += " block_key=" + context.block_key;
    return prefix;
}

// 计算从指定时刻起经过的时间。
std::uint32_t elapsed_since(TimestampMs started_at_ms)
{
    const auto now = time_utils::steady_now_ms();
    return now > started_at_ms ? static_cast<std::uint32_t>(now - started_at_ms) : 0U;
}

}  // namespace

namespace channel_internal {

namespace {

constexpr std::uint64_t kMaximumSerialSendBudgetMs = 60000U;

}  // namespace

std::uint32_t serial_bits_per_character(const ChannelConfig& config) noexcept
{
    const auto data_bits = std::clamp<std::uint32_t>(config.data_bits, 5U, 8U);
    const auto stop_bits = std::clamp<std::uint32_t>(config.stop_bits, 1U, 2U);
    const auto parity_bits = config.parity == SerialParity::kNone ? 0U : 1U;
    return 1U + data_bits + parity_bits + stop_bits;
}

std::uint64_t serial_frame_wire_time_ms(
    const ChannelConfig& config,
    std::size_t frame_bytes) noexcept
{
    if (frame_bytes == 0U) {
        return 0U;
    }
    if (config.baud_rate == 0U) {
        return kMaximumSerialSendBudgetMs;
    }

    const auto bits_per_character =
        static_cast<std::uint64_t>(serial_bits_per_character(config));
    const auto denominator = bits_per_character * 1000U;
    const auto maximum_uncapped_bytes =
        (kMaximumSerialSendBudgetMs * static_cast<std::uint64_t>(config.baud_rate)) /
        denominator;
    if (frame_bytes > maximum_uncapped_bytes) {
        return kMaximumSerialSendBudgetMs;
    }

    const auto numerator = static_cast<std::uint64_t>(frame_bytes) * denominator;
    return std::min<std::uint64_t>(
        (numerator + config.baud_rate - 1U) / config.baud_rate,
        kMaximumSerialSendBudgetMs);
}

std::uint64_t serial_send_budget_ms(
    const ChannelConfig& config,
    std::size_t frame_bytes) noexcept
{
    const auto wire_time_ms = serial_frame_wire_time_ms(config, frame_bytes);
    if (wire_time_ms >= kMaximumSerialSendBudgetMs) {
        return kMaximumSerialSendBudgetMs;
    }

    // 驱动排队、USB 串口批量提交和非实时调度需要有限余量；预算与响应超时相互独立。
    const auto scheduling_margin_ms = std::clamp<std::uint64_t>(wire_time_ms / 5U + 20U, 50U, 2000U);
    return std::clamp<std::uint64_t>(
        wire_time_ms + scheduling_margin_ms,
        100U,
        kMaximumSerialSendBudgetMs);
}

std::uint64_t serial_tail_guard_ms(const ChannelConfig& config) noexcept
{
    if (config.baud_rate == 0U) {
        return 100U;
    }
    const auto numerator =
        static_cast<std::uint64_t>(serial_bits_per_character(config)) * 1000U;
    return std::clamp<std::uint64_t>(
        (numerator + config.baud_rate - 1U) / config.baud_rate,
        1U,
        100U);
}

std::size_t modbus_rtu_expected_response_length(
    const std::vector<std::uint8_t>& response) noexcept
{
    if (response.size() < 2U) {
        return 0U;
    }
    const auto function_code = response[1];
    if ((function_code & 0x80U) != 0U) {
        return 5U;
    }
    if (function_code == 0x10U) {
        return 8U;
    }
    if ((function_code == 0x03U || function_code == 0x04U) && response.size() >= 3U) {
        return 3U + static_cast<std::size_t>(response[2]) + 2U;
    }
    return 0U;
}

}  // namespace channel_internal

SerialChannel::SerialChannel(ChannelConfig config)
    : config_(std::move(config))
{
    status_.channel_id = config_.channel_id;
    status_.configured = true;
    status_.enabled = config_.enabled;
    status_.device_path = config_.device_path;
    status_.status = config_.enabled ? "closed" : "disabled";
    status_.last_change_time_ms = now_ms();
    status_.diagnosis = make_normal_diagnosis(
        DiagnosisLevel::kChannel,
        config_.channel_id,
        config_.device_path,
        0);
}

SerialChannel::~SerialChannel()
{
    close();
}

// 打开并配置串口通道。
StatusCode SerialChannel::open()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_locked();
}

// 在持锁状态下打开。
StatusCode SerialChannel::open_locked()
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

#if !defined(__linux__)
    set_error_locked("串口通道仅支持 Linux", DiagnosisErrorCode::kChannelOpenFailed);
    return StatusCode::kInvalidState;
#else
    if (Logger::debug_enabled()) {
        Logger::debug("打开串口通道 " + config_.channel_id + "，串口路径=" + config_.device_path);
    }

    fd_ = ::open(config_.device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        set_error_locked("打开串口设备失败: " + std::string(std::strerror(errno)), DiagnosisErrorCode::kChannelOpenFailed);
        return StatusCode::kIoError;
    }

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        set_error_locked("读取串口参数失败: " + std::string(std::strerror(errno)), DiagnosisErrorCode::kChannelConfigFailed);
        close_locked();
        return StatusCode::kIoError;
    }

    const auto speed = to_baud_rate(config_.baud_rate);
    if (speed == 0) {
        set_error_locked("不支持的波特率: " + std::to_string(config_.baud_rate), DiagnosisErrorCode::kChannelConfigFailed);
        close_locked();
        return StatusCode::kInvalidArgument;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE);
    switch (config_.data_bits) {
    case 5:
        tty.c_cflag |= CS5;
        break;
    case 6:
        tty.c_cflag |= CS6;
        break;
    case 7:
        tty.c_cflag |= CS7;
        break;
    case 8:
        tty.c_cflag |= CS8;
        break;
    default:
        set_error_locked("不支持的数据位: " + std::to_string(config_.data_bits), DiagnosisErrorCode::kChannelConfigFailed);
        close_locked();
        return StatusCode::kInvalidArgument;
    }

    tty.c_iflag &= static_cast<unsigned int>(
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY));
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= static_cast<unsigned int>(~CRTSCTS);

    if (config_.stop_bits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= static_cast<unsigned int>(~CSTOPB);
    }

    tty.c_cflag &= static_cast<unsigned int>(~(PARENB | PARODD));
    if (config_.parity == SerialParity::kEven) {
        tty.c_cflag |= PARENB;
    } else if (config_.parity == SerialParity::kOdd) {
        tty.c_cflag |= PARENB;
        tty.c_cflag |= PARODD;
    }

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        set_error_locked("应用串口参数失败: " + std::string(std::strerror(errno)), DiagnosisErrorCode::kChannelConfigFailed);
        close_locked();
        return StatusCode::kIoError;
    }

    tcflush(fd_, TCIOFLUSH);

    status_.enabled = true;
    status_.opened = true;
    status_.status = "online";
    status_.last_open_time_ms = now_ms();
    status_.last_change_time_ms = status_.last_open_time_ms;
    status_.diagnosis = make_normal_diagnosis(
        DiagnosisLevel::kChannel,
        config_.channel_id,
        config_.device_path,
        status_.last_open_time_ms);
    status_.last_error_message.clear();
    last_terminal_error_message_.clear();
    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_channel_open_done channel_id=" + config_.channel_id +
            " serial_device=" + config_.device_path +
            " fd=" + std::to_string(fd_) +
            " success=true");
        Logger::debug(
            "rtu_channel_configure_done channel_id=" + config_.channel_id +
            " serial_device=" + config_.device_path +
            " fd=" + std::to_string(fd_) +
            " baud_rate=" + std::to_string(config_.baud_rate) +
            " data_bits=" + std::to_string(config_.data_bits) +
            " stop_bits=" + std::to_string(config_.stop_bits) +
            " parity=" + std::string(to_string(config_.parity)) +
            " success=true");
        Logger::debug("串口通道已打开: " + config_.channel_id);
    }
    return StatusCode::kOk;
#endif
}

// 关闭串口通道。
void SerialChannel::close()
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
void SerialChannel::close_locked()
{
#if defined(__linux__)
    if (fd_ >= 0) {
        if (tcflush(fd_, TCIOFLUSH) != 0) {
            Logger::warn(
                "rtu_channel_close_flush_failed channel_id=" + config_.channel_id +
                " serial_device=" + config_.device_path +
                " fd=" + std::to_string(fd_) +
                " error=\"" + std::string(std::strerror(errno)) + "\"");
        }
        ::close(fd_);
        fd_ = -1;
    }
#endif

    if (status_.opened) {
        status_.opened = false;
        status_.last_close_time_ms = now_ms();
        status_.last_change_time_ms = status_.last_close_time_ms;
        status_.status = config_.enabled ? "closed" : "disabled";
        if (Logger::debug_enabled()) {
            Logger::debug("串口通道已关闭: " + config_.channel_id);
        }
        return;
    }

    if (!config_.enabled) {
        status_.status = "disabled";
    } else if (status_.status.empty()) {
        status_.status = "closed";
    }
}

// 判断串口通道是否打开。
bool SerialChannel::is_open() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_locked();
}

// 在持锁状态下判断通道是否已打开。
bool SerialChannel::is_open_locked() const
{
    return status_.opened;
}

// 清空串口输入输出缓冲。
StatusCode SerialChannel::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_locked();
}

// 在持锁状态下刷新。
StatusCode SerialChannel::flush_locked()
{
#if !defined(__linux__)
    return StatusCode::kInvalidState;
#else
    if (fd_ < 0) {
        return StatusCode::kInvalidState;
    }
    if (tcflush(fd_, TCIOFLUSH) != 0) {
        set_error_locked("刷新串口缓冲区失败: " + std::string(std::strerror(errno)));
        return StatusCode::kIoError;
    }
    // open 成功只代表 fd 已创建；完整 prepare/flush 成功后才结束上一段连续错误。
    status_.consecutive_error_count = 0;
    status_.last_change_time_ms = now_ms();
    return StatusCode::kOk;
#endif
}

// 返回通道底层文件描述符；内部加锁保护句柄状态。
int SerialChannel::native_handle() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_;
}

// 发送 Modbus RTU 请求并读取响应。
StatusCode SerialChannel::transceive(
    const std::vector<std::uint8_t>& request,
    int timeout_ms,
    std::vector<std::uint8_t>* response,
    const ChannelTraceContext& trace_context,
    const std::atomic<bool>* cancel_requested,
    const ChannelResponseValidator& response_validator)
{
    // 串行化通道收发并校验输出参数。
    std::lock_guard<std::mutex> lock(mutex_);
    const auto transceive_started_at_ms = time_utils::steady_now_ms();
    std::string trace_prefix;
    bool trace_prefix_ready = false;
    const auto get_trace_prefix = [&]() -> const std::string& {
        if (!trace_prefix_ready) {
            trace_prefix = rtu_trace_prefix(config_, trace_context);
            trace_prefix_ready = true;
        }
        return trace_prefix;
    };

    if (response == nullptr) {
        set_error_locked("响应输出参数为空");
        Logger::warn(
            "rtu_response_error" + get_trace_prefix() +
            " error=\"响应输出参数为空\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
        return StatusCode::kInvalidArgument;
    }
    response->clear();
    if (serial_operation_cancelled(cancel_requested, &close_requested_)) {
        return StatusCode::kInvalidState;
    }

    // 通道未打开时先按当前配置尝试初始化。
    if (!is_open_locked()) {
        const auto open_status = open_locked();
        if (!is_ok(open_status)) {
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return open_status;
        }
    }

#if !defined(__linux__)
    (void)request;
    (void)timeout_ms;
    (void)cancel_requested;
    (void)response_validator;
    set_error_locked("串口收发仅支持 Linux");
    return StatusCode::kInvalidState;
#else
    if (request.empty()) {
        set_error_locked("请求帧为空");
        Logger::warn(
            "rtu_response_error" + get_trace_prefix() +
            " error=\"请求帧为空\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
        return StatusCode::kInvalidArgument;
    }

    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_request_send_start" + get_trace_prefix() +
            " request_bytes=" + std::to_string(request.size()));
        Logger::debug("通道 " + config_.channel_id + " 发送 Modbus RTU 请求，串口路径=" +
                      config_.device_path + "，帧=" + format_utils::bytes_to_hex(request));
    }

    tcflush(fd_, TCIFLUSH);
    const int frame_silence_ms = rtu_interval_ms(config_, 35, 5, 100);
    const auto silence_status = wait_for_serial_delay_until(
        time_utils::steady_now_ms() + static_cast<TimestampMs>(frame_silence_ms),
        close_wakeup_.native_handle(),
        cancel_requested,
        &close_requested_);
    if (!is_ok(silence_status)) {
        if (silence_status == StatusCode::kInvalidState) {
            return silence_status;
        }
        set_error_locked("等待 RTU 帧间隔失败");
        return StatusCode::kIoError;
    }

    // 写入和驱动排空使用按物理线速计算的独立预算；响应超时从发送真正完成后开始。
    const auto send_started_at_ms = time_utils::steady_now_ms();
    const auto send_deadline_ms = send_started_at_ms +
        static_cast<TimestampMs>(channel_internal::serial_send_budget_ms(config_, request.size()));
    const auto physical_send_not_before_ms = send_started_at_ms +
        static_cast<TimestampMs>(channel_internal::serial_frame_wire_time_ms(config_, request.size()));
    const auto tail_guard_ms =
        static_cast<TimestampMs>(channel_internal::serial_tail_guard_ms(config_));
    const auto wait_for_fallback_transmitter_empty = [&](TimestampMs observed_at_ms) {
        const auto fallback_target_ms = std::max<TimestampMs>(
            physical_send_not_before_ms,
            observed_at_ms + tail_guard_ms);
        const auto fallback_wait_status = wait_for_serial_delay_until(
            std::min<TimestampMs>(send_deadline_ms, fallback_target_ms),
            close_wakeup_.native_handle(),
            cancel_requested,
            &close_requested_);
        if (fallback_wait_status == StatusCode::kInvalidState) {
            return fallback_wait_status;
        }
        if (!is_ok(fallback_wait_status)) {
            set_error_locked("等待串口末字节发送完成失败");
            return StatusCode::kIoError;
        }
        if (fallback_target_ms > send_deadline_ms) {
            set_error_locked("等待串口输出完成超时", DiagnosisErrorCode::kModbusTimeout);
            return StatusCode::kTimeout;
        }
        return StatusCode::kOk;
    };
    std::size_t written = 0;
    while (written < request.size()) {
        std::string wait_error;
        const auto wait_status = wait_for_serial_event_until(
            fd_,
            POLLOUT,
            close_wakeup_.native_handle(),
            send_deadline_ms,
            cancel_requested,
            &close_requested_,
            "串口发送超时",
            &wait_error);
        if (!is_ok(wait_status)) {
            if (wait_status == StatusCode::kInvalidState) {
                return wait_status;
            }
            set_error_locked(wait_error);
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return wait_status;
        }
        const auto write_size = ::write(fd_, request.data() + written, request.size() - written);
        if (write_size < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            set_error_locked("串口写入失败: " + std::string(std::strerror(errno)));
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return StatusCode::kIoError;
        }
        if (write_size == 0) {
            set_error_locked("串口写入返回 0 字节");
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return StatusCode::kIoError;
        }
        written += static_cast<std::size_t>(write_size);
    }

    while (true) {
        if (serial_operation_cancelled(cancel_requested, &close_requested_)) {
            return StatusCode::kInvalidState;
        }
        const auto now = time_utils::steady_now_ms();
        if (now >= send_deadline_ms) {
            set_error_locked("等待串口输出完成超时", DiagnosisErrorCode::kModbusTimeout);
            return StatusCode::kTimeout;
        }
        int queued_bytes = 0;
        if (::ioctl(fd_, TIOCOUTQ, &queued_bytes) != 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error_locked("查询串口输出队列失败: " + std::string(std::strerror(errno)));
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return StatusCode::kIoError;
        }
        if (queued_bytes <= 0) {
#if defined(TIOCSERGETLSR) && defined(TIOCSER_TEMT)
            int line_status = 0;
            if (::ioctl(fd_, TIOCSERGETLSR, &line_status) == 0) {
                if ((line_status & TIOCSER_TEMT) != 0) {
                    break;
                }
            } else if (errno == EINTR) {
                continue;
            } else if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS ||
                       errno == EOPNOTSUPP) {
                // USB/虚拟串口通常不支持查询移位寄存器；保守等待整帧理论线速时间，
                // 若驱动排队本身已覆盖该时间，则仍补一个完整字符保护。
                const auto tail_status = wait_for_fallback_transmitter_empty(now);
                if (!is_ok(tail_status)) return tail_status;
                break;
            } else {
                set_error_locked("查询串口发送器状态失败: " + std::string(std::strerror(errno)));
                return StatusCode::kIoError;
            }
#else
            const auto tail_status = wait_for_fallback_transmitter_empty(now);
            if (!is_ok(tail_status)) return tail_status;
            break;
#endif
        }
        const auto delay_status = wait_for_serial_delay_until(
            std::min<TimestampMs>(send_deadline_ms, now + 5U),
            close_wakeup_.native_handle(),
            cancel_requested,
            &close_requested_);
        if (delay_status == StatusCode::kInvalidState) {
            return delay_status;
        }
        if (!is_ok(delay_status)) {
            set_error_locked("等待串口输出队列变化失败");
            return StatusCode::kIoError;
        }
    }

    status_.last_send_time_ms = now_ms();
    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_request_send_done" + get_trace_prefix() +
            " written_bytes=" + std::to_string(written) +
            " elapsed_ms=" + std::to_string(elapsed_since(send_started_at_ms)));
    }

    // 在总超时时限内分段等待并累计响应字节。
    const int response_timeout_ms = std::max(timeout_ms, 1);
    const int inter_byte_timeout_ms = rtu_interval_ms(config_, 15, 5, 100);
    const auto response_deadline_ms = time_utils::steady_now_ms() + static_cast<TimestampMs>(response_timeout_ms);
    const auto wait_started_at_ms = time_utils::steady_now_ms();
    auto next_read_deadline_ms = response_deadline_ms;
    std::size_t expected_response_length = 0U;
    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_wait_response_start" + get_trace_prefix() +
            " timeout_ms=" + std::to_string(response_timeout_ms));
    }

    while (true) {
        if (serial_operation_cancelled(cancel_requested, &close_requested_)) {
            return StatusCode::kInvalidState;
        }
        std::string wait_error;
        const auto wait_status = wait_for_serial_event_until(
            fd_,
            POLLIN,
            close_wakeup_.native_handle(),
            next_read_deadline_ms,
            cancel_requested,
            &close_requested_,
            "串口响应超时",
            &wait_error);
        if (wait_status == StatusCode::kTimeout) {
            break;
        }
        if (wait_status == StatusCode::kInvalidState) {
            return wait_status;
        }
        if (!is_ok(wait_status)) {
            set_error_locked(wait_error);
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return wait_status;
        }

        std::uint8_t buffer[512];
        const auto read_size = ::read(fd_, buffer, sizeof(buffer));
        if (read_size < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            set_error_locked("串口读取失败: " + std::string(std::strerror(errno)));
            Logger::warn(
                "rtu_response_error" + get_trace_prefix() +
                " error=\"" + status_.last_error_message +
                "\" elapsed_ms=" + std::to_string(elapsed_since(transceive_started_at_ms)));
            return StatusCode::kIoError;
        }
        if (read_size == 0) {
            break;
        }

        response->insert(response->end(), buffer, buffer + read_size);
        status_.last_receive_time_ms = now_ms();
        expected_response_length =
            channel_internal::modbus_rtu_expected_response_length(*response);
        if (expected_response_length != 0U && response->size() >= expected_response_length) {
            break;
        }
        // 标准响应一旦能从头部确定完整长度，就允许后续分批数据一直等到总响应超时。
        // 未知格式仍使用原有 RTU 字节间隔作为辅助帧结束条件。
        next_read_deadline_ms = expected_response_length == 0U
            ? std::min<TimestampMs>(
                  response_deadline_ms,
                  time_utils::steady_now_ms() + static_cast<TimestampMs>(inter_byte_timeout_ms))
            : response_deadline_ms;
    }

    // 区分无响应超时与成功接收，并更新通道诊断状态。
    if (response->empty()) {
        status_.status = "online";
        status_.last_change_time_ms = now_ms();
        if (Logger::debug_enabled()) {
            Logger::debug(
                "rtu_response_timeout" + get_trace_prefix() +
                " timeout_ms=" + std::to_string(response_timeout_ms) +
                " elapsed_ms=" + std::to_string(elapsed_since(wait_started_at_ms)));
            Logger::debug(
                "通道 " + config_.channel_id +
                " 未在超时时间内收到 Modbus RTU 响应，保持通道打开以便后续重试");
        }
        return StatusCode::kTimeout;
    }

    status_.consecutive_error_count = 0;
    status_.diagnosis = make_normal_diagnosis(
        DiagnosisLevel::kChannel,
        config_.channel_id,
        config_.device_path,
        status_.last_receive_time_ms);
    status_.last_error_message.clear();
    status_.status = "online";
    status_.last_change_time_ms = now_ms();
    last_terminal_error_message_.clear();
    if (Logger::debug_enabled()) {
        Logger::debug(
            "rtu_response_received" + get_trace_prefix() +
            " response_bytes=" + std::to_string(response->size()) +
            " elapsed_ms=" + std::to_string(elapsed_since(wait_started_at_ms)));
        Logger::debug("通道 " + config_.channel_id + " 收到 Modbus RTU 响应，串口路径=" +
                      config_.device_path + "，帧=" + format_utils::bytes_to_hex(*response));
    }
    if (response_validator) {
        bool connection_reusable = true;
        const auto validation_status = response_validator(*response, &connection_reusable);
        if (!connection_reusable) {
            close_locked();
        }
        return validation_status;
    }
    return StatusCode::kOk;
#endif
}

// 获取当前组件配置。
const ChannelConfig& SerialChannel::config() const
{
    return config_;
}

// 返回串口通道运行状态。
ChannelStatus SerialChannel::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

// 在持锁状态下设置错误信息。
void SerialChannel::set_error_locked(const std::string& error_message, DiagnosisErrorCode code)
{
#if defined(__linux__)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    status_.enabled = config_.enabled;
    status_.device_path = config_.device_path;
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
        config_.device_path,
        config_.enabled ? DiagnosisRunStatus::kError : DiagnosisRunStatus::kWarning,
        code,
        status_.last_receive_time_ms,
        status_.last_change_time_ms,
        status_.consecutive_error_count);
    status_.last_error_message = status_.diagnosis.message;
    if (last_terminal_error_message_ != error_message) {
        last_terminal_error_message_ = error_message;
        Logger::error(
            "通道 " + config_.channel_id +
            " 串口异常，路径=" + config_.device_path +
            "，原因=" + error_message);
    } else {
        if (Logger::debug_enabled()) {
            Logger::debug(
                "通道 " + config_.channel_id +
                " 串口异常重复发生，终端已降噪，路径=" + config_.device_path +
                "，原因=" + error_message);
        }
    }
}

}  // namespace edge_controller
