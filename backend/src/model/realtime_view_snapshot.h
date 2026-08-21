// 通道、主站、设备和实时值的一致性页面快照模型。
#pragma once

#include <cstdint>
#include <vector>

#include "model/channel_config.h"
#include "model/device_config.h"
#include "model/device_realtime.h"
#include "model/master_node_config.h"
#include "model/system_status.h"

namespace edge_controller {

struct RealtimeViewSnapshot {
    std::uint64_t device_template_generation{0};
    std::vector<DeviceConfig> devices;
    std::vector<ChannelConfig> channels;
    std::vector<MasterNodeConfig> masters;
    SystemStatus system_status;
    std::vector<DeviceRealtimeSnapshot> device_realtime_snapshots;
};

}  // namespace edge_controller
