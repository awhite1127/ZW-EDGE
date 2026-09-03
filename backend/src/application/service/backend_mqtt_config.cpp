// MQTT 配置入口：密码只写不回显，证书仅保存路径；配置应用与采集轮询相互独立。
#include "application/service/backend_service.h"

#include <mutex>

#include "shared/common/logger.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

// 读取MQTT设置。
MqttSettings BackendService::get_mqtt_settings() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    auto settings = system_config_.mqtt_settings;
    settings.password.clear();
    return settings;
}

// 更新MQTT设置。
StatusCode BackendService::update_mqtt_settings(
    const MqttSettingsUpdateRequest& request,
    MqttSettingsUpdateResult* result,
    std::string* error_message)
{
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少 MQTT 配置保存结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);

    MqttSettings next_settings = system_config_.mqtt_settings;
    next_settings.enabled = request.enabled;
    next_settings.broker_host = request.broker_host;
    next_settings.broker_port = request.broker_port;
    next_settings.client_id = request.client_id;
    next_settings.node_id = request.node_id;
    next_settings.username = request.username;
    if (request.clear_password) {
        next_settings.password.clear();
    } else if (!request.password.empty()) {
        next_settings.password = request.password;
    }
    next_settings.topic_prefix = request.topic_prefix;
    next_settings.publish_interval_seconds = request.publish_interval_seconds;
    next_settings.qos = request.qos;
    next_settings.retain_status = request.retain_status;
    next_settings.keep_alive_seconds = request.keep_alive_seconds;
    next_settings.tls_enabled = request.tls_enabled;
    next_settings.tls_ca_file = request.tls_ca_file;
    next_settings.tls_client_cert_file = request.tls_client_cert_file;
    next_settings.tls_client_key_file = request.tls_client_key_file;
    next_settings.tls_insecure = request.tls_insecure;

    std::string write_error;
    lock.unlock();
    const auto write_status = config_store_.save_mqtt_settings(next_settings, &write_error);
    if (is_ok(write_status)) {
        std::string load_error;
        const auto load_status = config_store_.load_mqtt_settings(&next_settings, &load_error);
        if (!is_ok(load_status)) {
            write_error = load_error.empty() ? "保存后重新读取 MQTT 配置失败" : load_error;
        }
    }
    lock.lock();
    if (!write_error.empty() && is_ok(write_status)) {
        if (error_message != nullptr) {
            *error_message = "保存 MQTT 配置失败: " + write_error;
        }
        return StatusCode::kIoError;
    }
    if (!is_ok(write_status)) {
        if (error_message != nullptr) {
            *error_message = write_error.empty() ? "保存 MQTT 配置失败" : "保存 MQTT 配置失败: " + write_error;
        }
        return write_status;
    }

    system_config_.mqtt_settings = next_settings;
    std::string mqtt_error;
    const auto configure_status = mqtt_publisher_service_.configure(
        next_settings,
        system_config_.settings,
        &mqtt_error);
    if (!is_ok(configure_status)) {
        result->settings = next_settings;
        result->settings.password.clear();
        result->applied = false;
        result->apply_error = mqtt_error.empty() ? "刷新 MQTT 服务配置失败" : mqtt_error;
        result->message = "配置已保存，但运行应用失败：" + result->apply_error;
        Logger::warn("MQTT 配置已保存但运行应用失败：" + result->apply_error);
        append_event("warning", "mqtt_config", next_settings.node_id, result->message, result->apply_error, 0);
        return StatusCode::kOk;
    }
    const auto start_status = mqtt_publisher_service_.start(&mqtt_error);
    if (!is_ok(start_status)) {
        result->settings = next_settings;
        result->settings.password.clear();
        result->applied = false;
        result->apply_error = mqtt_error.empty() ? "启动 MQTT 运行服务失败" : mqtt_error;
        result->message = "配置已保存，但运行应用失败：" + result->apply_error;
        Logger::warn("MQTT 配置已保存但运行应用失败：" + result->apply_error);
        append_event("warning", "mqtt_config", next_settings.node_id, result->message, result->apply_error, 0);
        return StatusCode::kOk;
    }

    result->settings = next_settings;
    result->settings.password.clear();
    result->applied = true;
    result->apply_error.clear();
    result->message = "配置已保存并应用";
    append_event(
        "info",
        "mqtt_config",
        next_settings.node_id,
        result->message,
        "",
        0);
    return StatusCode::kOk;
}

// 读取 MQTT 运行状态。
MqttRuntimeStatus BackendService::get_mqtt_runtime_status() const
{
    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    return mqtt_publisher_service_.runtime_status();
}

}  // namespace edge_controller
