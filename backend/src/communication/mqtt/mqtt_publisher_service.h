// MQTT 后台发布服务的线程、配置和运行态接口。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <array>

#include "shared/common/status_code.h"
#include "data/model/mqtt_settings.h"
#include "data/model/realtime_view_snapshot.h"
#include "data/model/service_summary.h"
#include "data/model/system_settings.h"
#include "data/model/time_change.h"
#include "communication/mqtt/mqtt_client.h"

namespace edge_controller {

class MqttPublisherService {
public:
    using SnapshotProvider = std::function<RealtimeViewSnapshot()>;

    MqttPublisherService();

    explicit MqttPublisherService(std::unique_ptr<MqttClient> client);

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
    // 返回 true 表示两类主题均已获 Broker 确认，或用户明确禁用 MQTT。
    bool deliver_durable_event(const ServiceEvent& event);
    // 发布事件。
    void publish_event(const ServiceEvent& event);
    // 发布告警。
    void publish_alarm(const ServiceEvent& event);
    // 处理系统时间调整并请求 MQTT 重连。
    MqttTimeAdjustmentResult on_system_time_adjusted();
    // 获取当前运行状态。
    MqttRuntimeStatus runtime_status() const;

private:
    // 事件与告警共享配置快照、世代检查及发布边界。
    void publish_service_event(const ServiceEvent& event, bool alarm);
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
    struct DurableDelivery {
        std::uint64_t generation{0};
        std::array<std::uint64_t, 2> sequences{};
        std::array<bool, 2> confirmed{};
    };
    std::unordered_map<std::string, DurableDelivery> durable_deliveries_;
    std::uint64_t sequence_{1};
    // 两阶段发布在锁外构造载荷；代次用于阻止旧配置载荷经新客户端发布。
    std::uint64_t configuration_generation_{0};
};

}  // namespace edge_controller
