// 配置包导入导出：version 4 完整携带自定义设备类型和显式设备数量。
#include "data/model/builtin_device_templates.h"
#include "data/model/alarm_validation.h"
#include "application/service/backend_service.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "shared/common/logger.h"
#include "data/datastore/config_import_transaction.h"
#include "data/datastore/config_store_internal.h"
#include "shared/common/time_utils.h"
#include "application/service/backend_service_internal.h"

namespace edge_controller {

using namespace backend_internal;

namespace {

constexpr const char kConfigBundleFormat[] = "edge-controller-config";
constexpr std::uint32_t kConfigBundleVersion = 4;

struct RestoreOutcome {
    StatusCode status{StatusCode::kOk};
    std::string details;

    void record(StatusCode candidate, const std::string& step, const std::string& detail)
    {
        if (is_ok(candidate)) return;
        if (is_ok(status)) status = candidate;
        if (!details.empty()) details += "；";
        details += step;
        if (!detail.empty()) details += "：" + detail;
    }

    bool succeeded() const { return is_ok(status); }
};

// 将通道配置转换为导入更新请求。
ChannelConfigUpdateRequest channel_import_request(const ChannelConfig& config)
{
    ChannelConfigUpdateRequest request;
    request.channel_id = config.channel_id;
    request.channel_name = config.channel_name;
    request.enabled = config.enabled;
    request.channel_type = config.channel_type;
    request.port_name = config.port_name.empty() ? config.device_path : config.port_name;
    request.baud_rate = config.baud_rate;
    request.data_bits = config.data_bits;
    request.parity = config.parity;
    request.stop_bits = config.stop_bits;
    request.response_timeout_ms = config.response_timeout_ms;
    request.retry_count = config.retry_count;
    request.tcp_host = config.tcp_host;
    request.tcp_port = config.tcp_port;
    request.connect_timeout_ms = config.connect_timeout_ms;
    return request;
}

// 将主站配置转换为导入更新请求。
MasterNodeConfigUpdateRequest master_import_request(const MasterNodeConfig& config)
{
    MasterNodeConfigUpdateRequest request;
    request.master_id = config.master_id;
    request.master_name = config.master_name;
    request.enabled = config.enabled;
    request.protocol = config.protocol;
    request.channel_id = config.channel_id;
    request.target_address = config.target_address;
    request.poll_interval_ms = config.poll_interval_ms;
    request.response_timeout_ms = config.response_timeout_ms;
    request.retry_count = config.retry_count;
    request.remark = config.remark;
    request.device_template = config.device_template;
    request.block_start_register = config.block_start_register;
    request.device_count = config.device_count;
    return request;
}

// 将网络设置转换为导入更新请求。
NetworkSettingsUpdateRequest network_import_request(const NetworkSettings& settings)
{
    return NetworkSettingsUpdateRequest{
        settings.mode,
        settings.interface_name,
        settings.ip_address,
        settings.netmask,
        settings.gateway,
        settings.dns_servers,
    };
}

struct ValidatedImportPayload {
    SystemSettings system_settings;
    TimeSettings time_settings;
    NetworkSettings network_settings;
    MqttSettings mqtt_settings;
    std::vector<DeviceTemplateDefinition> custom_device_types;
    std::vector<ChannelConfig> channels;
    std::vector<MasterNodeConfig> masters;
    ModbusServerSettings modbus_server_settings;
    std::vector<ModbusRegisterMapping> modbus_register_mappings;
};

class ScopedEventPersistenceSuppression {
public:

    explicit ScopedEventPersistenceSuppression(std::atomic_bool& flag) : flag_(flag)
    {
        flag_.store(true);
    }

