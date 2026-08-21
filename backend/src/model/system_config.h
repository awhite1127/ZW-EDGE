// 聚合后端当前生效的系统、通道、主站、自动设备及外部服务配置。
// 边界：仅承载值语义；字段变更需同步检查存储、IPC、Web 与 MQTT。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/enums.h"
#include "model/channel_config.h"
#include "model/device_config.h"
#include "model/master_node_config.h"
#include "model/mqtt_settings.h"
#include "model/network_settings.h"
#include "model/system_settings.h"
#include "model/time_settings.h"

namespace edge_controller {

// 保存后端进程启动所需的静态配置快照。

struct SystemConfig {
    std::string project_name;
    std::string project_version;
    std::string site_id;
    std::string data_directory;
    std::string log_directory;
    LogLevel log_level{LogLevel::kInfo};
    std::uint32_t default_poll_interval_ms{1000};
    std::uint32_t max_channel_reconnect_interval_ms{5000};
    SystemSettings settings{};
    TimeSettings time_settings{};
    NetworkSettings network_settings{};
    // 仅由用户保存并应用或配置导入确认；全新数据库的默认网络值不代表现场配置。
    bool network_settings_explicitly_configured{false};
    MqttSettings mqtt_settings{};

    std::vector<ChannelConfig> channels;
    std::vector<MasterNodeConfig> master_nodes;
    std::vector<DeviceConfig> devices;
};

}  // namespace edge_controller
