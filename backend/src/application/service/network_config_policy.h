// 网络首次启动策略：仅在显式启用环境开关时要求持久化配置已由用户确认。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace edge_controller::network_config_policy {

inline bool require_explicit_config()
{
    const auto* raw = std::getenv("EDGE_CONTROLLER_REQUIRE_EXPLICIT_NETWORK_CONFIG");
    if (raw == nullptr) return false;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value == "1" || value == "true" || value == "yes";
}

}  // namespace edge_controller::network_config_policy
