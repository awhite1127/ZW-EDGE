// 校验读取模型、数量和地址，并原子生成自动设备清单。
#pragma once

#include <string>
#include <vector>

#include "data/model/device_config.h"
#include "data/model/master_node_config.h"
#include "shared/common/status_code.h"

namespace edge_controller {

// 输出只在全部主站合法时替换；名称和 ID 与现有持久化/IPC 契约保持一致。
StatusCode derive_devices_from_masters(
    const std::vector<MasterNodeConfig>& masters,
    std::vector<DeviceConfig>& devices,
    std::vector<std::string>* errors = nullptr);

}  // namespace edge_controller
