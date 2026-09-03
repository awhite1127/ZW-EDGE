// 实现 Modbus TCP 连接的建立、收发、超时、重连与资源释放。
// 边界：只处理传输与资源，不解释寄存器业务语义。

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "communication/channel/bounded_address_resolver.h"
#include "communication/channel/i_channel.h"
#include "communication/channel/channel_wake_event.h"
#include "shared/common/types.h"
#include "data/model/diagnosis_status.h"

namespace edge_controller {

// 基于 POSIX socket 的 Modbus TCP 通道。
class TcpChannel : public IChannel {
public:
    // 构造 TcpChannel 实例。
    explicit TcpChannel(
        ChannelConfig config,
        channel_internal::AddressResolverFunction resolver = nullptr);
    // 销毁 TcpChannel 实例并释放相关资源。
    ~TcpChannel() override;

    // 打开 Modbus TCP socket 连接。
    StatusCode open() override;
    // 关闭 TCP socket 连接。
    void close() override;
    // 判断 TCP socket 当前是否已连接。
    bool is_open() const override;
    // TCP 通道无本地缓冲，保持接口一致。
    StatusCode flush() override;
    // 返回 TCP socket 文件描述符。
    int native_handle() const override;
    // 发送 Modbus TCP 请求并读取完整响应。
    StatusCode transceive(
        const std::vector<std::uint8_t>& request,
        int timeout_ms,
        std::vector<std::uint8_t>* response,
        const ChannelTraceContext& trace_context = {},
        const std::atomic<bool>* cancel_requested = nullptr,
        const ChannelResponseValidator& response_validator = {}) override;

    // 返回 TCP 通道配置。
    const ChannelConfig& config() const override;
    // 返回 TCP 通道最近运行状态。
    ChannelStatus status() const override;

private:
    // 在已持锁状态下建立 TCP 连接。
    StatusCode open_locked(const std::atomic<bool>* cancel_requested = nullptr);
    // 在已持锁状态下关闭 TCP 连接。
    void close_locked();
    // 在已持锁状态下判断 TCP 是否连接。
    bool is_open_locked() const;
    // 在已持锁状态下发送完整请求数据。
    StatusCode send_all_locked(
        const std::vector<std::uint8_t>& request,
        TimestampMs deadline_ms,
        const std::atomic<bool>* cancel_requested);
    // 在已持锁状态下按截止时间读取指定长度响应。
    StatusCode read_exact_until_locked(
        std::size_t expected_size,
        TimestampMs deadline_ms,
        std::vector<std::uint8_t>* response,
        const std::atomic<bool>* cancel_requested);
    // 记录 TCP 通道错误并更新诊断状态。
    void set_error_locked(
        const std::string& error_message,
        DiagnosisErrorCode error_code);
    // 返回 TCP 通道目标地址描述。
    std::string endpoint() const;

    ChannelConfig config_{};
    ChannelStatus status_{};
    std::string last_terminal_error_message_;
    int fd_{-1};
    // close() 可在主锁外请求取消，并立即唤醒 DNS/connect/send/recv 的 poll。
    std::atomic<bool> close_requested_{false};
    channel_internal::ChannelWakeEvent close_wakeup_;
    mutable std::mutex close_mutex_;
    // 默认使用系统 getaddrinfo；可注入阻塞 resolver 以直接验证超时和停止语义。
    channel_internal::AddressResolverFunction resolver_{nullptr};
    // TCP 连接和一次完整请求响应共用此锁，防止并发请求在同一字节流上交叉读写。
    mutable std::mutex mutex_;
};

}  // namespace edge_controller
