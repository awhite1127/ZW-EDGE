// 定义通信通道抽象，隔离采集层与串口、TCP 等具体传输实现。
// 边界：只处理传输与资源，不解释寄存器业务语义。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shared/common/status_code.h"
#include "data/model/channel_config.h"
#include "data/model/channel_status.h"

namespace edge_controller {

struct ChannelTraceContext {
    std::string channel_id;
    std::string channel_name;
    std::string master_id;
    std::string master_name;
    std::string target;
    std::string device_id;
    std::string block_key;
    std::string block_display_name;
};

// 在通道事务锁仍被持有时校验完整响应。
// 回调不得再次调用同一通道；将 connection_reusable 置为 false 可在下一事务开始前关闭连接。
using ChannelResponseValidator = std::function<StatusCode(
    const std::vector<std::uint8_t>& response,
    bool* connection_reusable)>;

// 定义最小化的字节流通道接口，用于一次请求对应一次响应的通道收发。
class IChannel {
public:

    virtual ~IChannel() = default;

    // 打开通道底层连接。
    virtual StatusCode open() = 0;
    // 关闭通道底层连接。
    virtual void close() = 0;
    // 判断通道当前是否已打开。
    virtual bool is_open() const = 0;
    // 清理通道输入输出缓冲。
    virtual StatusCode flush() = 0;
    // 返回底层文件描述符或 socket 句柄。
    virtual int native_handle() const = 0;
    // 发送一次请求并读取对应响应。
    virtual StatusCode transceive(
        const std::vector<std::uint8_t>& request,
        int timeout_ms,
        std::vector<std::uint8_t>* response,
        const ChannelTraceContext& trace_context = {},
        const std::atomic<bool>* cancel_requested = nullptr,
        const ChannelResponseValidator& response_validator = {}) = 0;

    // 返回通道配置。
    virtual const ChannelConfig& config() const = 0;
    // 返回通道最近运行状态。
    virtual ChannelStatus status() const = 0;
};

}  // namespace edge_controller
