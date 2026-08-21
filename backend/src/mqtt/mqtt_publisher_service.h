// MQTT 后台发布服务的线程、配置和运行态接口。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/status_code.h"
#include "model/mqtt_settings.h"
#include "model/realtime_view_snapshot.h"
#include "model/service_summary.h"
#include "model/system_settings.h"
#include "model/time_change.h"
#include "mqtt/mqtt_client.h"

namespace edge_controller {

class MqttPublisherService {
public:
    using SnapshotProvider = std::function<RealtimeViewSnapshot()>;

    // 构造 MqttPublisherService 实例。
    MqttPublisherService();
    // 构造 MqttPublisherService 实例。
    explicit MqttPublisherService(std::unique_ptr<MqttClient> client);
    // 销毁 MqttPublisherService 实例并释放相关资源。
    ~MqttPublisherService();

    // 设置实时快照提供器。
    void set_snapshot_provider(SnapshotProvider provider);
    // 校验并应用 MQTT 客户端配置。
    StatusCode configure(
        const MqttSettings& settings,
        const SystemSettings& system_settings,
        std::string* error_message = nullptr);
    // 启动。
    StatusCode start(std::string* error_message = nullptr);
    // 停止。
    void stop();
    // 发布。
    StatusCode publish(
        const std::string& topic,
        const std::string& payload,
        int qos,
        bool retain,
        std::string* error_message = nullptr,
        std::uint64_t publish_sequence = 0);
    // 发布事件。
    void publish_event(const ServiceEvent& event);
    // 发布告警。
    void publish_alarm(const ServiceEvent& event);
    // 处理系统时间调整并请求 MQTT 重连。
    MqttTimeAdjustmentResult on_system_time_adjusted();
    // 获取当前运行状态。
    MqttRuntimeStatus runtime_status() const;

private:
    // 在后台循环中维护连接并执行周期发布。
    void publish_loop();
    // 发布状态。
    void publish_status(bool online);
    // 在持锁状态下发布。
    StatusCode publish_locked(
        const std::string& topic,
        const std::string& payload,
        int qos,
        bool retain,
        std::string* error_message = nullptr,
        std::uint64_t publish_sequence = 0);

    // 配置、客户端所有权和发布线程状态共享同一把锁；快照提供者返回的是可脱锁使用的值对象。
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    MqttSettings settings_{};
    SystemSettings system_settings_{};
    std::unique_ptr<MqttClient> client_;
    SnapshotProvider snapshot_provider_;
    std::thread worker_;
    bool stop_requested_{true};
    bool running_{false};
    // 同步 client start 的临时失败由同一 publisher worker 负责退避重试。
    bool client_start_pending_{false};
    bool time_reconnect_requested_{false};
    bool client_configuration_valid_{false};
    std::uint64_t sequence_{1};
    // 两阶段发布在锁外构造载荷；代次用于阻止旧配置载荷经新客户端发布。
    std::uint64_t configuration_generation_{0};
};

}  // namespace edge_controller
