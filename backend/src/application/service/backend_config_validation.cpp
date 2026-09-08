// 配置校验集中层：跨通道、主站、模板和设备检查引用关系，避免各写入入口产生不同规则。
#include "application/service/backend_service_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "shared/common/string_utils.h"
#include "data/model/builtin_device_templates.h"
#include "data/model/device_template.h"

namespace edge_controller {
namespace backend_internal {

// 解析 IPv4 地址并输出整数形式。
bool parse_ipv4_address(const std::string& value, std::uint32_t* output)
{
    std::uint32_t parsed = 0;
    std::size_t begin = 0;
    for (int part_index = 0; part_index < 4; ++part_index) {
        const auto dot = value.find('.', begin);
        const auto end = dot == std::string::npos ? value.size() : dot;
        if (end == begin) {
            return false;
        }

        unsigned int part = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const auto ch = value[index];
            if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
                return false;
            }
            part = part * 10U + static_cast<unsigned int>(ch - '0');
            if (part > 255U) {
                return false;
            }
        }

        parsed = (parsed << 8U) | part;
        if (part_index < 3) {
            if (dot == std::string::npos) {
                return false;
            }
            begin = dot + 1;
        } else if (dot != std::string::npos) {
            return false;
        }
    }

    if (output != nullptr) {
        *output = parsed;
    }
    return true;
}

// 校验 IPv4 地址格式是否合法。
bool is_valid_ipv4_address(const std::string& value)
{
    return parse_ipv4_address(value, nullptr);
}

// 校验 IPv4 子网掩码是否连续有效。
bool is_valid_ipv4_netmask(const std::string& value)
{
    std::uint32_t mask = 0;
    if (!parse_ipv4_address(value, &mask) || mask == 0U || mask == 0xFFFFFFFFU) {
        return false;
    }
    const auto inverted = ~mask;
    return (inverted & (inverted + 1U)) == 0U;
}

// 判断 IP 地址与网关是否处于同一子网。
bool is_same_ipv4_subnet(const std::string& ip_address, const std::string& gateway, const std::string& netmask)
{
    std::uint32_t ip_value = 0;
    std::uint32_t gateway_value = 0;
    std::uint32_t mask_value = 0;
    if (!parse_ipv4_address(ip_address, &ip_value) || !parse_ipv4_address(gateway, &gateway_value) ||
        !parse_ipv4_address(netmask, &mask_value)) {
        return false;
    }
    return (ip_value & mask_value) == (gateway_value & mask_value);
}

// 校验系统设置请求。
StatusCode validate_system_settings_request(
    const SystemSettingsUpdateRequest& request,
    SystemSettings* settings,
    std::string* error_message)
{
    // 从请求生成候选配置，并规范化各文本字段。
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少系统设置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    SystemSettings parsed;
    parsed.device_name = trim_copy(request.device_name);
    parsed.site_location = trim_copy(request.site_location);
    parsed.display_name = trim_copy(request.display_name);
    if (parsed.site_location.empty()) {
        parsed.site_location = kDefaultSystemSiteLocation;
    }

    if (parsed.device_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "设备名称不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (utf8_character_count(parsed.device_name) > 64) {
        if (error_message != nullptr) {
            *error_message = "设备名称不能超过 64 个字符";
        }
        return StatusCode::kInvalidArgument;
    }
    if (utf8_character_count(parsed.site_location) > 128) {
        if (error_message != nullptr) {
            *error_message = "安装位置不能超过 128 个字符";
        }
        return StatusCode::kInvalidArgument;
    }
    if (parsed.display_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "系统显示名称不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (utf8_character_count(parsed.display_name) > 64) {
        if (error_message != nullptr) {
            *error_message = "系统显示名称不能超过 64 个字符";
        }
        return StatusCode::kInvalidArgument;
    }

    *settings = std::move(parsed);
    return StatusCode::kOk;
}

// 校验网络设置请求。
StatusCode validate_network_settings_request(
    const NetworkSettingsUpdateRequest& request,
    NetworkSettings* settings,
    std::string* error_message)
{
    return canonicalize_network_settings_request(request, nullptr, settings, error_message);
}