    ~ScopedEventPersistenceSuppression() { flag_.store(false); }

private:
    std::atomic_bool& flag_;
};

// 校验配置包中的告警规则。
StatusCode validate_import_alarm_rules(
    const std::vector<AlarmRule>& rules,
    std::string* error_message)
{
    std::set<std::string> alarm_keys;
    for (const auto& rule : rules) {
        const auto key = rule.device_id + "\n" + rule.point_key;
        if (rule.device_id.empty() || rule.point_key.empty()) {
            if (error_message != nullptr) {
                *error_message = "告警规则存在空设备、空数据项或重复项";
            }
            return StatusCode::kInvalidArgument;
        }
        if (!alarm_keys.insert(key).second) {
            if (error_message != nullptr) {
                *error_message = "告警规则存在空设备、空数据项或重复项";
            }
            return StatusCode::kInvalidArgument;
        }
        std::string rule_error;
        const auto validation_status = validate_alarm_rule(rule, &rule_error);
        if (!is_ok(validation_status)) {
            if (error_message != nullptr) {
                *error_message = "告警规则校验失败：" + rule_error;
            }
            return validation_status;
        }
    }
    return StatusCode::kOk;
}

// 校验配置导入包的版本、引用和业务约束。
StatusCode validate_import_bundle(
    const ConfigExportBundle& bundle,
    const SystemConfig& current_config,
    const DeviceTemplateStore& device_template_store,
    ValidatedImportPayload* payload,
    std::string* error_message)
{
    // 校验输出参数、配置包格式、版本和必填时间戳。
    if (payload == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少配置导入校验输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    if (bundle.format != kConfigBundleFormat) {
        if (error_message != nullptr) {
            *error_message = "配置文件格式不匹配";
        }
        return StatusCode::kInvalidArgument;
    }
    if (bundle.version != kConfigBundleVersion) {
        if (error_message != nullptr) {
            *error_message = "配置文件版本不受支持，仅支持当前 version 4";
        }
        return StatusCode::kInvalidArgument;
    }
    if (bundle.exported_at_ms == 0 || bundle.time_settings.updated_at == 0) {
        if (error_message != nullptr) *error_message = "配置文件缺少必填时间戳字段";
        return StatusCode::kInvalidArgument;
    }
    if (!is_ok(validate_import_resource_ids(bundle.channels, bundle.masters, error_message)) ||
        !is_ok(validate_import_channel_limits(bundle.channels, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    // 规范化系统、时间和网络设置。
    SystemSettingsUpdateRequest system_request{
        bundle.system_settings.device_name,
        bundle.system_settings.site_location,
        bundle.system_settings.display_name,
    };
    SystemSettings normalized_system;
    if (!is_ok(validate_system_settings_request(system_request, &normalized_system, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    payload->system_settings = normalized_system;

    payload->time_settings = bundle.time_settings;
    if (!is_ok(LinuxTimeRuntime::validate_settings_shape(payload->time_settings, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    NetworkSettings normalized_network;
    if (!is_ok(canonicalize_network_settings_request(
            network_import_request(bundle.network_settings),
            &current_config.network_settings,
            &normalized_network,
            error_message))) {
        return StatusCode::kInvalidArgument;
    }
    payload->network_settings = normalized_network;

    // 校验自定义设备类型，并合并为主站校验可见的类型清单。
    std::set<std::string> builtin_template_ids;
    auto available_templates = builtin_device_templates();
    for (const auto& builtin_template : available_templates) {
        builtin_template_ids.insert(builtin_template.template_id);
    }
    std::set<std::string> custom_template_ids;
    for (auto device_template : bundle.custom_device_types) {
        device_template.builtin = false;
        // 配置包中的旧开发期 FC10 定义不再传播或恢复为可执行能力。
        device_template.write_commands.clear();
        const auto template_id = trim_copy(device_template.template_id);
        const auto template_label = trim_copy(device_template.display_name).empty()
                                        ? template_id
                                        : trim_copy(device_template.display_name);
        if (template_id.empty() || !custom_template_ids.insert(template_id).second) {
            if (error_message != nullptr) {
                *error_message = template_id.empty()
                                     ? "自定义设备类型 template_id 不能为空"
                                     : "自定义设备类型标识重复：" + template_id;
            }
            return StatusCode::kInvalidArgument;
        }
        if (builtin_template_ids.find(template_id) != builtin_template_ids.end()) {
            if (error_message != nullptr) {
                *error_message = "配置包不能覆盖内置设备类型“" + template_label + "”（" + template_id + "）";
            }
            return StatusCode::kInvalidArgument;
        }
        std::string template_error;
        if (!is_ok(device_template_store.validate_template_definition(device_template, &template_error))) {
            if (error_message != nullptr) {
                *error_message = "设备类型“" + template_label + "”（" + template_id + "）无效：" + template_error;
            }
            return StatusCode::kInvalidArgument;
        }
        payload->custom_device_types.push_back(device_template);
        available_templates.push_back(std::move(device_template));
    }

    // 校验通道与主站配置及其关联关系。
    std::vector<ChannelConfig> validated_channels;
    validated_channels.reserve(bundle.channels.size());
    std::vector<SerialPortInfo> no_serial_scan;
    for (const auto& channel : bundle.channels) {
        std::string warning;
        if (!is_ok(validate_channel_update_request(
                validated_channels,
                channel_import_request(channel),
                no_serial_scan,
                &warning,
                error_message))) {
            return StatusCode::kInvalidArgument;
        }
        validated_channels.push_back(build_new_channel_config(channel_import_request(channel)));
    }
    payload->channels = validated_channels;

    SystemConfig config_for_masters = current_config;
    config_for_masters.channels = validated_channels;
    config_for_masters.master_nodes.clear();
    for (const auto& master : bundle.masters) {
        const auto request = master_import_request(master);
        std::string master_error;
        if (!is_ok(validate_master_update_request_with_templates(
                config_for_masters, available_templates, request, true, &master_error))) {
            if (error_message != nullptr) {
                const auto master_label = trim_copy(master.master_name).empty()
                                              ? trim_copy(master.master_id)
                                              : trim_copy(master.master_name) + "（" + trim_copy(master.master_id) + "）";
                *error_message = "主站“" + master_label + "”无效：" + master_error;
            }
            return StatusCode::kInvalidArgument;
        }
        config_for_masters.master_nodes.push_back(build_updated_master_config(request));
    }
    payload->masters = config_for_masters.master_nodes;

    // 校验 MQTT、报警规则和 Modbus 服务端配置。
    MqttSettings next_mqtt = bundle.mqtt_settings;
    next_mqtt.password.clear();
    std::string mqtt_error;
    if (!is_ok(config_store_internal::validate_mqtt_settings(next_mqtt, "mqtt_settings", &mqtt_error))) {
        if (error_message != nullptr) {
            *error_message = mqtt_error;
        }
        return StatusCode::kInvalidArgument;
    }
    payload->mqtt_settings = next_mqtt;

    if (!is_ok(validate_import_alarm_rules(bundle.alarm_rules, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    payload->modbus_server_settings = bundle.modbus_server_settings;
    normalize_modbus_server_settings(&payload->modbus_server_settings);
    if (!is_ok(validate_modbus_server_settings(
            payload->modbus_server_settings, "modbus_server_settings", error_message)) ||
        !is_ok(validate_modbus_register_mappings(bundle.modbus_register_mappings, error_message))) {
        return StatusCode::kInvalidArgument;
    }
    payload->modbus_register_mappings = bundle.modbus_register_mappings;

    // 所有组成部分校验通过后返回规范化载荷。
    return StatusCode::kOk;
}

}  // namespace

// 导出系统配置。
StatusCode BackendService::export_system_config(
    ConfigExportBundle* bundle,
    std::string* error_message)
{
    if (bundle == nullptr) {
        if (error_message != nullptr) {
            *error_message = "缺少配置导出输出参数";
        }
        return StatusCode::kInvalidArgument;
    }

    std::shared_lock<std::shared_mutex> lock(service_mutex_);
    ConfigExportBundle result;
    result.exported_at_ms = time_utils::system_now_ms();
    result.system_settings = system_config_.settings;
    result.time_settings = system_config_.time_settings;
    result.network_settings = system_config_.network_settings;
    result.mqtt_settings = system_config_.mqtt_settings;
    result.mqtt_settings.password.clear();
    result.password_exported = false;
    result.alarm_rules_included = true;
    result.channels = system_config_.channels;
    result.masters = system_config_.master_nodes;
    result.modbus_server_settings = modbus_server_settings_;
    result.modbus_register_mappings = modbus_register_mappings_;
    std::vector<DeviceTemplateDefinition> templates;
    std::string template_error;
    const auto template_status = device_template_store_.load_templates(&templates, &template_error);
    if (!is_ok(template_status)) {
        if (error_message != nullptr) {
            *error_message = template_error.empty() ? "读取自定义设备类型失败" : template_error;
        }
        return template_status;
    }
    for (auto& device_template : templates) {
        if (!device_template.builtin) {
            device_template.write_commands.clear();
            result.custom_device_types.push_back(std::move(device_template));
        }
    }

    std::string alarm_error;
    const auto alarm_status = alarm_store_.list_rules(&result.alarm_rules, &alarm_error);
    if (!is_ok(alarm_status)) {
        if (error_message != nullptr) {
            *error_message = alarm_error.empty() ? "读取告警规则失败" : alarm_error;
        }
        return alarm_status;
    }

    *bundle = std::move(result);
    append_event(
        "info",
        "system_maintenance",
        "config_export",
        "导出配置",
        "用户导出系统配置备份包。",
        time_utils::system_now_ms());
    return StatusCode::kOk;
}

// 导入系统配置。
StatusCode BackendService::import_system_config(
    const ConfigImportRequest& request,
    ConfigImportResult* result,
    std::string* error_message)
{
    std::unique_lock<std::mutex> modbus_management_lock(modbus_management_mutex_);
    std::lock_guard<std::mutex> time_operation_lock(time_service_mutex_);
    std::shared_ptr<PollingService> polling_to_stop;
    std::unique_lock<std::shared_mutex> lock(service_mutex_);
    const auto ready_status =
        ensure_config_mutation_ready_locked("缺少配置导入结果输出参数", result, error_message);
    if (!is_ok(ready_status)) {
        return ready_status;
    }
    backend_internal::ScopedConfigApplyFlag config_apply_guard(config_apply_in_progress_);

    ValidatedImportPayload validated;
    const auto validation_status = validate_import_bundle(
        request.bundle, system_config_, device_template_store_, &validated, error_message);
    if (!is_ok(validation_status)) {
        return validation_status;
    }
    if (!is_ok(time_runtime_.validate_settings(validated.time_settings, error_message))) {
        return StatusCode::kInvalidArgument;
    }

    // 所有预校验通过后再读取完整恢复快照；此时尚未发生任何持久化写入。
    const auto previous_system_config = system_config_;
    const auto previous_modbus_settings = modbus_server_settings_;
    const auto previous_modbus_mappings = modbus_register_mappings_;
    std::vector<DeviceTemplateDefinition> previous_custom_device_types;
    {
        std::vector<DeviceTemplateDefinition> templates;
        std::string snapshot_error;
        const auto snapshot_status = device_template_store_.load_templates(&templates, &snapshot_error);
        if (!is_ok(snapshot_status)) {
            if (error_message != nullptr) {
                *error_message = snapshot_error.empty() ? "读取导入前设备类型快照失败" : snapshot_error;
            }
            return snapshot_status;
        }
        for (auto& device_template : templates) {
            if (!device_template.builtin) previous_custom_device_types.push_back(std::move(device_template));
        }
    }
    std::vector<AlarmRule> previous_alarm_rules;
    {
        std::string snapshot_error;
        const auto snapshot_status = alarm_store_.list_rules(&previous_alarm_rules, &snapshot_error);
        if (!is_ok(snapshot_status)) {
            if (error_message != nullptr) {
                *error_message = snapshot_error.empty() ? "读取导入前告警规则快照失败" : snapshot_error;
            }
            return snapshot_status;
        }
    }
    std::vector<AlarmRuntimeState> previous_alarm_runtime_states;

    // 配置包导入不得写 edge-history.db 或 edge-events.db。
    ScopedEventPersistenceSuppression suppress_events(event_persistence_suppressed_);
    std::unique_lock<std::mutex> alarm_delivery_guard(alarm_delivery_mutex_);

    MqttSettings next_mqtt = validated.mqtt_settings;
    next_mqtt.password = system_config_.mqtt_settings.password;

    auto restore_previous_persisted_config = [&]() {
        RestoreOutcome restore;
        ConfigImportPersistencePayload previous_payload;
        previous_payload.system_settings = previous_system_config.settings;
        previous_payload.time_settings = previous_system_config.time_settings;
        previous_payload.network_settings = previous_system_config.network_settings;
        previous_payload.network_settings_explicitly_configured =
            previous_system_config.network_settings_explicitly_configured;
        previous_payload.mqtt_settings = previous_system_config.mqtt_settings;
        previous_payload.custom_device_types = previous_custom_device_types;
        previous_payload.channels = previous_system_config.channels;
        previous_payload.masters = previous_system_config.master_nodes;
        previous_payload.replace_alarm_data = request.bundle.alarm_rules_included;
        previous_payload.alarm_rules = previous_alarm_rules;
        previous_payload.alarm_runtime_states = previous_alarm_runtime_states;
        previous_payload.modbus_server_settings = previous_modbus_settings;
        previous_payload.modbus_register_mappings = previous_modbus_mappings;

        std::string restore_error;
        const auto status = ConfigImportTransaction::apply(
            config_store_,
            device_template_store_,
            alarm_store_,
            previous_payload,
            &restore_error);
        restore.record(status, "原子恢复导入前持久化配置失败", restore_error);
        return restore;
    };

    const bool was_polling_running = is_polling_running_locked();
    if (was_polling_running) {
        stop_polling_for_config_apply(lock, &polling_to_stop);
    }
    // 告警状态由采集线程持续更新；必须先停采集再取快照，避免回滚覆盖停机窗口内的新状态。
    if (request.bundle.alarm_rules_included) {
        std::string snapshot_error;
        const auto snapshot_status =
            alarm_store_.list_runtime_states(&previous_alarm_runtime_states, &snapshot_error);
        if (!is_ok(snapshot_status)) {
            const auto message = snapshot_error.empty()
                                     ? std::string("读取导入前告警运行状态快照失败")
                                     : snapshot_error;
            return fail_config_apply_locked(
                snapshot_status,
                message,
                was_polling_running,
                error_message,
                true);
        }
    }

    ConfigImportPersistencePayload imported_payload;
    imported_payload.system_settings = validated.system_settings;
    imported_payload.time_settings = validated.time_settings;
    imported_payload.network_settings = validated.network_settings;
    imported_payload.network_settings_explicitly_configured = true;
    imported_payload.mqtt_settings = next_mqtt;
    imported_payload.custom_device_types = validated.custom_device_types;
    imported_payload.channels = validated.channels;
    imported_payload.masters = validated.masters;
    imported_payload.replace_alarm_data = request.bundle.alarm_rules_included;
    imported_payload.alarm_rules = request.bundle.alarm_rules;
    imported_payload.modbus_server_settings = validated.modbus_server_settings;
    imported_payload.modbus_register_mappings = validated.modbus_register_mappings;

    std::string write_error;
    lock.unlock();
    const auto write_status = ConfigImportTransaction::apply(
        config_store_, device_template_store_, alarm_store_, imported_payload, &write_error);
    lock.lock();

    if (!is_ok(write_status)) {
        std::string message = config_write_error_message("导入配置写入失败", write_error);
        message += "；整套 SQLite 事务已回滚，持久化配置未改变";
        return fail_config_apply_locked(
            write_status,
            message,
            was_polling_running,
            error_message,
            true);
    }

    std::vector<std::string> reload_errors;
    const auto reload_status = reload_config_for_apply(lock, &reload_errors);
    if (!is_ok(reload_status)) {
        const auto reload_detail = reload_errors.empty() ? "未知错误" : reload_errors.front();
        lock.unlock();
        const auto restore = restore_previous_persisted_config();
        lock.lock();
        std::vector<std::string> restore_reload_errors;
        StatusCode restore_reload_status = StatusCode::kInvalidState;
        if (restore.succeeded()) {
            restore_reload_status = reload_config_for_apply(lock, &restore_reload_errors);
        }
        std::string message = "导入配置后采集运行配置重建失败：" + reload_detail;
        if (restore.succeeded() && is_ok(restore_reload_status)) {
            message += "；已恢复导入前配置";
        } else if (!restore.succeeded()) {
            message += "；恢复导入前配置未完整成功：" + restore.details;
        } else {
            message += "；原配置持久化已恢复，新运行态未切换，将继续使用旧运行态";
            if (!restore_reload_errors.empty()) message += "：" + restore_reload_errors.front();
        }
        return fail_config_apply_locked(
            reload_status,
            message,
            was_polling_running,
            error_message,
            restore.succeeded());
    }

    const bool imported_has_collection_target = has_enabled_collection_target_locked();
    const auto apply_outcome = finish_config_apply_locked(was_polling_running);
    if (imported_has_collection_target && !apply_outcome.polling_restarted) {
        lock.unlock();
        const auto restore = restore_previous_persisted_config();
        lock.lock();
        std::vector<std::string> restore_reload_errors;
        const auto restore_reload_status = restore.succeeded()
                                               ? reload_config_for_apply(lock, &restore_reload_errors)
                                               : StatusCode::kInvalidState;
        std::string message = "导入配置后采集任务启动失败";
        if (restore.succeeded() && is_ok(restore_reload_status)) {
            message += "；已恢复导入前配置";
        } else if (!restore.succeeded()) {
            message += "；恢复导入前配置未完整成功：" + restore.details;
        } else if (!restore_reload_errors.empty()) {
            message += "；恢复原运行配置失败：" + restore_reload_errors.front();
        }
        return fail_config_apply_locked(
            StatusCode::kInvalidState,
            message,
            was_polling_running,
            error_message,
            restore.succeeded() && is_ok(restore_reload_status));
    }
    modbus_server_settings_ = validated.modbus_server_settings;
    lock.unlock();

    std::string modbus_apply_error;
    auto modbus_apply_status = reload_modbus_register_mappings_managed(&modbus_apply_error);
    std::shared_ptr<ModbusRegisterBank> bank;
    if (is_ok(modbus_apply_status)) {
        std::shared_lock<std::shared_mutex> read_lock(service_mutex_);
        bank = modbus_register_bank_;
    }
    if (is_ok(modbus_apply_status)) {
        modbus_apply_status = modbus_tcp_server_.restart(
            validated.modbus_server_settings, std::move(bank), &modbus_apply_error);
    }
    if (!is_ok(modbus_apply_status)) {
        std::shared_ptr<PollingService> imported_polling_to_stop;
        {
            std::unique_lock<std::shared_mutex> write_lock(service_mutex_);
            if (is_polling_running_locked()) {
                stop_polling_for_config_apply(write_lock, &imported_polling_to_stop);
            }
        }

        auto restore = restore_previous_persisted_config();
        std::vector<std::string> restore_reload_errors;
        StatusCode restore_reload_status = StatusCode::kInvalidState;
        {
            std::unique_lock<std::shared_mutex> write_lock(service_mutex_);
            if (restore.succeeded()) {
                restore_reload_status = reload_config_for_apply(write_lock, &restore_reload_errors);
            }
            modbus_server_settings_ = previous_modbus_settings;
        }

        std::string restore_error;
        if (restore.succeeded()) {
            const auto bank_restore_status = reload_modbus_register_mappings_managed(&restore_error);
            restore.record(bank_restore_status, "恢复原 Modbus Register Bank 失败", restore_error);
            if (is_ok(bank_restore_status)) {
                std::shared_lock<std::shared_mutex> read_lock(service_mutex_);
                bank = modbus_register_bank_;
            }
            if (is_ok(bank_restore_status)) {
                restore_error.clear();
                const auto server_restore_status = modbus_tcp_server_.restart(
                    previous_modbus_settings, std::move(bank), &restore_error);
                restore.record(server_restore_status, "恢复原 Modbus TCP Server 运行状态失败", restore_error);
            }
        }
        if (!restore.succeeded()) {
            modbus_tcp_server_.stop();
        }

        std::string message = "配置导入的 Modbus 运行态应用失败：" + modbus_apply_error;
        if (restore.succeeded() && is_ok(restore_reload_status)) {
            message += "；已恢复导入前完整配置";
        } else if (!restore.succeeded()) {
            message += "；恢复导入前配置未完整成功：" + restore.details;
        } else if (!restore_reload_errors.empty()) {
            message += "；恢复原采集运行配置失败：" + restore_reload_errors.front();
        }
        std::unique_lock<std::shared_mutex> write_lock(service_mutex_);
        return fail_config_apply_locked(
            modbus_apply_status,
            message,
            was_polling_running,
            error_message,
            restore.succeeded() && is_ok(restore_reload_status));
    }
    result->imported = true;
    result->polling_restarted = apply_outcome.polling_restarted;
    result->message = "配置导入成功，自定义设备类型与采集运行配置已刷新；Modbus Server 与 Register Bank 已即时应用。网络和时间设置将在设备重启或通过对应设置页手动应用，请重新确认目标设备网络；MQTT 密码和 TLS 证书文件内容不会随配置文件导入，请人工补齐凭据与证书。";
    event_persistence_suppressed_.store(false);
    append_event(
        "info",
        "system_maintenance",
        "config_import",
        "导入配置",
        "用户导入 version 4 系统配置包，自定义设备类型与采集任务已按依赖顺序恢复；网络、时间和 MQTT 敏感字段按导入策略处理。",
        time_utils::system_now_ms());
    return StatusCode::kOk;
}

}  // namespace edge_controller
