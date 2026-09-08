// libmosquitto 适配器：封装 TLS 配置、连接回调和发布统计，使上层不依赖具体客户端 API。
#include "communication/mqtt/mqtt_client.h"
#include <unordered_map>
#include <unordered_set>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <mosquitto.h>

#include "shared/common/time_utils.h"
#include "communication/mqtt/mqtt_payload_builder.h"

namespace edge_controller {

namespace {

// 将 mosquitto 错误码转换为可读文本。
std::string mosquitto_error_message(const std::string& prefix, int code)
{
    const char* detail = mosquitto_strerror(code);
    return prefix + (detail == nullptr ? std::string("未知错误") : std::string(detail));
}

// 将 MQTT 连接返回码转换为可读文本。
std::string connect_error_message(int code)
{
    const char* detail = mosquitto_connack_string(code);
    return std::string("MQTT Broker 连接失败：") +
           (detail == nullptr ? std::string("未知错误") : std::string(detail));
}

// 返回配置生效后的 MQTT 客户端标识。
std::string effective_client_id(const MqttSettings& settings)
{
    if (!settings.client_id.empty()) {
        return settings.client_id;
    }
    if (!settings.node_id.empty()) {
        return settings.node_id;
    }
    return kDefaultMqttNodeId;
}

class MosquittoLibraryLifecycle {
public:
    // 初始化 mosquitto 全局库；内部加锁保护生命周期计数。
    MosquittoLibraryLifecycle()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ref_count_ == 0) {
            mosquitto_lib_init();
        }
        ++ref_count_;
    }

    // 关闭数据库连接并释放存储资源；内部加锁保证析构安全。
    ~MosquittoLibraryLifecycle()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ref_count_ == 0) {
            return;
        }
        --ref_count_;
        if (ref_count_ == 0) {
            mosquitto_lib_cleanup();
        }
    }

    MosquittoLibraryLifecycle(const MosquittoLibraryLifecycle&) = delete;
    // 移动赋值对象并转移其资源所有权。
    MosquittoLibraryLifecycle& operator=(const MosquittoLibraryLifecycle&) = delete;

private:
    inline static std::mutex mutex_{};
    inline static std::size_t ref_count_{0};
};

}  // namespace

class MosquittoMqttClient final : public MqttClient {
public:

    ~MosquittoMqttClient() override
    {
        stop();
    }

    // 应用 MQTT 客户端配置并重建连接状态。
    StatusCode configure(const MqttSettings& settings, std::string* error_message) override
    {
        (void)error_message;
        stop();
        std::lock_guard<std::mutex> lock(status_mutex_);
        settings_ = settings;
        status_ = {};
        status_.enabled = settings.enabled;
        status_.connected = false;
        status_.state = settings.enabled ? "disconnected" : "disabled";
        populate_runtime_configuration_locked(settings);
        return StatusCode::kOk;
    }

    // 启动。
    StatusCode start(std::string* error_message) override
    {
        if (error_message != nullptr) {
            error_message->clear();
        }
        MqttSettings settings;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            settings = settings_;
            status_.enabled = settings.enabled;
            status_.connected = false;
            status_.state = settings.enabled ? "connecting" : "disabled";
            populate_runtime_configuration_locked(settings);
            if (!settings.enabled) {
                status_.last_error_message.clear();
                return StatusCode::kOk;
            }
        }

        std::lock_guard<std::mutex> mqtt_lock(mqtt_mutex_);
        if (mosq_ != nullptr) {
            return StatusCode::kOk;
        }

        lifecycle_ = std::make_unique<MosquittoLibraryLifecycle>();
        mosq_ = mosquitto_new(effective_client_id(settings).c_str(), true, this);
        if (mosq_ == nullptr) {
            set_error("创建 MQTT 客户端失败", true);
            if (error_message != nullptr) {
                *error_message = runtime_status().last_error_message;
            }
            lifecycle_.reset();
        std::lock_guard<std::mutex> lock(receipt_mutex_);
        pending_receipts_.clear();
        acknowledged_.clear();
            return StatusCode::kInternalError;
        }

        mosquitto_publish_callback_set(mosq_, &MosquittoMqttClient::on_publish);
        mosquitto_connect_callback_set(mosq_, &MosquittoMqttClient::on_connect);
        mosquitto_disconnect_callback_set(mosq_, &MosquittoMqttClient::on_disconnect);
        mosquitto_reconnect_delay_set(mosq_, 1, 30, true);

        if (settings.tls_enabled) {
            const auto tls_status = apply_tls_settings_locked(settings, error_message);
            if (!is_ok(tls_status)) {
                return tls_status;
            }
        }

