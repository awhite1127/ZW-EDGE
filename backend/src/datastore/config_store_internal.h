// ConfigStore 各实现文件共享的 SQLite 语句、绑定和迁移辅助声明。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/filesystem_compat.h"
#include "common/sqlite_compat.h"
#include "common/status_code.h"
#include "common/string_utils.h"
#include "datastore/database_paths.h"
#include "datastore/sqlite_helpers.h"
#include "model/channel_config.h"
#include "model/master_node_config.h"
#include "model/modbus_server.h"
#include "model/mqtt_settings.h"
#include "model/network_settings.h"
#include "model/system_settings.h"
#include "protocol/modbus_rtu_protocol.h"

namespace edge_controller {
namespace config_store_internal {

using sqlite_helpers::Statement;
using sqlite_helpers::bind_int;
using sqlite_helpers::bind_int64;
using sqlite_helpers::bind_text;
using sqlite_helpers::column_int64;
using sqlite_helpers::column_text;
using sqlite_helpers::schema_migration_required_message;
using sqlite_helpers::sqlite_error;

// 确保数据库文件的父目录存在。
inline StatusCode ensure_parent_directory(const edge::fs::path& path, std::string* error_message)
{
    if (!path.has_parent_path()) {
        return StatusCode::kOk;
    }

    std::error_code create_error;
    edge::fs::create_directories(path.parent_path(), create_error);
    if (create_error) {
        if (error_message != nullptr) {
            *error_message = "创建配置 SQLite 数据库目录失败：" + path.parent_path().string() +
                             "，原因=" + create_error.message();
        }
        return StatusCode::kIoError;
    }
    return StatusCode::kOk;
}

// 规范化系统设置。
inline void normalize_system_settings(SystemSettings* settings)
{
    if (settings == nullptr) {
        return;
    }
    if (settings->device_name.empty()) {
        settings->device_name = kDefaultSystemDeviceName;
    }
    if (settings->site_location.empty()) {
        settings->site_location = kDefaultSystemSiteLocation;
    }
    if (settings->display_name.empty()) {
        settings->display_name = kDefaultSystemDisplayName;
    }
}

// 校验系统设置。
inline StatusCode validate_system_settings(
    const SystemSettings& settings,
    const std::string& source,
    std::string* error_message)
{
    if (settings.device_name.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".device_name 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (utf8_character_count(settings.device_name) > 64) {
        if (error_message != nullptr) {
            *error_message = source + ".device_name 不能超过 64 个字符";
        }
        return StatusCode::kInvalidArgument;
    }
    if (utf8_character_count(settings.site_location) > 128) {
        if (error_message != nullptr) {
            *error_message = source + ".site_location 不能超过 128 个字符";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.display_name.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".display_name 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (utf8_character_count(settings.display_name) > 64) {
        if (error_message != nullptr) {
            *error_message = source + ".display_name 不能超过 64 个字符";
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 解析 IPv4 地址并输出整数形式。
inline bool parse_ipv4_address(const std::string& value, std::uint32_t* output)
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
inline bool is_valid_ipv4_address(const std::string& value)
{
    return parse_ipv4_address(value, nullptr);
}

// 校验 IPv4 子网掩码是否连续有效。
inline bool is_valid_ipv4_netmask(const std::string& value)
{
    std::uint32_t mask = 0;
    if (!parse_ipv4_address(value, &mask) || mask == 0U || mask == 0xFFFFFFFFU) {
        return false;
    }
    const auto inverted = ~mask;
    return (inverted & (inverted + 1U)) == 0U;
}

// 判断 IP 地址与网关是否处于同一子网。
inline bool is_same_ipv4_subnet(const std::string& ip_address, const std::string& gateway, const std::string& netmask)
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

// 返回去除 ASCII 首尾空白的字符串副本。
inline std::string trim_ascii_copy(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(begin, end - begin);
}

// 规范化网络设置。
inline void normalize_network_settings(NetworkSettings* settings)
{
    if (settings == nullptr) {
        return;
    }
    settings->mode = trim_ascii_copy(settings->mode);
    settings->interface_name = trim_ascii_copy(settings->interface_name);
    settings->ip_address = trim_ascii_copy(settings->ip_address);
    settings->netmask = trim_ascii_copy(settings->netmask);
    settings->gateway = trim_ascii_copy(settings->gateway);
    if (settings->interface_name.empty()) {
        settings->interface_name = kDefaultNetworkInterfaceName;
    }
    if (settings->mode.empty()) {
        settings->mode = kNetworkModeStatic;
    }
    if (settings->ip_address.empty()) {
        settings->ip_address = kDefaultNetworkIpAddress;
    }
    if (settings->netmask.empty()) {
        settings->netmask = kDefaultNetworkNetmask;
    }
    if (settings->gateway.empty()) {
        settings->gateway = kDefaultNetworkGateway;
    }
    std::vector<std::string> normalized_dns;
    for (const auto& dns_server : settings->dns_servers) {
        const auto normalized = trim_ascii_copy(dns_server);
        if (!normalized.empty()) {
            normalized_dns.push_back(normalized);
        }
    }
    settings->dns_servers = std::move(normalized_dns);
    settings->apply_mode_text = settings->mode == kNetworkModeDhcp
                                    ? kDhcpNetworkApplyModeText
                                    : kStaticNetworkApplyModeText;
}

// 校验网络接口名称是否合法。
inline bool is_valid_network_interface_name(const std::string& value)
{
    if (value.empty() || value.size() > 15) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto value = static_cast<unsigned char>(ch);
        return std::isalnum(value) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    });
}

// 校验网络设置。
inline StatusCode validate_network_settings(
    const NetworkSettings& settings,
    const std::string& source,
    std::string* error_message)
{
    if (settings.interface_name.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".interface_name 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_valid_network_interface_name(settings.interface_name)) {
        if (error_message != nullptr) {
            *error_message = source + ".interface_name 格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.mode != kNetworkModeStatic && settings.mode != kNetworkModeDhcp) {
        if (error_message != nullptr) {
            *error_message = source + ".mode 只能是 static 或 dhcp";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.dns_servers.size() > 2) {
        if (error_message != nullptr) {
            *error_message = source + ".dns_servers 最多支持两个";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.ip_address.empty() || !is_valid_ipv4_address(settings.ip_address)) {
        if (error_message != nullptr) {
            *error_message = source + ".ip_address 格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.netmask.empty() || !is_valid_ipv4_netmask(settings.netmask)) {
        if (error_message != nullptr) {
            *error_message = source + ".netmask 格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.gateway.empty() || !is_valid_ipv4_address(settings.gateway)) {
        if (error_message != nullptr) {
            *error_message = source + ".gateway 格式不正确";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!is_same_ipv4_subnet(settings.ip_address, settings.gateway, settings.netmask)) {
        if (error_message != nullptr) {
            *error_message = source + ".gateway 必须与 ip_address 位于同一子网";
        }
        return StatusCode::kInvalidArgument;
    }
    for (const auto& dns_server : settings.dns_servers) {
        if (!is_valid_ipv4_address(dns_server)) {
            if (error_message != nullptr) {
                *error_message = source + ".dns_servers 格式不正确";
            }
            return StatusCode::kInvalidArgument;
        }
    }
    return StatusCode::kOk;
}

// 判断 MQTT 主题是否包含通配符。
inline bool contains_mqtt_topic_wildcard(const std::string& value)
{
    return value.find('#') != std::string::npos || value.find('+') != std::string::npos;
}

// 判断配置数据库路径是否为绝对路径。
inline bool is_absolute_config_path(const std::string& value)
{
    return edge::fs::path(value).is_absolute();
}

// 规范化MQTT设置。
inline void normalize_mqtt_settings(MqttSettings* settings)
{
    if (settings == nullptr) {
        return;
    }
    settings->broker_host = trim_ascii_copy(settings->broker_host);
    settings->client_id = trim_ascii_copy(settings->client_id);
    settings->node_id = trim_ascii_copy(settings->node_id);
    settings->username = trim_ascii_copy(settings->username);
    settings->password = trim_ascii_copy(settings->password);
    settings->topic_prefix = trim_ascii_copy(settings->topic_prefix);
    settings->tls_ca_file = trim_ascii_copy(settings->tls_ca_file);
    settings->tls_client_cert_file = trim_ascii_copy(settings->tls_client_cert_file);
    settings->tls_client_key_file = trim_ascii_copy(settings->tls_client_key_file);
    if (settings->node_id.empty()) {
        settings->node_id = kDefaultMqttNodeId;
    }
    if (settings->client_id.empty()) {
        settings->client_id = settings->node_id;
    }
    if (settings->topic_prefix.empty()) {
        settings->topic_prefix = kDefaultMqttTopicPrefix;
    }
}

// 校验MQTT设置。
inline StatusCode validate_mqtt_settings(
    const MqttSettings& settings,
    const std::string& source,
    std::string* error_message)
{
    if (settings.enabled && settings.broker_host.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".broker_host 在启用 MQTT 时不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.broker_port == 0) {
        if (error_message != nullptr) {
            *error_message = source + ".broker_port 必须在 1 到 65535 之间";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.node_id.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".node_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.client_id.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".client_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.topic_prefix.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".topic_prefix 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (contains_mqtt_topic_wildcard(settings.node_id) || contains_mqtt_topic_wildcard(settings.topic_prefix)) {
        if (error_message != nullptr) {
            *error_message = source + ".node_id 和 topic_prefix 不能包含 MQTT 通配符 # 或 +";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.publish_interval_seconds < 1 || settings.publish_interval_seconds > 3600) {
        if (error_message != nullptr) {
            *error_message = source + ".publish_interval_seconds 必须在 1 到 3600 之间";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.qos != 0 && settings.qos != 1) {
        if (error_message != nullptr) {
            *error_message = source + ".qos 仅支持 0 或 1";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.keep_alive_seconds < 10 || settings.keep_alive_seconds > 300) {
        if (error_message != nullptr) {
            *error_message = source + ".keep_alive_seconds 必须在 10 到 300 之间";
        }
        return StatusCode::kInvalidArgument;
    }
    if (settings.tls_enabled) {
        if (settings.tls_ca_file.empty()) {
            if (error_message != nullptr) {
                *error_message = source + ".tls_ca_file 在启用 TLS 时不能为空";
            }
            return StatusCode::kInvalidArgument;
        }
        if (!is_absolute_config_path(settings.tls_ca_file)) {
            if (error_message != nullptr) {
                *error_message = source + ".tls_ca_file 必须是绝对路径";
            }
            return StatusCode::kInvalidArgument;
        }
    }
    if (settings.tls_client_cert_file.empty() != settings.tls_client_key_file.empty()) {
        if (error_message != nullptr) {
            *error_message = source + ".tls_client_cert_file 和 tls_client_key_file 必须同时填写或同时为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!settings.tls_client_cert_file.empty() && !is_absolute_config_path(settings.tls_client_cert_file)) {
        if (error_message != nullptr) {
            *error_message = source + ".tls_client_cert_file 必须是绝对路径";
        }
        return StatusCode::kInvalidArgument;
    }
    if (!settings.tls_client_key_file.empty() && !is_absolute_config_path(settings.tls_client_key_file)) {
        if (error_message != nullptr) {
            *error_message = source + ".tls_client_key_file 必须是绝对路径";
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

// 解析通道类型。
inline bool parse_channel_type(const std::string& value, ChannelType* output)
{
    if (value == "modbus_rtu_serial") {
        *output = ChannelType::kModbusRtuSerial;
        return true;
    }
    if (value == "modbus_tcp") {
        *output = ChannelType::kModbusTcp;
        return true;
    }
    return false;
}

// 解析主站协议类型。
inline bool parse_master_protocol(const std::string& value, MasterProtocol* output)
{
    if (value == "modbus_rtu") {
        *output = MasterProtocol::kModbusRtu;
        return true;
    }
    if (value == "modbus_tcp") {
        *output = MasterProtocol::kModbusTcp;
        return true;
    }
    return false;
}

// 解析串口校验位配置。
inline bool parse_serial_parity(const std::string& value, SerialParity* output)
{
    if (value == "none" || value == "N" || value == "n") {
        *output = SerialParity::kNone;
        return true;
    }
    if (value == "even" || value == "E" || value == "e") {
        *output = SerialParity::kEven;
        return true;
    }
    if (value == "odd" || value == "O" || value == "o") {
        *output = SerialParity::kOdd;
        return true;
    }
    return false;
}

// 校验通道配置。
inline StatusCode validate_channel_config(const ChannelConfig& channel, std::string* error_message)
{
    if (channel.channel_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "channel_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (channel.channel_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "通道 " + channel.channel_id + " 的 channel_name 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (channel.channel_type == ChannelType::kUnknown) {
        if (error_message != nullptr) {
            *error_message = "通道 " + channel.channel_id + " 的 channel_type 非法";
        }
        return StatusCode::kInvalidArgument;
    }
    if (channel.channel_type == ChannelType::kModbusRtuSerial && channel.device_path.empty()) {
        if (error_message != nullptr) {
            *error_message = "通道 " + channel.channel_id + " 的 device_path 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (channel.channel_type == ChannelType::kModbusRtuSerial) {
        if (channel.baud_rate == 0 || channel.data_bits < 5 || channel.data_bits > 8 ||
            (channel.stop_bits != 1 && channel.stop_bits != 2) || channel.response_timeout_ms == 0 ||
            channel.retry_count > 10) {
            if (error_message != nullptr) {
                *error_message = "通道 " + channel.channel_id + " 的串口参数或超时时间非法";
            }
            return StatusCode::kInvalidArgument;
        }
        return StatusCode::kOk;
    }
    if (channel.channel_type == ChannelType::kModbusTcp) {
        if (channel.tcp_host.empty()) {
            if (error_message != nullptr) {
                *error_message = "通道 " + channel.channel_id + " 的 tcp_host 不能为空";
            }
            return StatusCode::kInvalidArgument;
        }
        if (channel.tcp_port == 0 || channel.connect_timeout_ms == 0 ||
            channel.response_timeout_ms == 0 || channel.retry_count > 10) {
            if (error_message != nullptr) {
                *error_message = "通道 " + channel.channel_id + " 的 TCP 目标、超时时间或重试次数非法";
            }
            return StatusCode::kInvalidArgument;
        }
    }
    return StatusCode::kOk;
}

// 校验主站配置。
inline StatusCode validate_master_config(const MasterNodeConfig& master, std::string* error_message)
{
    if (master.master_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "master_id 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (master.master_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "主控 " + master.master_id + " 的 master_name 不能为空";
        }
        return StatusCode::kInvalidArgument;
    }
    if (master.protocol == MasterProtocol::kUnknown) {
        if (error_message != nullptr) {
            *error_message = "主控 " + master.master_id + " 的 protocol 非法";
        }
        return StatusCode::kInvalidArgument;
    }
    if ((master.protocol == MasterProtocol::kModbusRtu ||
         master.protocol == MasterProtocol::kModbusTcp) &&
        master.channel_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "主控 " + master.master_id + " 必须绑定通道";
        }
        return StatusCode::kInvalidArgument;
    }
    if (master.target_address == 0 || master.target_address > 247 ||
        master.poll_interval_ms == 0 || master.response_timeout_ms == 0 ||
        master.retry_count > 10) {
        if (error_message != nullptr) {
            *error_message = "主控 " + master.master_id + " 的采集参数非法";
        }
        return StatusCode::kInvalidArgument;
    }
    if (master.device_count == 0 || master.device_count > kMaxDevicesPerMaster) {
        if (error_message != nullptr) {
            *error_message =
                "主控 " + master.master_id + " 的设备数量必须在 1-" +
                std::to_string(kMaxDevicesPerMaster) + " 范围内";
        }
        return StatusCode::kInvalidArgument;
    }
    if (master.device_template.empty()) {
        if (error_message != nullptr) {
            *error_message = "主控 " + master.master_id + " 未绑定设备模板";
        }
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

}  // namespace config_store_internal
}  // namespace edge_controller