// 规范化并校验网络设置请求。
StatusCode canonicalize_network_settings_request(
    const NetworkSettingsUpdateRequest& request,
    const NetworkSettings* current_settings,
    NetworkSettings* settings,
    std::string* error_message)
{
    if (settings == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少网络配置输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    NetworkSettings parsed;
    parsed.mode = trim_copy(request.mode);
    parsed.interface_name = trim_copy(request.interface_name);
    const auto requested_ip_address = trim_copy(request.ip_address);
    const auto requested_netmask = trim_copy(request.netmask);
    const auto requested_gateway = trim_copy(request.gateway);
    parsed.dns_servers.clear();
    if (parsed.mode.empty()) {
        parsed.mode = kNetworkModeStatic;
    }
    parsed.apply_mode_text = parsed.mode == kNetworkModeDhcp
                                 ? kDhcpNetworkApplyModeText
                                 : kStaticNetworkApplyModeText;

    // 校验网口名称、配置模式和 DNS 数量。
    if (parsed.interface_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "网口名称不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (parsed.interface_name.size() > 15 ||
        !std::all_of(parsed.interface_name.begin(), parsed.interface_name.end(), [](char ch) {
            const auto value = static_cast<unsigned char>(ch);
            return std::isalnum(value) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':';
        })) {
        if (error_message != nullptr) {
            *error_message = "网口名称格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (parsed.mode != kNetworkModeStatic && parsed.mode != kNetworkModeDhcp) {
        if (error_message != nullptr) {
            *error_message = "网络模式只能是 static 或 dhcp";
        }
        return StatusCode::kInvalidArgument;
    }
    std::vector<std::string> requested_dns;
    for (const auto& dns_server : request.dns_servers) {
        const auto normalized = trim_copy(dns_server);
        if (!normalized.empty()) {
            requested_dns.push_back(normalized);
        }
    }
    if (requested_dns.size() > 2) {
        if (error_message != nullptr) {
            *error_message = "DNS 服务器最多支持两个";
        }
        return StatusCode::kInvalidArgument;
    }

    // DHCP 模式保留现有静态字段作为回退；静态模式采用请求值。
    if (parsed.mode == kNetworkModeDhcp) {
        const auto current_ip_address =
            current_settings == nullptr ? std::string{} : trim_copy(current_settings->ip_address);
        const auto current_netmask =
            current_settings == nullptr ? std::string{} : trim_copy(current_settings->netmask);
        const auto current_gateway =
            current_settings == nullptr ? std::string{} : trim_copy(current_settings->gateway);
        parsed.ip_address = requested_ip_address.empty()
                                ? (current_ip_address.empty() ? kDefaultNetworkIpAddress : current_ip_address)
                                : requested_ip_address;
        parsed.netmask = requested_netmask.empty()
                             ? (current_netmask.empty() ? kDefaultNetworkNetmask : current_netmask)
                             : requested_netmask;
        parsed.gateway = requested_gateway.empty()
                             ? (current_gateway.empty() ? kDefaultNetworkGateway : current_gateway)
                             : requested_gateway;
        if (requested_dns.empty()) {
            if (current_settings != nullptr) {
                for (const auto& dns_server : current_settings->dns_servers) {
                    const auto normalized = trim_copy(dns_server);
                    if (!normalized.empty()) {
                        parsed.dns_servers.push_back(normalized);
                    }
                }
            }
            if (parsed.dns_servers.empty()) {
                parsed.dns_servers = {kDefaultNetworkDnsPrimary, kDefaultNetworkDnsSecondary};
            }
        } else {
            parsed.dns_servers = requested_dns;
        }
    } else {
        parsed.ip_address = requested_ip_address;
        parsed.netmask = requested_netmask;
        parsed.gateway = requested_gateway;
        parsed.dns_servers = requested_dns;
    }

    if (parsed.dns_servers.size() > 2) {
        if (error_message != nullptr) {
            *error_message = "DNS 服务器最多支持两个";
        }
        return StatusCode::kInvalidArgument;
    }

    // 校验 IPv4 地址、掩码、网关、同网段关系及 DNS 地址。
    if (parsed.ip_address.empty()) {
        if (error_message != nullptr) {
            *error_message = "IP 地址不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (parsed.netmask.empty()) {
        if (error_message != nullptr) {
            *error_message = "子网掩码不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (parsed.gateway.empty()) {
        if (error_message != nullptr) {
            *error_message = "默认网关不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_valid_ipv4_address(parsed.ip_address)) {
        if (error_message != nullptr) {
            *error_message = "IP 地址格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_valid_ipv4_netmask(parsed.netmask)) {
        if (error_message != nullptr) {
            *error_message = "子网掩码格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_valid_ipv4_address(parsed.gateway)) {
        if (error_message != nullptr) {
            *error_message = "默认网关格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_same_ipv4_subnet(parsed.ip_address, parsed.gateway, parsed.netmask)) {
        if (error_message != nullptr) {
            *error_message = "默认网关必须与 IP 地址位于同一子网";
        }
        return StatusCode::kInvalidArgument;
    }

    for (const auto& dns_server : parsed.dns_servers) {
        if (!is_valid_ipv4_address(dns_server)) {
            if (error_message != nullptr) {
                *error_message = "DNS 服务器格式不正确";
            }
            return StatusCode::kInvalidArgument;
        }
    }

    // 输出规范化且通过校验的网络配置。
    *settings = std::move(parsed);
    return StatusCode::kOk;
}

// 判断串口波特率是否受支持。
bool is_supported_baud_rate(std::uint32_t baud_rate)
{
    switch (baud_rate) {
    case 1200:
    case 2400:
    case 4800:
    case 9600:
    case 19200:
    case 38400:
    case 57600:
    case 115200:
        return true;
    default:
        return false;
    }
}

// 校验串口设备路径是否合法。
bool is_legal_device_path(const std::string& path)
{
    if (path.empty() || path.rfind("/dev/", 0) != 0) {
        return false;
    }
    return std::none_of(path.begin(), path.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    });
}

// 校验 TCP 主机地址是否合法。
bool is_legal_tcp_host(const std::string& host)
{
    if (host.empty()) {
        return false;
    }
    return std::none_of(host.begin(), host.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    });
}

// 返回通道配置对应的端口描述。
std::string configured_channel_port(const ChannelConfig& channel)
{
    if (!channel.device_path.empty()) {
        return channel.device_path;
    }
    return channel.port_name;
}

// 在配置导入复制或应用任何载荷前校验通道容量，避免超大配置进入后续运行态。
StatusCode validate_import_channel_limits(
    const std::vector<ChannelConfig>& channels,
    std::string* error_message)
{
    std::string limit_error;
    if (validate_channel_count_limits(channels, &limit_error)) {
        return StatusCode::kOk;
    }
    if (error_message != nullptr) {
        *error_message = "配置导入失败：" + limit_error;
    }
    return StatusCode::kInvalidArgument;
}

// 校验配置导入包中按 trim 规范化后的资源 ID，防止相同对象被当成多次“更新”后重复压入载荷。
StatusCode validate_import_resource_ids(
    const std::vector<ChannelConfig>& channels,
    const std::vector<MasterNodeConfig>& masters,
    std::string* error_message)
{
    std::set<std::string> channel_ids;
    for (const auto& channel : channels) {
        const auto channel_id = trim_copy(channel.channel_id);
        if (channel_id.empty() || !channel_ids.insert(channel_id).second) {
            if (error_message != nullptr) {
                *error_message = channel_id.empty()
                                     ? "配置导入失败：通道 channel_id 不能为空"
                                     : "配置导入失败：通道标识重复：" + channel_id;
            }
            return StatusCode::kInvalidArgument;
        }
    }

    std::set<std::string> master_ids;
    for (const auto& master : masters) {
        const auto master_id = trim_copy(master.master_id);
        if (master_id.empty() || !master_ids.insert(master_id).second) {
            if (error_message != nullptr) {
                *error_message = master_id.empty()
                                     ? "配置导入失败：主站 master_id 不能为空"
                                     : "配置导入失败：主站标识重复：" + master_id;
            }
            return StatusCode::kInvalidArgument;
        }
    }
    return StatusCode::kOk;
}

// 校验通道配置更新请求。
StatusCode validate_channel_update_request(
    const std::vector<ChannelConfig>& channels,
    const ChannelConfigUpdateRequest& request,
    const std::vector<SerialPortInfo>& serial_ports,
    std::string* warning_message,
    std::string* error_message)
{
    const auto channel_id = trim_copy(request.channel_id);
    if (channel_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "通道 ID 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (trim_copy(request.channel_name).empty()) {
        if (error_message != nullptr) {
            *error_message = "通道名称不能为空";
        }
        return StatusCode::kInvalidArgument;
    }

    if (request.response_timeout_ms == 0 || request.response_timeout_ms > 60000) {
        if (error_message != nullptr) {
            *error_message = "响应超时时间必须在 1-60000 ms 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.retry_count > 10) {
        if (error_message != nullptr) {
            *error_message = "重试次数必须在 0-10 范围内";
        }
        return StatusCode::kInvalidArgument;
    }

    if (request.channel_type == ChannelType::kModbusRtuSerial) {
        const auto port_name = trim_copy(request.port_name);
        if (!is_legal_device_path(port_name)) {
            if (error_message != nullptr) {
                *error_message = "串口路径必须是 /dev/* 格式的绝对路径";
            }
            return StatusCode::kInvalidArgument;
        }

        for (const auto& channel : channels) {
            if (channel.channel_id == channel_id || channel.channel_type != ChannelType::kModbusRtuSerial) {
                continue;
            }
            if (configured_channel_port(channel) == port_name) {
                if (error_message != nullptr) {
                    *error_message =
                        "串口路径已被通道 " + channel.channel_id + " 使用: " + port_name;
                }
                return StatusCode::kInvalidArgument;
            }
        }

        if (!is_supported_baud_rate(request.baud_rate)) {
            if (error_message != nullptr) {
                *error_message = "不支持的波特率";
            }
            return StatusCode::kInvalidArgument;
        }
        if (request.data_bits < 5 || request.data_bits > 8) {
            if (error_message != nullptr) {
                *error_message = "数据位必须在 5-8 范围内";
            }
            return StatusCode::kInvalidArgument;
        }
        if (request.stop_bits != 1 && request.stop_bits != 2) {
            if (error_message != nullptr) {
                *error_message = "停止位必须为 1 或 2";
            }
            return StatusCode::kInvalidArgument;
        }

        if (!serial_ports.empty()) {
            const auto matched = std::any_of(serial_ports.begin(), serial_ports.end(), [&](const SerialPortInfo& port) {
                return port.path == port_name;
            });
            if (!matched) {
                backend_internal::append_message(
                    warning_message,
                    "当前串口路径未出现在最新系统扫描结果中，已按当前配置保存");
            }
        }
    } else if (request.channel_type == ChannelType::kModbusTcp) {
        if (!is_legal_tcp_host(trim_copy(request.tcp_host))) {
            if (error_message != nullptr) {
                *error_message = "TCP 远端地址不能为空且不能包含空白字符";
            }
            return StatusCode::kInvalidArgument;
        }
        if (request.tcp_port == 0) {
            if (error_message != nullptr) {
                *error_message = "TCP 远端端口必须在 1-65535 范围内";
            }
            return StatusCode::kInvalidArgument;
        }
        if (request.connect_timeout_ms == 0 || request.connect_timeout_ms > 60000) {
            if (error_message != nullptr) {
                *error_message = "TCP 建连超时时间必须在 1-60000 ms 范围内";
            }
            return StatusCode::kInvalidArgument;
        }
    } else {
        if (error_message != nullptr) {
            *error_message = "通道类型无效";
        }
        return StatusCode::kInvalidArgument;
    }

    // 使用请求构造变更后的容量视图：同 ID 是更新，不增加总数；新 ID 是创建。
    auto prospective_channels = channels;
    const auto current = std::find_if(
        prospective_channels.begin(),
        prospective_channels.end(),
        [&](const ChannelConfig& channel) { return channel.channel_id == channel_id; });
    if (current == prospective_channels.end()) {
        ChannelConfig created;
        created.channel_id = channel_id;
        created.channel_type = request.channel_type;
        prospective_channels.push_back(std::move(created));
    } else {
        current->channel_type = request.channel_type;
    }
    if (!validate_channel_count_limits(prospective_channels, error_message)) {
        return StatusCode::kInvalidArgument;
    }

    return StatusCode::kOk;
}

// 在写入 SQLite 前校验引用该通道的主站协议，避免新配置落库后才在拓扑重建阶段失败。
StatusCode validate_channel_type_for_referencing_masters(
    const std::vector<MasterNodeConfig>& masters,
    const ChannelId& channel_id,
    ChannelType channel_type,
    std::string* error_message)
{
    const auto normalized_channel_id = trim_copy(channel_id);
    for (const auto& master : masters) {
        if (trim_copy(master.channel_id) != normalized_channel_id) {
            continue;
        }
        const bool compatible =
            (master.protocol == MasterProtocol::kModbusRtu &&
             channel_type == ChannelType::kModbusRtuSerial) ||
            (master.protocol == MasterProtocol::kModbusTcp &&
             channel_type == ChannelType::kModbusTcp);
        if (compatible ||
            (master.protocol != MasterProtocol::kModbusRtu &&
             master.protocol != MasterProtocol::kModbusTcp)) {
            continue;
        }
        if (error_message != nullptr) {
            *error_message =
                "通道 " + normalized_channel_id + " 仍被主站 " + master.master_id +
                " 引用，不能变更为与主站协议不匹配的通道类型";
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 构造新建通道配置。
ChannelConfig build_new_channel_config(const ChannelConfigUpdateRequest& request)
{
    ChannelConfig created;
    created.channel_id = trim_copy(request.channel_id);
    created.channel_name = trim_copy(request.channel_name);
    created.enabled = request.enabled;
    created.channel_type = request.channel_type;

    const auto port_name = trim_copy(request.port_name);
    created.device_path = port_name;
    created.port_name = port_name;
    created.baud_rate = request.baud_rate;
    created.data_bits = request.data_bits;
    created.parity = request.parity;
    created.stop_bits = request.stop_bits;
    created.response_timeout_ms = request.response_timeout_ms;
    created.retry_count = request.retry_count;
    created.tcp_host = trim_copy(request.tcp_host);
    created.tcp_port = request.tcp_port == 0 ? 502 : request.tcp_port;
    created.connect_timeout_ms = request.connect_timeout_ms == 0 ? 3000 : request.connect_timeout_ms;
    return created;
}

// 合并现有配置与请求，构造更新后的通道配置。
ChannelConfig build_updated_channel_config(
    const ChannelConfig& current,
    const ChannelConfigUpdateRequest& request)
{
    auto updated = build_new_channel_config(request);
    updated.channel_id = current.channel_id;
    return updated;
}

// 校验主站配置更新请求。
StatusCode validate_master_update_request(
    const SystemConfig& system_config,
    const MasterNodeConfigUpdateRequest& request,
    bool creating,
    std::string* error_message)
{
    return validate_master_update_request_with_templates(
        system_config, device_templates(), request, creating, error_message);
}

// 校验主站更新类型。
StatusCode validate_master_update_request_with_templates(
    const SystemConfig& system_config,
    const std::vector<DeviceTemplateDefinition>& templates,
    const MasterNodeConfigUpdateRequest& request,
    bool creating,
    std::string* error_message)
{
    if (trim_copy(request.master_id).empty()) {
        if (error_message != nullptr) {
            *error_message = "主控 ID 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (trim_copy(request.master_name).empty()) {
        if (error_message != nullptr) {
            *error_message = "主控名称不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.protocol == MasterProtocol::kUnknown) {
        if (error_message != nullptr) {
            *error_message = "协议类型无效";
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.protocol == MasterProtocol::kModbusRtu ||
        request.protocol == MasterProtocol::kModbusTcp) {
        if (trim_copy(request.channel_id).empty()) {
            if (error_message != nullptr) {
                *error_message = request.protocol == MasterProtocol::kModbusRtu
                                     ? "Modbus RTU 主控必须绑定 RTU 通道"
                                     : "Modbus TCP 主控必须绑定 TCP 通道";
            }
            return StatusCode::kInvalidArgument;
        }
        const auto* channel = find_channel_config_in_list(system_config.channels, trim_copy(request.channel_id));
        if (channel == nullptr) {
            if (error_message != nullptr) {
                *error_message = "绑定通道不存在: " + trim_copy(request.channel_id);
            }
            return StatusCode::kInvalidArgument;
        }
        const bool type_matched =
            (request.protocol == MasterProtocol::kModbusRtu &&
             channel->channel_type == ChannelType::kModbusRtuSerial) ||
            (request.protocol == MasterProtocol::kModbusTcp &&
             channel->channel_type == ChannelType::kModbusTcp);
        if (!type_matched) {
            if (error_message != nullptr) {
                *error_message = request.protocol == MasterProtocol::kModbusRtu
                                     ? "Modbus RTU 主控必须绑定 RTU 串口通道"
                                     : "Modbus TCP 主控必须绑定 TCP 通道";
            }
            return StatusCode::kInvalidArgument;
        }
    }
    if (request.target_address == 0 || request.target_address > 247) {
        if (error_message != nullptr) {
            *error_message = "Unit ID / 从站地址必须在 1-247 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.poll_interval_ms == 0 || request.poll_interval_ms > 3600000) {
        if (error_message != nullptr) {
            *error_message = "轮询周期必须在 1-3600000 ms 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.response_timeout_ms == 0 || request.response_timeout_ms > 60000) {
        if (error_message != nullptr) {
            *error_message = "响应超时时间必须在 1-60000 ms 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.retry_count > 10) {
        if (error_message != nullptr) {
            *error_message = "重试次数必须在 0-10 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (trim_copy(request.device_template).empty()) {
        if (error_message != nullptr) {
            *error_message = "请选择设备模板";
        }
        return StatusCode::kInvalidArgument;
    }
    const DeviceTemplateDefinition* stored_device_template = nullptr;
    const auto requested_template_id = trim_copy(request.device_template);
    for (const auto& candidate : templates) {
        if (candidate.template_id == requested_template_id) {
            stored_device_template = &candidate;
            break;
        }
    }
    if (stored_device_template == nullptr) {
        if (error_message != nullptr) {
            *error_message = "设备模板不存在：" + requested_template_id;
        }
        return StatusCode::kInvalidArgument;
    }
    const auto* device_template = stored_device_template;
    std::string read_model_error;
    if (!is_ok(validate_device_template_read_model(*device_template, &read_model_error))) {
        if (error_message != nullptr) {
            *error_message = "主站 " + trim_copy(request.master_id) +
                             " 绑定的设备类型读取区块配置无效：" + read_model_error;
        }
        return StatusCode::kInvalidArgument;
    }
    if (request.device_count == 0 || request.device_count > kMaxDevicesPerMaster) {
        if (error_message != nullptr) {
            *error_message =
                "主站设备数量必须在 1-" + std::to_string(kMaxDevicesPerMaster) + " 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    const auto maximum_device_index = static_cast<std::uint64_t>(request.device_count - 1U);
    for (const auto& read_block : device_template->read_blocks) {
        const auto range_start =
            static_cast<std::uint64_t>(request.block_start_register) +
            maximum_device_index * static_cast<std::uint64_t>(device_template->device_address_stride) +
            static_cast<std::uint64_t>(read_block.start_offset);
        const auto range_end =
            range_start + static_cast<std::uint64_t>(read_block.register_count);
        if (range_start >= 65536ULL || range_end > 65536ULL) {
            if (error_message != nullptr) {
                const auto master_label = trim_copy(request.master_name).empty()
                                              ? trim_copy(request.master_id)
                                              : trim_copy(request.master_name) + "（" +
                                                    trim_copy(request.master_id) + "）";
                const auto block_label = read_block.display_name.empty()
                                             ? read_block.block_key
                                             : read_block.display_name + "（" +
                                                   read_block.block_key + "）";
                *error_message =
                    "主站 " + master_label + " 的设备序号 " +
                    std::to_string(maximum_device_index) + " 读取区块 " + block_label +
                    " 地址范围 [" + std::to_string(range_start) + "," +
                    std::to_string(range_end) + ") 超出 Modbus 寄存器地址上限";
            }
            return StatusCode::kInvalidArgument;
        }
    }

    const auto* existing = find_master_config_in_list(system_config.master_nodes, trim_copy(request.master_id));
    if (creating && existing != nullptr) {
        if (error_message != nullptr) {
            *error_message = "主控 ID 已存在: " + trim_copy(request.master_id);
        }
        return StatusCode::kInvalidArgument;
    }
    if (!creating && existing == nullptr) {
        if (error_message != nullptr) {
            *error_message = "未找到主控: " + trim_copy(request.master_id);
        }
        return StatusCode::kNotFound;
    }
    return StatusCode::kOk;
}

// 合并现有配置与请求，构造更新后的主站配置。
MasterNodeConfig build_updated_master_config(
    const MasterNodeConfigUpdateRequest& request)
{
    MasterNodeConfig updated;
    updated.master_id = trim_copy(request.master_id);
    updated.master_name = trim_copy(request.master_name);
    updated.enabled = request.enabled;
    updated.protocol = request.protocol;
    updated.channel_id = trim_copy(request.channel_id);
    updated.target_address = request.target_address;
    updated.poll_interval_ms = request.poll_interval_ms;
    updated.response_timeout_ms = request.response_timeout_ms;
    updated.retry_count = request.retry_count;
    updated.remark = trim_copy(request.remark);
    updated.device_template = trim_copy(request.device_template).empty()
                                  ? default_device_template_id()
                                  : trim_copy(request.device_template);
    updated.block_start_register = request.block_start_register;
    updated.device_count = request.device_count;
    return updated;
}


}  // namespace backend_internal
}  // namespace edge_controller
