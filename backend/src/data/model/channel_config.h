// 串口与 TCP 通道的统一持久化配置模型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/common/enums.h"
#include "shared/common/types.h"

namespace edge_controller {

struct ChannelConfig {
    ChannelId channel_id;
    std::string channel_name;
    bool enabled{true};
    ChannelType channel_type{ChannelType::kUnknown};

    std::string device_path;
    std::string port_name;
    std::uint32_t baud_rate{9600};
    std::uint8_t data_bits{8};
    SerialParity parity{SerialParity::kNone};
    std::uint8_t stop_bits{1};
    std::uint32_t response_timeout_ms{500};
    std::uint32_t retry_count{2};

    std::string tcp_host;
    std::uint16_t tcp_port{502};
    std::uint32_t connect_timeout_ms{3000};
};

// 统一生成通道目标文本，日志、诊断和通讯跟踪必须使用同一套 TCP/串口回退规则。
// 少数只面向串口的调用点可传入更具体的未配置提示，但不能自行重复拼接目标。
inline std::string channel_target_description(
    const ChannelConfig& channel,
    const char* unconfigured_serial_text = "未配置通道目标")
{
    if (channel.channel_type == ChannelType::kModbusTcp) {
        return channel.tcp_host.empty()
                   ? "未配置 TCP 目标"
                   : channel.tcp_host + ":" + std::to_string(channel.tcp_port);
    }
    if (!channel.device_path.empty()) {
        return channel.device_path;
    }
    if (!channel.port_name.empty()) {
        return channel.port_name;
    }
    return unconfigured_serial_text;
}

// 轮询规划允许暂时找不到通道配置；空指针必须转换为稳定诊断文本。
inline std::string channel_target_description(const ChannelConfig* channel)
{
    return channel == nullptr
               ? "未知通道"
               : channel_target_description(*channel, "未配置串口设备");
}

// 通道运行态会为每条 TCP 通道分配独立 worker，因此全部已配置通道（含停用项）必须有统一硬边界。
inline constexpr std::size_t kMaxConfiguredChannelCount = 32;
inline constexpr std::size_t kMaxConfiguredTcpChannelCount = 16;
static_assert(kMaxConfiguredTcpChannelCount <= kMaxConfiguredChannelCount);

// 校验统一的通道容量边界。调用方可在创建、导入、持久化加载和运行态启动前复用同一口径。
inline bool validate_channel_count_limits(
    const std::vector<ChannelConfig>& channels,
    std::string* error_message = nullptr)
{
    std::size_t tcp_channel_count = 0;
    for (const auto& channel : channels) {
        if (channel.channel_type == ChannelType::kModbusTcp) {
            ++tcp_channel_count;
        }
    }

    if (tcp_channel_count > kMaxConfiguredTcpChannelCount) {
        if (error_message != nullptr) {
            *error_message =
                "Modbus TCP 通道数超过上限：当前 " + std::to_string(tcp_channel_count) +
                " 个，最多允许 " + std::to_string(kMaxConfiguredTcpChannelCount) + " 个";
        }
        return false;
    }
    if (channels.size() > kMaxConfiguredChannelCount) {
        if (error_message != nullptr) {
            *error_message =
                "通道总数超过上限：当前 " + std::to_string(channels.size()) +
                " 个，最多允许 " + std::to_string(kMaxConfiguredChannelCount) + " 个";
        }
        return false;
    }
    return true;
}

}  // namespace edge_controller
