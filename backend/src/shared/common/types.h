// 后端跨模块使用的基础 ID、时间戳和通用类型别名。
#pragma once

#include <cstdint>
#include <string>

namespace edge_controller {

using TimestampMs = std::uint64_t;
using ChannelId = std::string;
using MasterNodeId = std::string;
using DeviceId = std::string;
using RegisterAddress = std::uint16_t;
using RegisterCount = std::uint16_t;

}  // namespace edge_controller
