// MQTT 状态、实时值、事件和告警 JSON 载荷构建接口。
#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"
#include "model/mqtt_settings.h"
#include "model/realtime_view_snapshot.h"
#include "model/service_summary.h"
#include "model/system_settings.h"

namespace edge_controller {

// 生成 MQTT 状态主题。
std::string mqtt_status_topic(const MqttSettings& settings);
// 生成 MQTT 实时数据主题。
std::string mqtt_realtime_topic(const MqttSettings& settings);
// 生成 MQTT 事件主题。
std::string mqtt_event_topic(const MqttSettings& settings);
// 生成 MQTT 告警主题。
std::string mqtt_alarm_topic(const MqttSettings& settings);

// 构造MQTT状态载荷。
std::string build_mqtt_status_payload(
    const MqttSettings& settings,
    const SystemSettings& system_settings,
    bool online,
    TimestampMs timestamp_ms);

// 构造MQTT实时数据载荷。
std::string build_mqtt_realtime_payload(
    const MqttSettings& settings,
    const RealtimeViewSnapshot& snapshot,
    std::uint64_t sequence,
    TimestampMs timestamp_ms);

// 构造MQTT事件载荷。
std::string build_mqtt_event_payload(
    const MqttSettings& settings,
    const ServiceEvent& event);

// 构造MQTT告警载荷。
std::string build_mqtt_alarm_payload(
    const MqttSettings& settings,
    const ServiceEvent& event);

}  // namespace edge_controller
