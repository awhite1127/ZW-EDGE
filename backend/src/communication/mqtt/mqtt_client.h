// 定义 MQTT 客户端能力边界，隔离发布服务与 mosquitto。
// 边界：发布失败不得阻塞采集，既有主题与字段语义保持兼容。

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "shared/common/status_code.h"
#include "data/model/mqtt_settings.h"

namespace edge_controller {

class MqttClient {
public:

    virtual ~MqttClient() = default;

    // 校验并应用 MQTT 客户端配置。
    virtual StatusCode configure(const MqttSettings& settings, std::string* error_message = nullptr) = 0;
    // 启动。
    virtual StatusCode start(std::string* error_message = nullptr) = 0;
    // 停止。
    virtual void stop() = 0;
    // 系统时间明显跳变后请求异步重连，主要用于重新执行 TLS 证书时间校验。
    virtual StatusCode request_reconnect(std::string* error_message = nullptr) = 0;
    // 发布。
    virtual StatusCode publish(
        const std::string& topic,
        const std::string& payload,
        int qos,
        bool retain,
        std::string* error_message = nullptr,
        std::uint64_t publish_sequence = 0) = 0;
    // 可靠事件使用非零 sequence 查询 Broker 确认；默认客户端不声称已确认。
    virtual bool consume_publish_ack(std::uint64_t) { return false; }
    virtual void forget_publish(std::uint64_t) {}
    // 获取当前运行状态。
    virtual MqttRuntimeStatus runtime_status() const = 0;
};

// 创建MQTT客户端。
std::unique_ptr<MqttClient> make_mqtt_client();

}  // namespace edge_controller
