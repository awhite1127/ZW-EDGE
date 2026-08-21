// MQTT 发布服务在独立线程中维护连接与周期发布；时间跳变仅设置异步重连标记。
#include "mqtt/mqtt_publisher_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "common/logger.h"
#include "common/time_utils.h"
#include "mqtt/mqtt_payload_builder.h"

namespace edge_controller {

namespace {

// 发布间隔。
std::chrono::seconds publish_interval(const MqttSettings& settings)
{
    const auto seconds = settings.publish_interval_seconds == 0
                             ? kDefaultMqttPublishIntervalSeconds
                             : settings.publish_interval_seconds;
    return std::chrono::seconds(seconds);
}

// 记录 MQTT 发布失败的诊断信息。
std::string publish_failure_log(
    const std::string& label,
    const std::string& topic,
    std::size_t payload_bytes,
    const std::string& error)
{
    return label + "失败：topic=" + topic +
           ", bytes=" + std::to_string(payload_bytes) +
           ", error=" + error;
}

// 将当前配置摘要填充到 MQTT 运行状态。
void populate_runtime_configuration(MqttRuntimeStatus* status, const MqttSettings& settings)
{
    if (status == nullptr) {
        return;
    }
    status->broker_endpoint = mqtt_broker_endpoint(settings);
    status->tls_enabled = settings.tls_enabled;
    status->tls_insecure = settings.tls_insecure;
    status->status_topic = mqtt_status_topic(settings);
    status->realtime_topic = mqtt_realtime_topic(settings);
    status->event_topic = mqtt_event_topic(settings);
    status->alarm_topic = mqtt_alarm_topic(settings);
}

// 判断 MQTT 连接与发布配置是否完全一致。
bool same_mqtt_settings(const MqttSettings& left, const MqttSettings& right)
{
    return left.enabled == right.enabled &&
           left.broker_host == right.broker_host &&
           left.broker_port == right.broker_port &&
           left.client_id == right.client_id &&
           left.node_id == right.node_id &&
           left.username == right.username &&
           left.password == right.password &&
           left.topic_prefix == right.topic_prefix &&
           left.publish_interval_seconds == right.publish_interval_seconds &&
           left.qos == right.qos &&
           left.retain_status == right.retain_status &&
           left.keep_alive_seconds == right.keep_alive_seconds &&
           left.tls_enabled == right.tls_enabled &&
           left.tls_ca_file == right.tls_ca_file &&
           left.tls_client_cert_file == right.tls_client_cert_file &&
           left.tls_client_key_file == right.tls_client_key_file &&
           left.tls_insecure == right.tls_insecure;
}

// 判断 MQTT 在线状态载荷使用的系统标识是否一致。
bool same_system_settings(const SystemSettings& left, const SystemSettings& right)
{
    return left.device_name == right.device_name &&
           left.site_location == right.site_location &&
           left.display_name == right.display_name;
}

// 载荷与主题仅依赖节点和主题前缀，避免为锁外构造复制密码和证书路径。
MqttSettings mqtt_payload_settings(const MqttSettings& settings)
{
    MqttSettings result;
    result.node_id = settings.node_id;
    result.topic_prefix = settings.topic_prefix;
    return result;
}

// 在线状态载荷只读取设备名称和站点位置。
SystemSettings mqtt_status_system_settings(const SystemSettings& settings)
{
    SystemSettings result;
    result.device_name = settings.device_name;
    result.site_location = settings.site_location;
    return result;
}

// 配置类错误等待用户修改；资源与 I/O 类启动错误允许后台恢复。
bool mqtt_start_failure_retryable(StatusCode status)
{
    return status == StatusCode::kIoError ||
           status == StatusCode::kTimeout ||
           status == StatusCode::kInternalError;
}

}  // namespace

// 构造 MqttPublisherService 实例。
MqttPublisherService::MqttPublisherService()
    : MqttPublisherService(make_mqtt_client())
{
}

// 构造 MqttPublisherService 实例。
MqttPublisherService::MqttPublisherService(std::unique_ptr<MqttClient> client)
    : client_(std::move(client))
{
    if (client_ == nullptr) {
        client_ = make_mqtt_client();
    }
    std::string ignored_error;
    client_configuration_valid_ =
        is_ok(client_->configure(settings_, &ignored_error));
}

// 销毁 MqttPublisherService 实例并释放相关资源。
MqttPublisherService::~MqttPublisherService()
{
    stop();
}

// 设置快照提供器。
void MqttPublisherService::set_snapshot_provider(SnapshotProvider provider)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_provider_ = std::move(provider);
}

