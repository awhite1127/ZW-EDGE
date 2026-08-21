// 设备名称、安装位置和页面标题等基础系统标识模型。
#pragma once

#include <string>

namespace edge_controller {

inline constexpr const char kDefaultSystemDeviceName[] = "边缘计算控制器";
inline constexpr const char kDefaultSystemSiteLocation[] = "未设置";
inline constexpr const char kDefaultSystemDisplayName[] = "珠海知更通讯管理系统";

struct SystemSettings {
    std::string device_name{kDefaultSystemDeviceName};
    std::string site_location{kDefaultSystemSiteLocation};
    std::string display_name{kDefaultSystemDisplayName};
};

struct SystemSettingsUpdateRequest {
    std::string device_name;
    std::string site_location;
    std::string display_name;
};

struct SystemSettingsUpdateResult {
    SystemSettings settings;
    std::string message;
};

}  // namespace edge_controller