        if (!settings.username.empty() || !settings.password.empty()) {
            const auto auth_result = mosquitto_username_pw_set(
                mosq_,
                settings.username.empty() ? nullptr : settings.username.c_str(),
                settings.password.empty() ? nullptr : settings.password.c_str());
            if (auth_result != MOSQ_ERR_SUCCESS) {
                set_error(mosquitto_error_message("设置 MQTT 用户名密码失败：", auth_result), true);
                cleanup_mosquitto_locked();
                if (error_message != nullptr) {
                    *error_message = runtime_status().last_error_message;
                }
                return StatusCode::kInvalidArgument;
            }
        }

        const auto will_payload = build_mqtt_status_payload(
            settings,
            SystemSettings{},
            false,
            time_utils::system_now_ms());
        const auto will_topic = mqtt_status_topic(settings);
        const auto will_result = mosquitto_will_set(
            mosq_,
            will_topic.c_str(),
            static_cast<int>(will_payload.size()),
            will_payload.data(),
            settings.qos,
            settings.retain_status);
        if (will_result != MOSQ_ERR_SUCCESS) {
            set_error(mosquitto_error_message("设置 MQTT 离线遗嘱失败：", will_result), true);
            cleanup_mosquitto_locked();
            if (error_message != nullptr) {
                *error_message = runtime_status().last_error_message;
            }
            return StatusCode::kInvalidArgument;
        }

        const auto connect_result = mosquitto_connect_async(
            mosq_,
            settings.broker_host.c_str(),
            static_cast<int>(settings.broker_port),
            static_cast<int>(settings.keep_alive_seconds));
        if (connect_result != MOSQ_ERR_SUCCESS) {
            set_error(mosquitto_error_message("发起 MQTT Broker 连接失败：", connect_result), true);
            cleanup_mosquitto_locked();
            if (error_message != nullptr) {
                *error_message = runtime_status().last_error_message;
            }
            return StatusCode::kIoError;
        }

        const auto loop_result = mosquitto_loop_start(mosq_);
        if (loop_result != MOSQ_ERR_SUCCESS) {
            set_error(mosquitto_error_message("启动 MQTT 网络循环失败：", loop_result), true);
            cleanup_mosquitto_locked();
            if (error_message != nullptr) {
                *error_message = runtime_status().last_error_message;
            }
            return StatusCode::kIoError;
        }