// 应用 MQTT 配置并按需重建客户端。
StatusCode MqttPublisherService::configure(
    const MqttSettings& settings,
    const SystemSettings& system_settings,
    std::string* error_message)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client_configuration_valid_ &&
            same_mqtt_settings(settings_, settings) &&
            (running_ || !settings.enabled)) {
            if (!same_system_settings(system_settings_, system_settings)) {
                system_settings_ = system_settings;
                ++configuration_generation_;
            }
            if (error_message != nullptr) {
                error_message->clear();
            }
            return StatusCode::kOk;
        }
    }

    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    system_settings_ = system_settings;
    ++configuration_generation_;
    sequence_ = 1;
    time_reconnect_requested_ = false;
    client_start_pending_ = false;
    if (client_ == nullptr) {
        client_ = make_mqtt_client();
    }
    const auto status = client_->configure(settings_, error_message);
    client_configuration_valid_ = is_ok(status);
    return status;
}

// 启动 MQTT 发布线程。
StatusCode MqttPublisherService::start(std::string* error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ == nullptr) {
        client_ = make_mqtt_client();
        std::string ignored_error;
        client_configuration_valid_ =
            is_ok(client_->configure(settings_, &ignored_error));
    }
    if (!settings_.enabled) {
        stop_requested_ = true;
        running_ = false;
        return client_->start(error_message);
    }
    if (running_) {
        return StatusCode::kOk;
    }

    const auto status = client_->start(error_message);
    const bool retry_start = !is_ok(status) && mqtt_start_failure_retryable(status);
    if (!is_ok(status) && !retry_start) {
        stop_requested_ = true;
        running_ = false;
        client_start_pending_ = false;
        return status;
    }
    stop_requested_ = false;
    client_start_pending_ = retry_start;
    try {
        worker_ = std::thread([this]() {
            publish_loop();
        });
    } catch (const std::exception& error) {
        stop_requested_ = true;
        running_ = false;
        client_start_pending_ = false;
        client_->stop();
        if (error_message != nullptr) {
            *error_message = "创建 MQTT 发布线程失败：" + std::string(error.what());
        }
        return StatusCode::kInternalError;
    }
    running_ = true;
    if (retry_start) {
        const auto detail = error_message == nullptr || error_message->empty()
                                ? std::string(to_string(status))
                                : *error_message;
        Logger::warn("MQTT 同步启动失败，将在 1 秒后后台重试：" + detail);
    }
    // 临时启动失败已经交由活动 worker 管理；服务级 start 视为已接受并进入恢复态。
    return retry_start ? StatusCode::kOk : status;
}

// 停止发布线程和 MQTT 客户端。
void MqttPublisherService::stop()
{
    bool should_publish_offline = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
        should_publish_offline = client_ != nullptr && client_->runtime_status().connected;
    }
    wakeup_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    if (should_publish_offline) {
        publish_status(false);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        time_reconnect_requested_ = false;
        client_start_pending_ = false;
        if (client_ != nullptr) {
            client_->stop();
        }
    }
}

// 将设备状态发布到 MQTT。
StatusCode MqttPublisherService::publish(
    const std::string& topic,
    const std::string& payload,
    int qos,
    bool retain,
    std::string* error_message,
    std::uint64_t publish_sequence)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return publish_locked(topic, payload, qos, retain, error_message, publish_sequence);
}

// 发布事件。
void MqttPublisherService::publish_event(const ServiceEvent& event)
{
    while (true) {
        MqttSettings settings;
        int qos = 0;
        std::uint64_t configuration_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto runtime =
                client_ == nullptr ? MqttRuntimeStatus{} : client_->runtime_status();
            if (!runtime.enabled || !runtime.connected) {
                return;
            }
            settings = mqtt_payload_settings(settings_);
            qos = settings_.qos;
            configuration_generation = configuration_generation_;
        }

        const auto topic = mqtt_event_topic(settings);
        const auto payload = build_mqtt_event_payload(settings, event);
        std::string publish_error;
        StatusCode status = StatusCode::kInvalidState;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (configuration_generation != configuration_generation_) {
                continue;
            }
            const auto runtime =
                client_ == nullptr ? MqttRuntimeStatus{} : client_->runtime_status();
            if (!runtime.enabled || !runtime.connected) {
                return;
            }
            status = publish_locked(topic, payload, qos, false, &publish_error);
        }
        if (!is_ok(status) && !publish_error.empty()) {
            Logger::warn(publish_failure_log(
                "MQTT 事件发布", topic, payload.size(), publish_error));
        }
        return;
    }
}

