// MQTT 载荷构建器只生成稳定 JSON 文档，不进行网络 I/O，也不读取密码或证书文件。
#include "mqtt/mqtt_payload_builder.h"

#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common/enums.h"

namespace edge_controller {

namespace {

// 判断数值是否为有限值。
nlohmann::json finite_number(double value)
{
    return std::isfinite(value) ? nlohmann::json(value) : nlohmann::json(nullptr);
}

// 去除主题首尾多余的斜杠。
std::string trim_topic_slashes(const std::string& value)
{
    const auto first = value.find_first_not_of('/');
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of('/');
    return value.substr(first, last - first + 1);
}

// 返回 MQTT 载荷使用的节点标识。
std::string effective_node_id(const MqttSettings& settings)
{
    return settings.node_id.empty() ? std::string(kDefaultMqttNodeId) : settings.node_id;
}

// 拼接规范化主题前缀与业务后缀。
std::string topic_with_suffix(const MqttSettings& settings, const std::string& suffix)
{
    const auto prefix = trim_topic_slashes(settings.topic_prefix.empty()
                                               ? std::string(kDefaultMqttTopicPrefix)
                                               : settings.topic_prefix);
    return prefix + "/" + effective_node_id(settings) + "/" + suffix;
}

// 根据告警事件推导 MQTT 告警状态。
std::string alarm_state_from_event(const ServiceEvent& event)
{
    return event.level == "info" && event.summary.find("已恢复") != std::string::npos ? "recovered" : "active";
}

// 从事件中提取告警点位的稳定键。
std::string alarm_point_key(const ServiceEvent& event)
{
// 数据报警事件中，AlarmEvaluator 使用 diagnosis.target_id 传递 rule.point_key。
    return event.diagnosis.target_id;
}

// 根据事件字段生成稳定的告警规则标识。
std::string alarm_rule_id(const ServiceEvent& event)
{
    const auto point_key = alarm_point_key(event);
    return point_key.empty() ? event.target_id : event.target_id + ":" + point_key;
}

// 按设备标识建立运行状态索引。
std::unordered_map<std::string_view, const DeviceStatus*> device_status_by_id(
    const RealtimeViewSnapshot& snapshot)
{
    std::unordered_map<std::string_view, const DeviceStatus*> statuses;
    statuses.reserve(snapshot.system_status.device_status_list.size());
    for (const auto& status : snapshot.system_status.device_status_list) {
        statuses[status.device_id] = &status;
    }
    return statuses;
}

// 将实时点位转换为 MQTT JSON 对象。
nlohmann::json point_to_json(const PointValue& point)
{
    const auto value = point.value;
    // 不可发布点仍要携带稳定 key/quality/valid，供接收方区分局部失败；数值本身不可继续对外使用。
    const auto publishable = point.valid && point.quality == DataQuality::kGood;
    const auto json_value = publishable ? finite_number(value) : nlohmann::json(nullptr);
    nlohmann::json result{
        {"key", point.key},
        {"name", point.name},
        {"value", json_value},
        {"unit", point.unit},
        {"precision", point.precision},
        {"quality", to_string(point.quality)},
        {"valid", point.valid},
        {"sample_time_ms", point.sample_time_ms},
    };
    if (publishable && !point.display_text.empty()) {
        result["display_value"] = point.display_text;
    }
    return result;
}

}  // namespace

// 生成 MQTT 状态主题。
std::string mqtt_status_topic(const MqttSettings& settings)
{
    return topic_with_suffix(settings, "status");
}

// 生成 MQTT 实时数据主题。
std::string mqtt_realtime_topic(const MqttSettings& settings)
{
    return topic_with_suffix(settings, "realtime");
}

// 生成 MQTT 事件主题。
std::string mqtt_event_topic(const MqttSettings& settings)
{
    return topic_with_suffix(settings, "event");
}

// 生成 MQTT 告警主题。
std::string mqtt_alarm_topic(const MqttSettings& settings)
{
    return topic_with_suffix(settings, "alarm");
}

// 构造MQTT状态载荷。
std::string build_mqtt_status_payload(
    const MqttSettings& settings,
    const SystemSettings& system_settings,
    bool online,
    TimestampMs timestamp_ms)
{
    nlohmann::json payload{
        {"schema", "edge.status.v1"},
        {"node_id", effective_node_id(settings)},
        {"online", online},
        {"timestamp_ms", timestamp_ms},
    };
    if (online) {
        payload["device_name"] = system_settings.device_name;
        payload["site_location"] = system_settings.site_location;
    }
    return payload.dump();
}

// 构造MQTT实时数据载荷。
std::string build_mqtt_realtime_payload(
    const MqttSettings& settings,
    const RealtimeViewSnapshot& snapshot,
    std::uint64_t sequence,
    TimestampMs timestamp_ms)
{
    const auto statuses = device_status_by_id(snapshot);
    nlohmann::json devices = nlohmann::json::array();
    devices.get_ref<nlohmann::json::array_t&>().reserve(
        snapshot.device_realtime_snapshots.size());

    for (const auto& realtime : snapshot.device_realtime_snapshots) {
        const auto status_iterator = statuses.find(realtime.device_id);
        const auto* status = status_iterator == statuses.end() ? nullptr : status_iterator->second;

        nlohmann::json points = nlohmann::json::array();
        points.get_ref<nlohmann::json::array_t&>().reserve(realtime.points.size());
        for (const auto& point : realtime.points) {
            if (!point.summary) {
                continue;
            }
            points.push_back(point_to_json(point));
        }

        devices.push_back(nlohmann::json{
            {"device_id", realtime.device_id},
            {"device_name", realtime.device_name},
            {"master_id", realtime.master_id},
            {"template_id", realtime.template_id},
            {"template_name", realtime.template_name},
            {"online", status != nullptr ? status->online : false},
            // MQTT 结构保持不变，设备级 quality 的含义明确为通讯质量。
            {"quality", to_string(realtime.communication_quality)},
            {"updated_at_ms", status != nullptr ? status->updated_at_ms : realtime.sample_time_ms},
            {"points", std::move(points)},
        });
    }

    const nlohmann::json payload{
        {"schema", "edge.realtime.v1"},
        {"node_id", effective_node_id(settings)},
        {"timestamp_ms", timestamp_ms},
        {"seq", sequence},
        {"devices", std::move(devices)},
    };
    return payload.dump();
}

// 构造MQTT事件载荷。
std::string build_mqtt_event_payload(
    const MqttSettings& settings,
    const ServiceEvent& event)
{
    const nlohmann::json payload{
        {"schema", "edge.event.v1"},
        {"node_id", effective_node_id(settings)},
        {"event_id", event.event_id},
        {"level", event.level},
        {"category", event.source},
        {"type", event.source},
        {"source", event.source},
        {"object_id", event.target_id},
        {"message", event.summary},
        {"detail", event.detail},
        {"occurred_at_ms", event.timestamp_ms},
        {"timestamp_ms", event.timestamp_ms},
    };
    return payload.dump();
}

// 构造MQTT告警载荷。
std::string build_mqtt_alarm_payload(
    const MqttSettings& settings,
    const ServiceEvent& event)
{
    const nlohmann::json payload{
        {"schema", "edge.alarm.v1"},
        {"node_id", effective_node_id(settings)},
        {"alarm_id", event.event_id},
        {"rule_id", alarm_rule_id(event)},
        {"device_id", event.target_id},
        {"point_key", alarm_point_key(event)},
        {"level", event.level},
        {"severity", event.level},
        {"state", alarm_state_from_event(event)},
        {"message", event.summary},
        {"detail", event.detail},
        {"occurred_at_ms", event.timestamp_ms},
        {"timestamp_ms", event.timestamp_ms},
    };
    return payload.dump();
}

}  // namespace edge_controller