        return StatusCode::kOk;
    }

    // 停止。
    void stop() override
    {
        std::lock_guard<std::mutex> mqtt_lock(mqtt_mutex_);
        if (mosq_ != nullptr) {
            mosquitto_disconnect(mosq_);
            mosquitto_loop_stop(mosq_, true);
            cleanup_mosquitto_locked();
        }

        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.connected = false;
        status_.state = settings_.enabled ? "disconnected" : "disabled";
        if (settings_.enabled) {
            status_.last_disconnect_time_ms = time_utils::system_now_ms();
        } else {
            status_.last_error_message.clear();
        }
    }

    // 请求 MQTT 客户端断开并重新连接。
    StatusCode request_reconnect(std::string* error_message) override
    {
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            if (!settings_.enabled) {
                return StatusCode::kOk;
            }
        }

        std::lock_guard<std::mutex> mqtt_lock(mqtt_mutex_);
        if (mosq_ == nullptr) {
            if (error_message != nullptr) *error_message = "MQTT 客户端尚未建立，需重新启动客户端";
            return StatusCode::kInvalidState;
        }
        (void)mosquitto_disconnect(mosq_);
        const auto result = mosquitto_reconnect_async(mosq_);
        if (result != MOSQ_ERR_SUCCESS) {
            const auto message = mosquitto_error_message("请求 MQTT TLS 重连失败：", result);
            set_error(message, true);
            if (error_message != nullptr) *error_message = message;
            return StatusCode::kIoError;
        }
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            status_.connected = false;
            status_.state = "reconnecting";
            status_.last_error_message.clear();
            status_.last_publish_error_message.clear();
        }
        return StatusCode::kOk;
    }

    // 发布。
    StatusCode publish(
        const std::string& topic,
        const std::string& payload,
        int qos,
        bool retain,
        std::string* error_message,
        std::uint64_t publish_sequence) override
    {
        {
            std::lock_guard<std::mutex> status_lock(status_mutex_);
            if (!settings_.enabled) {
                status_.last_publish_topic = topic;
                status_.last_publish_payload_bytes = payload.size();
                status_.last_publish_error_message = "MQTT 北向发布未启用";
                status_.last_publish_sequence = publish_sequence;
                if (error_message != nullptr) {
                    *error_message = status_.last_publish_error_message;
                }
                return StatusCode::kInvalidState;
            }
            if (!status_.connected) {
                ++status_.failed_publish_count;
                status_.last_error_message = "MQTT Broker 尚未连接";
                status_.last_publish_topic = topic;
                status_.last_publish_payload_bytes = payload.size();
                status_.last_publish_error_message = status_.last_error_message;
                status_.last_publish_sequence = publish_sequence;
                if (error_message != nullptr) {
                    *error_message = status_.last_error_message;
                }
                return StatusCode::kInvalidState;
            }
        }

        std::lock_guard<std::mutex> mqtt_lock(mqtt_mutex_);
        if (mosq_ == nullptr) {
            set_publish_failure(topic, payload.size(), publish_sequence, "MQTT 客户端尚未启动", error_message);
            return StatusCode::kInvalidState;
        }

        std::unique_lock<std::mutex> receipt_lock(receipt_mutex_);
        int message_id = 0;
        const auto result = mosquitto_publish(
            mosq_,
            &message_id,
            topic.c_str(),
            static_cast<int>(payload.size()),
            payload.data(),
            qos,
            retain);
        if (result == MOSQ_ERR_SUCCESS && (publish_sequence & (1ULL << 63)) != 0 && qos > 0)
            pending_receipts_[message_id] = publish_sequence;
        receipt_lock.unlock();
        if (result != MOSQ_ERR_SUCCESS) {
            set_publish_failure(
                topic,
                payload.size(),
                publish_sequence,
                mosquitto_error_message("MQTT 发布失败：", result),
                error_message);
            if (result == MOSQ_ERR_NO_CONN) {
                std::lock_guard<std::mutex> status_lock(status_mutex_);
                status_.connected = false;
                status_.state = "disconnected";
            }
            return StatusCode::kInvalidState;
        }

        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.last_publish_time_ms = time_utils::system_now_ms();
        status_.last_publish_topic = topic;
        status_.last_publish_payload_bytes = payload.size();
        status_.last_publish_error_message.clear();
        status_.last_publish_sequence = publish_sequence;
        if (topic == status_.event_topic) {
            status_.last_event_publish_time_ms = status_.last_publish_time_ms;
            status_.last_event_publish_topic = topic;
            ++status_.event_publish_count;
        } else if (topic == status_.alarm_topic) {
            status_.last_alarm_publish_time_ms = status_.last_publish_time_ms;
            status_.last_alarm_publish_topic = topic;
            ++status_.alarm_publish_count;
        }
        ++status_.published_message_count;
        return StatusCode::kOk;
    }

    // 返回 MQTT 客户端运行状态快照。
    MqttRuntimeStatus runtime_status() const override
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_;
    }

    bool consume_publish_ack(std::uint64_t sequence) override {
        std::lock_guard<std::mutex> lock(receipt_mutex_);
        return acknowledged_.erase(sequence) != 0;
    }
    void forget_publish(std::uint64_t sequence) override {
        std::lock_guard<std::mutex> lock(receipt_mutex_);
        acknowledged_.erase(sequence);
        for (auto it = pending_receipts_.begin(); it != pending_receipts_.end();) {
            if (it->second == sequence) it = pending_receipts_.erase(it); else ++it;
        }
    }