// 发布告警。
void MqttPublisherService::publish_alarm(const ServiceEvent& event)
{
    while (true) {
        MqttSettings settings;
        int qos = 0;
        std::uint64_t configuration_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto runtime =
                client_ == nullptr ? MqttRuntimeStatus{} : client_->runtime_status();
            if (!runtime.enabled || !runtime.connected) {
                return;
            }
            settings = mqtt_payload_settings(settings_);
            qos = settings_.qos;
            configuration_generation = configuration_generation_;
        }

        const auto topic = mqtt_alarm_topic(settings);
        const auto payload = build_mqtt_alarm_payload(settings, event);
        std::string publish_error;
        StatusCode status = StatusCode::kInvalidState;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (configuration_generation != configuration_generation_) {
                continue;
            }
            const auto runtime =
                client_ == nullptr ? MqttRuntimeStatus{} : client_->runtime_status();
            if (!runtime.enabled || !runtime.connected) {
                return;
            }
            status = publish_locked(topic, payload, qos, false, &publish_error);
        }
        if (!is_ok(status) && !publish_error.empty()) {
            Logger::warn(publish_failure_log(
                "MQTT 告警发布", topic, payload.size(), publish_error));
        }
        return;
    }
}

// 系统时间调整后重置发布节流状态。
MqttTimeAdjustmentResult MqttPublisherService::on_system_time_adjusted()
{
    MqttTimeAdjustmentResult result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.enabled = settings_.enabled;
        result.tls_enabled = settings_.tls_enabled;
        if (!settings_.enabled) {
            result.detail = "MQTT 未启用，已跳过联动";
            return result;
        }
        if (!settings_.tls_enabled) {
            result.detail = "MQTT 未启用 TLS，无需强制重连";
            return result;
        }
        time_reconnect_requested_ = true;
        result.reconnect_requested = true;
        result.detail = "已通知 MQTT TLS 客户端异步重新连接";
    }
    wakeup_.notify_all();
    return result;
}

// 返回 MQTT 发布服务的运行状态快照。
MqttRuntimeStatus MqttPublisherService::runtime_status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ == nullptr) {
        MqttRuntimeStatus status;
        status.enabled = settings_.enabled;
        status.connected = false;
        status.state = settings_.enabled ? "error" : "disabled";
        populate_runtime_configuration(&status, settings_);
        if (settings_.enabled) {
            status.last_error_message = "当前构建未启用 MQTT 客户端支持";
        }
        return status;
    }
    return client_->runtime_status();
}

