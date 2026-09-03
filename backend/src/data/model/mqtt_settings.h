// MQTT 配置与运行态模型；密码字段遵循只写、不回显约束。
#pragma once

#include <cstdint>
#include <string>

#include "shared/common/types.h"

namespace edge_controller {

inline constexpr const char kDefaultMqttNodeId[] = "edge-controller";
inline constexpr const char kDefaultMqttTopicPrefix[] = "edge-controller";
inline constexpr std::uint16_t kDefaultMqttBrokerPort = 1883;
inline constexpr std::uint32_t kDefaultMqttPublishIntervalSeconds = 10;
inline constexpr std::uint32_t kDefaultMqttKeepAliveSeconds = 60;

struct MqttSettings {
    bool enabled{false};
    std::string broker_host;
    std::uint16_t broker_port{kDefaultMqttBrokerPort};
    std::string client_id;
    std::string node_id{kDefaultMqttNodeId};
    std::string username;
    std::string password;
    std::string topic_prefix{kDefaultMqttTopicPrefix};
    std::uint32_t publish_interval_seconds{kDefaultMqttPublishIntervalSeconds};
    int qos{0};
    bool retain_status{true};
    std::uint32_t keep_alive_seconds{kDefaultMqttKeepAliveSeconds};
    bool tls_enabled{false};
    std::string tls_ca_file;
    std::string tls_client_cert_file;
    std::string tls_client_key_file;
    bool tls_insecure{false};
};

// MQTT 客户端和发布服务共享同一 endpoint 展示语义，空主机不生成仅含端口的地址。
inline std::string mqtt_broker_endpoint(const MqttSettings& settings)
{
    return settings.broker_host.empty()
               ? std::string{}
               : settings.broker_host + ":" + std::to_string(settings.broker_port);
}

struct MqttSettingsUpdateRequest {
    bool enabled{false};
    std::string broker_host;
    std::uint16_t broker_port{kDefaultMqttBrokerPort};
    std::string client_id;
    std::string node_id{kDefaultMqttNodeId};
    std::string username;
    std::string password;
    std::string topic_prefix{kDefaultMqttTopicPrefix};
    std::uint32_t publish_interval_seconds{kDefaultMqttPublishIntervalSeconds};
    int qos{0};
    bool retain_status{true};
    std::uint32_t keep_alive_seconds{kDefaultMqttKeepAliveSeconds};
    bool clear_password{false};
    bool tls_enabled{false};
    std::string tls_ca_file;
    std::string tls_client_cert_file;
    std::string tls_client_key_file;
    bool tls_insecure{false};
};

struct MqttSettingsUpdateResult {
    MqttSettings settings;
    std::string message;
    bool applied{false};
    std::string apply_error;
};

struct MqttRuntimeStatus {
    bool enabled{false};
    bool connected{false};
    std::string state{"disabled"};
    std::string broker_endpoint;
    bool tls_enabled{false};
    bool tls_insecure{false};
    std::string status_topic;
    std::string realtime_topic;
    std::string event_topic;
    std::string alarm_topic;
    std::string last_error_message;
    TimestampMs last_connect_time_ms{0};
    TimestampMs last_disconnect_time_ms{0};
    TimestampMs last_publish_time_ms{0};
    std::string last_publish_topic;
    std::uint64_t last_publish_payload_bytes{0};
    std::string last_publish_error_message;
    std::uint64_t last_publish_sequence{0};
    TimestampMs last_event_publish_time_ms{0};
    TimestampMs last_alarm_publish_time_ms{0};
    std::string last_event_publish_topic;
    std::string last_alarm_publish_topic;
    std::uint64_t event_publish_count{0};
    std::uint64_t alarm_publish_count{0};
    std::uint64_t published_message_count{0};
    std::uint64_t failed_publish_count{0};
};

}  // namespace edge_controller