private:
    static void on_publish(mosquitto*, void* userdata, int message_id) {
        auto* self = static_cast<MosquittoMqttClient*>(userdata);
        std::lock_guard<std::mutex> lock(self->receipt_mutex_);
        const auto receipt = self->pending_receipts_.find(message_id);
        if (receipt != self->pending_receipts_.end()) {
            self->acknowledged_.insert(receipt->second);
            self->pending_receipts_.erase(receipt);
        }
    }
    // 处理 MQTT 连接结果回调并更新状态。
    static void on_connect(mosquitto*, void* userdata, int rc)
    {
        auto* self = static_cast<MosquittoMqttClient*>(userdata);
        if (self == nullptr) {
            return;
        }
        self->handle_connect(rc);
    }

    // 处理 MQTT 断开回调并更新状态。
    static void on_disconnect(mosquitto*, void* userdata, int rc)
    {
        auto* self = static_cast<MosquittoMqttClient*>(userdata);
        if (self == nullptr) {
            return;
        }
        self->handle_disconnect(rc);
    }

    // 处理连接。
    void handle_connect(int rc)
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.enabled = settings_.enabled;
        if (rc == 0) {
            status_.connected = true;
            status_.state = "connected";
            status_.last_error_message.clear();
            status_.last_connect_time_ms = time_utils::system_now_ms();
            return;
        }
        status_.connected = false;
        status_.state = "error";
        status_.last_disconnect_time_ms = time_utils::system_now_ms();
        status_.last_error_message = connect_error_message(rc);
    }

    // 处理断开。
    void handle_disconnect(int rc)
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.enabled = settings_.enabled;
        status_.connected = false;
        status_.last_disconnect_time_ms = time_utils::system_now_ms();
        if (rc == 0) {
            status_.state = settings_.enabled ? "disconnected" : "disabled";
            return;
        }
        status_.state = settings_.enabled ? "reconnecting" : "disabled";
        status_.last_error_message = mosquitto_error_message("MQTT Broker 连接已断开：", rc);
    }

    // 设置错误。
    void set_error(const std::string& message, bool disconnected)
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.enabled = settings_.enabled;
        status_.connected = false;
        status_.state = disconnected ? "error" : "disconnected";
        status_.last_error_message = message;
        status_.last_disconnect_time_ms = time_utils::system_now_ms();
    }

    // 在持锁状态下填充 MQTT 运行状态中的配置摘要。
    void populate_runtime_configuration_locked(const MqttSettings& settings)
    {
        status_.broker_endpoint = mqtt_broker_endpoint(settings);
        status_.tls_enabled = settings.tls_enabled;
        status_.tls_insecure = settings.tls_insecure;
        status_.status_topic = mqtt_status_topic(settings);
        status_.realtime_topic = mqtt_realtime_topic(settings);
        status_.event_topic = mqtt_event_topic(settings);
        status_.alarm_topic = mqtt_alarm_topic(settings);
    }

    // 设置发布失败。
    void set_publish_failure(
        const std::string& topic,
        std::size_t payload_bytes,
        std::uint64_t publish_sequence,
        const std::string& message,
        std::string* error_message)
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        ++status_.failed_publish_count;
        status_.last_error_message = message;
        status_.last_publish_topic = topic;
        status_.last_publish_payload_bytes = payload_bytes;
        status_.last_publish_error_message = message;
        status_.last_publish_sequence = publish_sequence;
        if (error_message != nullptr) {
            *error_message = message;
        }
    }

    // 在持锁状态下应用 MQTT TLS 设置。
    StatusCode apply_tls_settings_locked(const MqttSettings& settings, std::string* error_message)
    {
        const char* cert_file = settings.tls_client_cert_file.empty() ? nullptr : settings.tls_client_cert_file.c_str();
        const char* key_file = settings.tls_client_key_file.empty() ? nullptr : settings.tls_client_key_file.c_str();
        const auto tls_result = mosquitto_tls_set(
            mosq_,
            settings.tls_ca_file.c_str(),
            nullptr,
            cert_file,
            key_file,
            nullptr);
        if (tls_result != MOSQ_ERR_SUCCESS) {
            set_error(mosquitto_error_message("设置 MQTT TLS 证书失败：", tls_result), true);
            cleanup_mosquitto_locked();
            if (error_message != nullptr) {
                *error_message = runtime_status().last_error_message;
            }
            return tls_result == MOSQ_ERR_INVAL || tls_result == MOSQ_ERR_TLS
                       ? StatusCode::kInvalidArgument
                       : StatusCode::kIoError;
        }

        if (settings.tls_insecure) {
            const auto insecure_result = mosquitto_tls_insecure_set(mosq_, true);
            if (insecure_result != MOSQ_ERR_SUCCESS) {
                set_error(mosquitto_error_message("设置 MQTT TLS 非安全校验失败：", insecure_result), true);
                cleanup_mosquitto_locked();
                if (error_message != nullptr) {
                    *error_message = runtime_status().last_error_message;
                }
                return insecure_result == MOSQ_ERR_INVAL
                           ? StatusCode::kInvalidArgument
                           : StatusCode::kIoError;
            }
        }
        return StatusCode::kOk;
    }

    // 在持锁状态下释放 mosquitto 客户端资源。
    void cleanup_mosquitto_locked()
    {
        if (mosq_ != nullptr) {
            mosquitto_destroy(mosq_);
            mosq_ = nullptr;
        }
        lifecycle_.reset();
    }

    std::mutex receipt_mutex_;
    std::unordered_map<int, std::uint64_t> pending_receipts_;
    std::unordered_set<std::uint64_t> acknowledged_;
    mutable std::mutex status_mutex_;
    std::mutex mqtt_mutex_;
    MqttSettings settings_{};
    MqttRuntimeStatus status_{};
    mosquitto* mosq_{nullptr};
    std::unique_ptr<MosquittoLibraryLifecycle> lifecycle_;
};

// 创建MQTT客户端。
std::unique_ptr<MqttClient> make_mqtt_client()
{
    return std::make_unique<MosquittoMqttClient>();
}

}  // namespace edge_controller