// 运行 MQTT 周期发布循环。
void MqttPublisherService::publish_loop()
{
    bool online_published_for_connection = false;
    std::uint32_t startup_retry_delay_seconds = 1;
    MqttSettings settings;
    {
        // configure() 会先停止并回收本线程，再替换配置，因此本次 worker 生命周期内配置不可变。
        std::lock_guard<std::mutex> lock(mutex_);
        settings = settings_;
    }

    while (true) {
        bool retry_client_start = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_requested_) {
                break;
            }
            retry_client_start = client_start_pending_;
            if (retry_client_start && wakeup_.wait_for(
                    lock,
                    std::chrono::seconds(startup_retry_delay_seconds),
                    [this]() { return stop_requested_; })) {
                break;
            }
        }
        if (retry_client_start) {
            std::string startup_error;
            StatusCode startup_status = StatusCode::kInvalidState;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_) {
                    break;
                }
                startup_status = client_ == nullptr
                                     ? StatusCode::kInvalidState
                                     : client_->start(&startup_error);
                if (is_ok(startup_status)) {
                    client_start_pending_ = false;
                } else if (!mqtt_start_failure_retryable(startup_status)) {
                    client_start_pending_ = false;
                    stop_requested_ = true;
                }
            }
            if (is_ok(startup_status)) {
                Logger::info("MQTT 同步启动后台重试成功，北向发布已恢复");
                startup_retry_delay_seconds = 1;
            } else if (!mqtt_start_failure_retryable(startup_status)) {
                Logger::error("MQTT 后台启动重试遇到配置类错误，已停止重试：" + startup_error);
                break;
            } else {
                startup_retry_delay_seconds = std::min<std::uint32_t>(
                    startup_retry_delay_seconds * 2U, 30U);
                Logger::warn(
                    "MQTT 同步启动仍失败，将在 " +
                    std::to_string(startup_retry_delay_seconds) +
                    " 秒后重试：" + startup_error);
                continue;
            }
        }

        SnapshotProvider provider;
        std::uint64_t sequence = 0;
        bool reconnect_requested = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_requested_) {
                break;
            }
            provider = snapshot_provider_;
            sequence = sequence_++;
            reconnect_requested = time_reconnect_requested_;
            time_reconnect_requested_ = false;
        }

        if (reconnect_requested) {
            std::string reconnect_error;
            StatusCode reconnect_status = StatusCode::kInvalidState;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (client_ != nullptr) {
                    reconnect_status = client_->request_reconnect(&reconnect_error);
                    if (!is_ok(reconnect_status)) {
                        // 客户端可能在此前连接失败后已释放句柄；在发布线程中重建，避免阻塞时间处理入口。
                        client_->stop();
                        std::string ignored_error;
                        client_configuration_valid_ =
                            is_ok(client_->configure(settings, &ignored_error));
                        reconnect_status = client_->start(&reconnect_error);
                    }
                }
            }
            if (!is_ok(reconnect_status)) {
                Logger::warn("系统时间调整后 MQTT TLS 重连请求失败：" + reconnect_error);
            } else {
                Logger::info("系统时间调整后已触发 MQTT TLS 重新连接");
            }
        }

        const auto runtime = runtime_status();
        if (!runtime.connected) {
            online_published_for_connection = false;
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return stop_requested_;
            });
            continue;
        }

        if (!online_published_for_connection) {
            publish_status(true);
            online_published_for_connection = true;
        }

        if (provider != nullptr) {
            const auto snapshot = provider();
            const auto payload = build_mqtt_realtime_payload(
                settings,
                snapshot,
                sequence,
                time_utils::system_now_ms());
            const auto topic = mqtt_realtime_topic(settings);
            std::string publish_error;
            const auto status = publish(
                topic,
                payload,
                settings.qos,
                false,
                &publish_error,
                sequence);
            if (!is_ok(status) && !publish_error.empty()) {
                Logger::warn(publish_failure_log("MQTT 实时数据发布", topic, payload.size(), publish_error));
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        wakeup_.wait_for(lock, publish_interval(settings), [this]() {
            return stop_requested_;
        });
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

// 发布状态。
void MqttPublisherService::publish_status(bool online)
{
    while (true) {
        MqttSettings settings;
        SystemSettings system_settings;
        int qos = 0;
        bool retain = false;
        std::uint64_t configuration_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings = mqtt_payload_settings(settings_);
            system_settings = mqtt_status_system_settings(system_settings_);
            qos = settings_.qos;
            retain = settings_.retain_status;
            configuration_generation = configuration_generation_;
        }

        const auto payload = build_mqtt_status_payload(
            settings,
            system_settings,
            online,
            time_utils::system_now_ms());
        const auto topic = mqtt_status_topic(settings);
        std::string publish_error;
        StatusCode status = StatusCode::kInvalidState;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (configuration_generation != configuration_generation_) {
                continue;
            }
            status = publish_locked(
                topic,
                payload,
                qos,
                retain,
                &publish_error);
        }
        if (!is_ok(status) && !publish_error.empty()) {
            Logger::warn(publish_failure_log(
                "MQTT 状态发布", topic, payload.size(), publish_error));
        }
        return;
    }
}

// 在持锁状态下发布。
StatusCode MqttPublisherService::publish_locked(
    const std::string& topic,
    const std::string& payload,
    int qos,
    bool retain,
    std::string* error_message,
    std::uint64_t publish_sequence)
{
    if (client_ == nullptr) {
        client_ = make_mqtt_client();
        std::string ignored_error;
        client_configuration_valid_ =
            is_ok(client_->configure(settings_, &ignored_error));
    }
    return client_->publish(topic, payload, qos, retain, error_message, publish_sequence);
}

}  // namespace edge_controller
