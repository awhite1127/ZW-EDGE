// static/DHCP 期望配置、应用结果和真实网口运行态模型。
#pragma once

#include <string>
#include <vector>

namespace edge_controller {

inline constexpr const char kDefaultNetworkInterfaceName[] = "eth0";
inline constexpr const char kDefaultNetworkIpAddress[] = "192.168.0.172";
inline constexpr const char kDefaultNetworkNetmask[] = "255.255.254.0";
inline constexpr const char kDefaultNetworkGateway[] = "192.168.1.1";
inline constexpr const char kDefaultNetworkDnsPrimary[] = "223.5.5.5";
inline constexpr const char kDefaultNetworkDnsSecondary[] = "8.8.8.8";
inline constexpr const char kNetworkModeStatic[] = "static";
inline constexpr const char kNetworkModeDhcp[] = "dhcp";
inline constexpr const char kStaticNetworkApplyModeText[] = "目标静态网络配置已保存";
inline constexpr const char kDhcpNetworkApplyModeText[] = "目标 DHCP 网络配置已保存";

struct NetworkSettings {
    std::string mode{kNetworkModeStatic};
    std::string interface_name{kDefaultNetworkInterfaceName};
    std::string ip_address{kDefaultNetworkIpAddress};
    std::string netmask{kDefaultNetworkNetmask};
    std::string gateway{kDefaultNetworkGateway};
    std::vector<std::string> dns_servers{kDefaultNetworkDnsPrimary, kDefaultNetworkDnsSecondary};
    std::string apply_mode_text{kStaticNetworkApplyModeText};
};

struct NetworkSettingsUpdateRequest {
    std::string mode{kNetworkModeStatic};
    std::string interface_name;
    std::string ip_address;
    std::string netmask;
    std::string gateway;
    std::vector<std::string> dns_servers;
};

struct NetworkSettingsUpdateResult {
    NetworkSettings settings;
    std::string message;
};

struct NetworkRuntimeStatus {
    std::string configured_mode{kNetworkModeStatic};
    std::string interface_name{kDefaultNetworkInterfaceName};
    bool interface_exists{false};
    std::string operstate{"unknown"};
    std::string link_state{"unknown"};
    std::string link_state_text{"未知"};
    std::string ip_address;
    std::string default_gateway;
    bool dhcp_client_running{false};
    std::string message;
};

struct NetworkApplyResult {
    bool applied{false};
    std::string mode{kNetworkModeStatic};
    std::string interface_name{kDefaultNetworkInterfaceName};
    std::string ip_address;
    std::string current_ip_address;
    std::string error_message;
    NetworkRuntimeStatus runtime_status;
    std::string message;
};

}  // namespace edge_controller
