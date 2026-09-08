// 实现 Modbus RTU 串口的打开、参数配置、收发、超时与关闭。
// 边界：只处理传输与资源，不解释寄存器业务语义。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "communication/channel/i_channel.h"
#include "communication/channel/channel_wake_event.h"

namespace edge_controller {

namespace channel_internal {

// 这些纯函数供串口发送阶段和单元测试共用，避免把响应超时误当作物理发送预算。
std::uint32_t serial_bits_per_character(const ChannelConfig& config) noexcept;
std::uint64_t serial_frame_wire_time_ms(const ChannelConfig& config, std::size_t frame_bytes) noexcept;
std::uint64_t serial_send_budget_ms(const ChannelConfig& config, std::size_t frame_bytes) noexcept;
std::uint64_t serial_tail_guard_ms(const ChannelConfig& config) noexcept;
// 根据已接收的标准 Modbus RTU 响应头返回完整帧长度；头部不足或未知功能码时返回 0。
std::size_t modbus_rtu_expected_response_length(
    const std::vector<std::uint8_t>& response) noexcept;

}  // namespace channel_internal

// 基于 termios 和 Linux 串口设备节点实现的串口通道。
class SerialChannel : public IChannel {
public:

    explicit SerialChannel(ChannelConfig config);

    ~SerialChannel() override;

    // 打开并配置 Linux 串口设备。
    StatusCode open() override;
    // 关闭串口设备。
    void close() override;
    // 判断串口设备是否已打开。
    bool is_open() const override;
    // 清空串口输入输出缓冲。
    StatusCode flush() override;
    // 返回串口文件描述符。
    int native_handle() const override;
    // 发送 Modbus RTU 请求并读取响应帧。
    StatusCode transceive(
        const std::vector<std::uint8_t>& request,
        int timeout_ms,
        std::vector<std::uint8_t>* response,
        const ChannelTraceContext& trace_context = {},
        const std::atomic<bool>* cancel_requested = nullptr,
        const ChannelResponseValidator& response_validator = {}) override;

    // 返回串口通道配置。
    const ChannelConfig& config() const override;
    // 返回串口通道最近运行状态。
    ChannelStatus status() const override;

private:
    // 在已持锁状态下打开并配置串口设备。
    StatusCode open_locked();
    // 在已持锁状态下关闭串口设备。
    void close_locked();
    // 在已持锁状态下判断串口是否打开。
    bool is_open_locked() const;
    // 在已持锁状态下清空串口缓冲。
    StatusCode flush_locked();
    // 记录串口通道错误并更新状态。
    void set_error_locked(const std::string& error_message,
        DiagnosisErrorCode code = DiagnosisErrorCode::kChannelIoError);

    ChannelConfig config_{};
    ChannelStatus status_{};
    std::string last_terminal_error_message_;
    int fd_{-1};
    // close() 先在锁外置位并唤醒 poll，避免等待正在阻塞 I/O 的 mutex_。
    std::atomic<bool> close_requested_{false};
    channel_internal::ChannelWakeEvent close_wakeup_;
    // 多个并发 close 必须各自完成一次置位、唤醒、回收和复位，不能互相清除通知。
    mutable std::mutex close_mutex_;
    // 串口文件描述符与状态不可分割；open/close/transceive/flush 由同一锁保证顺序访问。
    mutable std::mutex mutex_;
};

}  // namespace edge_controller
