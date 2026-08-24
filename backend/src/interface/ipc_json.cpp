// IPC JSON 编解码集中层：保持当前模型字段一致、数值范围校验和敏感字段不回显策略。
#include "interface/ipc_json.h"

#include <cmath>
#include <utility>

#include "common/enums.h"

namespace edge_controller::ipc_json {

namespace {

// 判断数值是否为有限值。
nlohmann::json finite_number(double value)
{
    // JSON 不支持 NaN/Inf；统一转成 null，避免前端 JSON.parse 或图表渲染失败。
    return std::isfinite(value) ? nlohmann::json(value) : nlohmann::json(nullptr);
}

}  // namespace

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateEnumItemDefinition& item)
{
    return nlohmann::json{
        {"value", item.value},
        {"label", item.label},
        {"sort_order", item.sort_order},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateFieldDefinition& field)
{
    return nlohmann::json{
        {"key", field.field_key},
        {"display_name", field.display_name},
        {"unit", field.unit},
        {"data_type", field.data_type},
        {"parser_id", field.parser_id},
        {"read_block_key", field.read_block_key},
        {"register_offset", field.register_offset},
        {"register_count", field.register_count},
        {"byte_order", field.byte_order.empty() ? "big_endian" : field.byte_order},
        {"word_order", field.word_order.empty() ? "high_word_first" : field.word_order},
        {"bit_index", field.bit_index},
        {"enum_items", to_json_array(field.enum_items)},
        {"scale", finite_number(field.scale)},
        {"offset", finite_number(field.value_offset)},
        {"value_offset", finite_number(field.value_offset)},
        {"precision", field.precision},
        {"summary", field.summary},
        {"history_enabled", device_template_field_history_enabled(field)},
        {"show_in_realtime", device_template_field_show_in_realtime(field)},
        {"realtime_group_id", field.realtime_group_id},
        {"display_order", field.display_order},
        {"invalid_rule_type", field.invalid_rule_type},
        {"invalid_rule_value", finite_number(field.invalid_rule_value)},
        {"invalid_rule_min", finite_number(field.invalid_rule_min)},
        {"invalid_rule_max", finite_number(field.invalid_rule_max)},
    };
}

// 将设备模板读取区块转换为 JSON。
nlohmann::json to_json(const DeviceTemplateReadBlockDefinition& read_block)
{
    return nlohmann::json{
        {"block_key", read_block.block_key},
        {"display_name", read_block.display_name},
        {"function_code", read_block.function_code},
        {"start_offset", read_block.start_offset},
        {"register_count", read_block.register_count},
        {"sort_order", read_block.sort_order},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const DeviceTemplateRealtimeGroupDefinition& group)
{
    return nlohmann::json{
        {"id", group.group_id},
        {"name", group.display_name},
        {"order", group.sort_order},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateWriteCommandOption& option)
{
    return nlohmann::json{
        {"label", option.label},
        {"value", option.value},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateWriteCommandField& field)
{
    return nlohmann::json{
        {"key", field.key},
        {"label", field.label},
        {"type", field.type},
        {"min", field.min},
        {"max", field.max},
        {"unit", field.unit},
        {"has_default_value", field.has_default_value},
        {"default_value", field.default_value},
        {"options", to_json_array(field.options)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateWriteCommandDefinition& command)
{
    return nlohmann::json{
        {"key", command.key},
        {"name", command.name},
        {"description", command.description},
        {"group", command.group},
        {"function_code", command.function_code},
        {"register_offset", command.register_offset},
        {"register_count", command.register_count},
        {"has_absolute_register", command.has_absolute_register},
        {"absolute_register", command.absolute_register},
        {"fixed_values", command.fixed_values},
        {"value_fields", to_json_array(command.value_fields)},
        {"warnings", command.warnings},
        {"require_confirm", command.require_confirm},
        {"confirm_text", command.confirm_text},
        {"success_hint", command.success_hint},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateDefinition& device_template)
{
    return nlohmann::json{
        {"id", device_template.template_id},
        {"display_name", device_template.display_name},
        {"description", device_template.description},
        {"default_start_register", device_template.default_start_register},
        {"device_address_stride", device_template.device_address_stride},
        {"read_blocks", to_json_array(device_template.read_blocks)},
        {"realtime_grouping_enabled", device_template.realtime_grouping_enabled},
        {"realtime_groups", to_json_array(device_template.realtime_groups)},
        {"builtin", device_template.builtin},
        {"fields", to_json_array(device_template.fields)},
        {"write_commands", to_json_array(device_template.write_commands)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ConfigSummary& summary)
{
    return nlohmann::json{
        {"project_name", summary.project_name},
        {"project_version", summary.project_version},
        {"site_id", summary.site_id},
        {"default_poll_interval_ms", summary.default_poll_interval_ms},
        {"channel_count", static_cast<std::uint64_t>(summary.channel_count)},
        {"master_count", static_cast<std::uint64_t>(summary.master_count)},
        {"device_count", static_cast<std::uint64_t>(summary.device_count)},
        {"device_templates", to_json_array(summary.device_templates)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ReferencedMasterSummary& master)
{
    return nlohmann::json{
        {"master_id", master.master_id},
        {"master_name", master.master_name},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateManagementItem& item)
{
    return nlohmann::json{
        {"template_id", item.template_id},
        {"template_name", item.template_name},
        {"description", item.description},
        {"default_start_register", item.default_start_register},
        {"device_address_stride", item.device_address_stride},
        {"read_blocks", to_json_array(item.read_blocks)},
        {"field_count", static_cast<std::uint64_t>(item.field_count)},
        {"builtin", item.builtin},
        {"editable", item.editable},
        {"deletable", item.deletable},
        {"readonly_reason", item.readonly_reason},
        {"referenced", item.referenced},
        {"reference_count", static_cast<std::uint64_t>(item.reference_count)},
        {"referenced_masters", to_json_array(item.referenced_masters)},
        {"fields", to_json_array(item.fields)},
        {"write_commands", to_json_array(item.write_commands)},
        {"realtime_grouping_enabled", item.realtime_grouping_enabled},
        {"realtime_groups", to_json_array(item.realtime_groups)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceTemplateManagementView& view)
{
    return nlohmann::json{
        {"total_templates", static_cast<std::uint64_t>(view.total_templates)},
        {"referenced_templates", static_cast<std::uint64_t>(view.referenced_templates)},
        {"unreferenced_templates", static_cast<std::uint64_t>(view.unreferenced_templates)},
        {"templates", to_json_array(view.templates)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemSettings& settings)
{
    return nlohmann::json{
        {"device_name", settings.device_name},
        {"site_location", settings.site_location},
        {"display_name", settings.display_name},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemSettingsUpdateResult& result)
{
    return nlohmann::json{
        {"settings", to_json(result.settings)},
        {"message", result.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeSettings& settings)
{
    return nlohmann::json{
        {"timezone", settings.timezone},
        {"ntp_enabled", settings.ntp_enabled},
        {"ntp_primary", settings.ntp_primary},
        {"ntp_secondary", settings.ntp_secondary},
        {"updated_at", settings.updated_at},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeApplyResult& result)
{
    return nlohmann::json{{"settings", to_json(result.settings)}, {"message", result.message}, {"warning_message", result.warning_message}};
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeSyncResult& result)
{
    return nlohmann::json{
        {"synchronized", result.synchronized}, {"sync_time_ms", result.sync_time_ms},
        {"rtc_written", result.rtc_written}, {"message", result.message}, {"warning_message", result.warning_message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ManualTimeSetResult& result)
{
    return nlohmann::json{
        {"time_set", result.time_set}, {"previous_time_ms", result.previous_time_ms},
        {"current_time_ms", result.current_time_ms}, {"rtc_written", result.rtc_written},
        {"message", result.message}, {"warning_message", result.warning_message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const TimeRuntimeStatus& status)
{
    return nlohmann::json{
        {"current_time_ms", status.current_time_ms}, {"current_time_text", status.current_time_text},
        {"timezone", status.timezone}, {"utc_offset_text", status.utc_offset_text},
        {"ntp_enabled", status.ntp_enabled}, {"ntp_process_running", status.ntp_process_running},
        {"ntp_process_state", status.ntp_process_state},
        {"ntp_process_state_text", status.ntp_process_state_text},
        {"sync_state", status.sync_state}, {"sync_state_text", status.sync_state_text},
        {"rtc_available", status.rtc_available}, {"rtc_time_text", status.rtc_time_text},
        {"last_sync_time_ms", status.last_sync_time_ms}, {"last_error_message", status.last_error_message},
        {"settings_pending_apply", status.settings_pending_apply},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkSettings& settings)
{
    return nlohmann::json{
        {"mode", settings.mode},
        {"interface_name", settings.interface_name},
        {"ip_address", settings.ip_address},
        {"netmask", settings.netmask},
        {"gateway", settings.gateway},
        {"dns_servers", settings.dns_servers},
        {"apply_mode_text", settings.apply_mode_text},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkSettingsUpdateResult& result)
{
    return nlohmann::json{
        {"settings", to_json(result.settings)},
        {"message", result.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkApplyResult& result)
{
    return nlohmann::json{
        {"applied", result.applied},
        {"mode", result.mode},
        {"interface_name", result.interface_name},
        {"ip_address", result.ip_address},
        {"current_ip_address", result.current_ip_address},
        {"error_message", result.error_message},
        {"runtime_status", to_json(result.runtime_status)},
        {"message", result.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const NetworkRuntimeStatus& status)
{
    return nlohmann::json{
        {"configured_mode", status.configured_mode},
        {"interface_name", status.interface_name},
        {"interface_exists", status.interface_exists},
        {"operstate", status.operstate},
        {"link_state", status.link_state},
        {"link_state_text", status.link_state_text},
        {"ip_address", status.ip_address},
        {"default_gateway", status.default_gateway},
        {"dhcp_client_running", status.dhcp_client_running},
        {"message", status.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MqttSettings& settings)
{
    return nlohmann::json{
        {"enabled", settings.enabled},
        {"broker_host", settings.broker_host},
        {"broker_port", settings.broker_port},
        {"client_id", settings.client_id},
        {"node_id", settings.node_id},
        {"username", settings.username},
        {"topic_prefix", settings.topic_prefix},
        {"publish_interval_seconds", settings.publish_interval_seconds},
        {"qos", settings.qos},
        {"retain_status", settings.retain_status},
        {"keep_alive_seconds", settings.keep_alive_seconds},
        {"tls_enabled", settings.tls_enabled},
        {"tls_ca_file", settings.tls_ca_file},
        {"tls_client_cert_file", settings.tls_client_cert_file},
        {"tls_client_key_file", settings.tls_client_key_file},
        {"tls_insecure", settings.tls_insecure},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MqttSettingsUpdateResult& result)
{
    return nlohmann::json{
        {"settings", to_json(result.settings)},
        {"message", result.message},
        {"applied", result.applied},
        {"apply_error", result.apply_error},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MqttRuntimeStatus& status)
{
    return nlohmann::json{
        {"enabled", status.enabled},
        {"connected", status.connected},
        {"state", status.state},
        {"broker_endpoint", status.broker_endpoint},
        {"tls_enabled", status.tls_enabled},
        {"tls_insecure", status.tls_insecure},
        {"status_topic", status.status_topic},
        {"realtime_topic", status.realtime_topic},
        {"event_topic", status.event_topic},
        {"alarm_topic", status.alarm_topic},
        {"last_error_message", status.last_error_message},
        {"last_connect_time_ms", status.last_connect_time_ms},
        {"last_disconnect_time_ms", status.last_disconnect_time_ms},
        {"last_publish_time_ms", status.last_publish_time_ms},
        {"last_publish_topic", status.last_publish_topic},
        {"last_publish_payload_bytes", status.last_publish_payload_bytes},
        {"last_publish_error_message", status.last_publish_error_message},
        {"last_publish_sequence", status.last_publish_sequence},
        {"last_event_publish_time_ms", status.last_event_publish_time_ms},
        {"last_alarm_publish_time_ms", status.last_alarm_publish_time_ms},
        {"last_event_publish_topic", status.last_event_publish_topic},
        {"last_alarm_publish_topic", status.last_alarm_publish_topic},
        {"event_publish_count", status.event_publish_count},
        {"alarm_publish_count", status.alarm_publish_count},
        {"published_message_count", status.published_message_count},
        {"failed_publish_count", status.failed_publish_count},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const ModbusServerSettings& settings)
{
    return nlohmann::json{
        {"enabled", settings.enabled},
        {"listen_address", settings.listen_address},
        {"listen_port", settings.listen_port},
        {"unit_id", settings.unit_id},
        {"strict_unit_id", settings.strict_unit_id},
        {"max_clients", settings.max_clients},
        {"idle_timeout_seconds", settings.idle_timeout_seconds},
        {"max_read_registers", settings.max_read_registers},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const ModbusServerRuntimeStatus& status)
{
    return nlohmann::json{
        {"configured_enabled", status.configured_enabled}, {"running", status.running},
        {"listening", status.listening}, {"state", status.state},
        {"listen_address", status.listen_address}, {"listen_port", status.listen_port},
        {"unit_id", status.unit_id}, {"current_connections", status.current_connections},
        {"total_connections", status.total_connections}, {"total_requests", status.total_requests},
        {"successful_requests", status.successful_requests}, {"exception_responses", status.exception_responses},
        {"unsupported_function_count", status.unsupported_function_count},
        {"invalid_address_count", status.invalid_address_count}, {"invalid_value_count", status.invalid_value_count},
        {"malformed_request_count", status.malformed_request_count},
        {"rejected_connection_count", status.rejected_connection_count},
        {"started_at_ms", status.started_at_ms}, {"last_request_time_ms", status.last_request_time_ms},
        {"last_client_ip", status.last_client_ip}, {"last_error_message", status.last_error_message},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const ModbusRegisterMapping& mapping)
{
    return nlohmann::json{
        {"mapping_id", mapping.mapping_id}, {"device_id", mapping.device_id}, {"point_key", mapping.point_key},
        {"device_name_snapshot", mapping.device_name_snapshot}, {"point_name_snapshot", mapping.point_name_snapshot},
        {"start_address", mapping.start_address}, {"data_type", to_string(mapping.data_type)},
        {"value_multiplier", finite_number(mapping.value_multiplier)}, {"value_offset", finite_number(mapping.value_offset)},
        {"byte_order", to_string(mapping.byte_order)}, {"word_order", to_string(mapping.word_order)},
        {"quality_address", mapping.quality_address}, {"enabled", mapping.enabled},
        {"created_at_ms", mapping.created_at_ms}, {"updated_at_ms", mapping.updated_at_ms},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const ModbusExportablePoint& point)
{
    return nlohmann::json{
        {"device_id", point.device_id}, {"device_name", point.device_name},
        {"device_type_id", point.device_type_id}, {"device_type_name", point.device_type_name},
        {"point_key", point.point_key}, {"point_name", point.point_name},
        {"unit", point.unit}, {"summary", point.summary},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const ModbusServerPageSnapshot& snapshot)
{
    return nlohmann::json{
        {"settings", to_json(snapshot.settings)},
        {"runtime_status", to_json(snapshot.runtime_status)},
        {"mappings", to_json_array(snapshot.mappings)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const PollingCycleSummary& summary)
{
    return nlohmann::json{
        {"polling_running", summary.polling_running},
        {"polling_state", summary.polling_state},
        {"last_cycle_started_at_ms", summary.last_cycle_started_at_ms},
        {"last_cycle_finished_at_ms", summary.last_cycle_finished_at_ms},
        {"last_cycle_master_count", static_cast<std::uint64_t>(summary.last_cycle_master_count)},
        {"last_cycle_success_master_count", static_cast<std::uint64_t>(summary.last_cycle_success_master_count)},
        {"last_cycle_failed_master_count", static_cast<std::uint64_t>(summary.last_cycle_failed_master_count)},
        {"last_cycle_success_device_count", static_cast<std::uint64_t>(summary.last_cycle_success_device_count)},
        {"last_cycle_failed_device_count", static_cast<std::uint64_t>(summary.last_cycle_failed_device_count)},
        {"last_cycle_has_error", summary.last_cycle_has_error},
        {"diagnosis", to_json(summary.diagnosis)},
        {"last_cycle_error_message", summary.last_cycle_error_message},
        {"last_heartbeat_ms", summary.last_heartbeat_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DiagnosisStatus& status)
{
    return nlohmann::json{
        {"level", status.level},
        {"target_id", status.target_id},
        {"target_name", status.target_name},
        {"status", status.status},
        {"error_code", status.error_code},
        {"message", status.message},
        {"suggestion", status.suggestion},
        {"last_success_time_ms", status.last_success_time_ms},
        {"last_error_time_ms", status.last_error_time_ms},
        {"consecutive_failures", status.consecutive_failures},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ServiceErrorSummary& summary)
{
    return nlohmann::json{
        {"has_error", summary.has_error},
        {"source", summary.source},
        {"target_id", summary.target_id},
        {"diagnosis", to_json(summary.diagnosis)},
        {"message", summary.message},
        {"timestamp_ms", summary.timestamp_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemHealthSummary& summary)
{
    return nlohmann::json{
        {"level", summary.level},
        {"message", summary.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemProcessMetrics& metrics)
{
    return nlohmann::json{
        {"available", metrics.available},
        {"error_message", metrics.error_message},
        {"sampled_at_ms", metrics.sampled_at_ms},
        {"backend_uptime_seconds", metrics.backend_uptime_seconds},
        {"vm_rss_kb", metrics.vm_rss_kb},
        {"vm_size_kb", metrics.vm_size_kb},
        {"thread_count", metrics.thread_count},
        {"open_fd_count", metrics.open_fd_count},
        {"load_average_available", metrics.load_average_available},
        {"load_average_1m", finite_number(metrics.load_average_1m)},
        {"load_average_5m", finite_number(metrics.load_average_5m)},
        {"load_average_15m", finite_number(metrics.load_average_15m)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DatabaseStorageMetrics& metrics)
{
    return nlohmann::json{
        {"role", metrics.role},
        {"path", metrics.path},
        {"size_bytes", metrics.size_bytes},
        {"wal_size_bytes", metrics.wal_size_bytes},
        {"shm_size_bytes", metrics.shm_size_bytes},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemStorageMetrics& metrics)
{
    return nlohmann::json{
        {"available", metrics.available},
        {"error_message", metrics.error_message},
        {"storage_path", metrics.storage_path},
        {"data_directory_size_bytes", metrics.data_directory_size_bytes},
        {"sqlite_database_size_bytes", metrics.sqlite_database_size_bytes},
        {"sqlite_auxiliary_size_bytes", metrics.sqlite_auxiliary_size_bytes},
        {"databases", to_json_array(metrics.databases)},
        {"storage_total_bytes", metrics.storage_total_bytes},
        {"storage_free_bytes", metrics.storage_free_bytes},
        {"storage_available_bytes", metrics.storage_available_bytes},
        {"storage_used_percent", finite_number(metrics.storage_used_percent)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemOverviewSnapshot& snapshot)
{
    return nlohmann::json{
        {"generated_at_ms", snapshot.generated_at_ms},
        {"health", to_json(snapshot.health)},
        {"process", to_json(snapshot.process)},
        {"storage", to_json(snapshot.storage)},
        {"system_status", to_json(snapshot.system_status)},
        {"polling", to_json(snapshot.polling)},
        {"current_error", to_json(snapshot.current_error)},
        {"mqtt_runtime", to_json(snapshot.mqtt_runtime)},
        {"modbus_server_runtime", to_json(snapshot.modbus_server_runtime)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ConfigExportBundle& bundle)
{
    auto mqtt_json = to_json(bundle.mqtt_settings);
    mqtt_json["password_exported"] = bundle.password_exported;
    return nlohmann::json{
        {"format", bundle.format},
        {"version", bundle.version},
        {"exported_at_ms", bundle.exported_at_ms},
        {"system_settings", to_json(bundle.system_settings)},
        {"time_settings", to_json(bundle.time_settings)},
        {"network_settings", to_json(bundle.network_settings)},
        {"mqtt_settings", mqtt_json},
        {"custom_device_types", to_json_array(bundle.custom_device_types)},
        {"channels", to_json_array(bundle.channels)},
        {"masters", to_json_array(bundle.masters)},
        {"alarm_rules_included", bundle.alarm_rules_included},
        {"alarm_rules", to_json_array(bundle.alarm_rules)},
        {"modbus_server_settings", to_json(bundle.modbus_server_settings)},
        {"modbus_register_mappings", to_json_array(bundle.modbus_register_mappings)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ConfigImportResult& result)
{
    return nlohmann::json{
        {"imported", result.imported},
        {"message", result.message},
        {"polling_restarted", result.polling_restarted},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ServiceEvent& event)
{
    return nlohmann::json{
        {"event_id", event.event_id},
        {"first_timestamp_ms", event.first_timestamp_ms},
        {"timestamp_ms", event.timestamp_ms},
        {"level", event.level},
        {"source", event.source},
        {"target_id", event.target_id},
        {"diagnosis", to_json(event.diagnosis)},
        {"summary", event.summary},
        {"detail", event.detail},
        {"occurrence_count", event.occurrence_count},
    };
}

// 将历史事件级别统计转换为 JSON。
nlohmann::json to_json(const EventLevelStats& stats)
{
    return nlohmann::json{
        {"error", stats.error},
        {"warning", stats.warning},
        {"info", stats.info},
    };
}

// 将历史事件来源统计转换为 JSON。
nlohmann::json to_json(const EventSourceStat& stat)
{
    return nlohmann::json{
        {"source", stat.source},
        {"count", stat.count},
    };
}

// 将历史事件分页结果转换为 JSON。
nlohmann::json to_json(const EventHistoryResult& result)
{
    return nlohmann::json{
        {"rows", to_json_array(result.rows)},
        {"total", result.total},
        {"level_stats", to_json(result.level_stats)},
        {"source_stats", to_json_array(result.source_stats)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const OverviewPageSnapshot& snapshot)
{
    return nlohmann::json{
        {"system_settings", to_json(snapshot.system_settings)},
        {"config_summary", to_json(snapshot.config_summary)},
        {"system_overview_snapshot", to_json(snapshot.system_overview_snapshot)},
        {"recent_events", to_json_array(snapshot.recent_events)},
        {"active_alarms", to_json_array(snapshot.active_alarms)},
        {"channels", to_json_array(snapshot.channels)},
        {"masters", to_json_array(snapshot.masters)},
        {"devices", to_json_array(snapshot.devices)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const AlarmRule& rule)
{
    return nlohmann::json{
        {"device_id", rule.device_id}, {"point_key", rule.point_key}, {"enabled", rule.enabled},
        {"high_enabled", rule.high_enabled}, {"high_threshold", finite_number(rule.high_threshold)},
        {"low_enabled", rule.low_enabled}, {"low_threshold", finite_number(rule.low_threshold)},
        {"level", rule.level}, {"hysteresis", finite_number(rule.hysteresis)},
        {"trigger_count", rule.trigger_count}, {"recovery_count", rule.recovery_count},
        {"updated_at_ms", rule.updated_at_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const AlarmRuntimeState& state)
{
    return nlohmann::json{
        {"device_id", state.device_id}, {"point_key", state.point_key}, {"state", state.state},
        {"direction", state.direction}, {"current_value", finite_number(state.current_value)},
        {"threshold_value", finite_number(state.threshold_value)},
        {"consecutive_trigger_count", state.consecutive_trigger_count},
        {"consecutive_recovery_count", state.consecutive_recovery_count},
        {"active_since_ms", state.active_since_ms}, {"last_evaluated_at_ms", state.last_evaluated_at_ms},
        {"acknowledged", state.acknowledged}, {"acknowledged_at_ms", state.acknowledged_at_ms},
        {"acknowledged_by", state.acknowledged_by},
        {"updated_at_ms", state.updated_at_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ActiveAlarmView& alarm)
{
    return nlohmann::json{
        {"device_id", alarm.device_id}, {"device_name", alarm.device_name}, {"master_id", alarm.master_id},
        {"template_id", alarm.template_id}, {"point_key", alarm.point_key}, {"point_name", alarm.point_name},
        {"unit", alarm.unit}, {"precision", alarm.precision}, {"direction", alarm.direction},
        {"level", alarm.level}, {"current_value", finite_number(alarm.current_value)},
        {"threshold_value", finite_number(alarm.threshold_value)}, {"active_since_ms", alarm.active_since_ms},
        {"last_evaluated_at_ms", alarm.last_evaluated_at_ms}, {"acknowledged", alarm.acknowledged},
        {"acknowledged_at_ms", alarm.acknowledged_at_ms}, {"acknowledged_by", alarm.acknowledged_by},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const FactoryResetResult& result)
{
    return nlohmann::json{
        {"reset_completed", result.reset_completed},
        {"message", result.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const WebAuthStatus& status)
{
    return nlohmann::json{
        {"username", status.username},
        {"password_change_recommended", status.password_change_recommended},
        {"auth_initialized", status.auth_initialized},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const WebLoginResult& result)
{
    return nlohmann::json{
        {"success", result.success},
        {"username", result.username},
        {"role", result.role},
        {"password_change_recommended", result.password_change_recommended},
        {"message", result.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const WebPasswordChangeResult& result)
{
    return nlohmann::json{
        {"success", result.success},
        {"message", result.message},
        {"password_change_recommended", result.password_change_recommended},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const WebUserView& user)
{
    return nlohmann::json{
        {"username", user.username},
        {"display_name", user.display_name},
        {"role", user.role},
        {"enabled", user.enabled},
        {"created_at_ms", user.created_at_ms},
        {"updated_at_ms", user.updated_at_ms},
        {"last_login_at_ms", user.last_login_at_ms},
        {"password_change_recommended", user.password_change_recommended},
    };
}

// 将当前模型序列化为 JSON。
nlohmann::json to_json(const WebUserMutationResult& result)
{
    return nlohmann::json{
        {"success", result.success},
        {"message", result.message},
        {"user", to_json(result.user)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const PointValue& value)
{
    return nlohmann::json{
        {"key", value.key},
        {"name", value.name},
        {"value", finite_number(value.value)},
        {"unit", value.unit},
        {"precision", value.precision},
        {"summary", value.summary},
        {"history_enabled", value.history_enabled},
        {"display_order", value.display_order},
        {"quality", to_string(value.quality)},
        {"valid", value.valid},
        {"raw_value", finite_number(value.raw_value)},
        {"display_text", value.display_text},
        {"message", value.message},
        {"sample_time_ms", value.sample_time_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SerialPortInfo& info)
{
    return nlohmann::json{
        {"path", info.path},
        {"name", info.name},
        {"kind", info.kind},
        {"display_name", info.display_name},
        {"available", info.available},
        {"busy", info.busy},
        {"description", info.description},
        {"symlink_by_id", info.symlink_by_id},
        {"symlink_by_path", info.symlink_by_path},
        {"driver", info.driver},
        {"physical_hint", info.physical_hint},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelConfig& config)
{
    const auto& port_name = config.port_name.empty() ? config.device_path : config.port_name;
    return nlohmann::json{
        {"channel_id", config.channel_id},
        {"channel_name", config.channel_name},
        {"enabled", config.enabled},
        {"channel_type", to_string(config.channel_type)},
        {"device_path", config.device_path},
        {"port_name", port_name},
        {"tcp_host", config.tcp_host},
        {"tcp_port", config.tcp_port},
        {"connect_timeout_ms", config.connect_timeout_ms},
        {"baud_rate", config.baud_rate},
        {"data_bits", config.data_bits},
        {"parity", to_string(config.parity)},
        {"stop_bits", config.stop_bits},
        {"response_timeout_ms", config.response_timeout_ms},
        {"retry_count", config.retry_count},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelConfigUpdateResult& result)
{
    return nlohmann::json{
        {"channel_id", result.channel_id},
        {"channel_config", to_json(result.channel_config)},
        {"message", result.message},
        {"warning_message", result.warning_message},
        {"polling_restarted", result.polling_restarted},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelConfigDeleteResult& result)
{
    return nlohmann::json{
        {"channel_id", result.channel_id},
        {"message", result.message},
        {"polling_restarted", result.polling_restarted},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeConfig& config)
{
    return nlohmann::json{
        {"master_id", config.master_id},
        {"master_name", config.master_name},
        {"enabled", config.enabled},
        {"protocol", to_string(config.protocol)},
        {"channel_id", config.channel_id},
        {"target_address", config.target_address},
        {"poll_interval_ms", config.poll_interval_ms},
        {"retry_count", config.retry_count},
        {"remark", config.remark},
        {"device_template", config.device_template},
        {"block_start_register", config.block_start_register},
        {"device_count", config.device_count},
        {"response_timeout_ms", config.response_timeout_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeConfigUpdateResult& result)
{
    return nlohmann::json{
        {"master_id", result.master_id},
        {"master_config", to_json(result.master_config)},
        {"message", result.message},
        {"warning_message", result.warning_message},
        {"polling_restarted", result.polling_restarted},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeConfigDeleteResult& result)
{
    return nlohmann::json{
        {"master_id", result.master_id},
        {"message", result.message},
        {"polling_restarted", result.polling_restarted},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceConfig& config)
{
    return nlohmann::json{
        {"device_id", config.device_id},
        {"system_name", config.generated_name},
        {"device_name", config.device_name},
        {"master_id", config.master_id},
        {"enabled", config.enabled},
        {"register_offset", config.register_offset},
    };
}

// 将批量设备名称更新结果转换为 JSON。
nlohmann::json to_json(const DeviceDisplayNameBatchResult& result)
{
    nlohmann::json failures = nlohmann::json::array();
    failures.get_ref<nlohmann::json::array_t&>().reserve(result.failures.size());
    for (const auto& failure : result.failures) {
        failures.push_back({
            {"device_id", failure.device_id},
            {"message", failure.message},
        });
    }
    return nlohmann::json{
        {"success_count", result.success_count},
        {"failure_count", result.failure_count},
        {"failures", std::move(failures)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelStatus& status)
{
    return nlohmann::json{
        {"channel_id", status.channel_id},
        {"configured", status.configured},
        {"enabled", status.enabled},
        {"opened", status.opened},
        {"status", status.status},
        {"device_path", status.device_path},
        {"last_open_time_ms", status.last_open_time_ms},
        {"last_close_time_ms", status.last_close_time_ms},
        {"last_send_time_ms", status.last_send_time_ms},
        {"last_receive_time_ms", status.last_receive_time_ms},
        {"last_change_time_ms", status.last_change_time_ms},
        {"consecutive_error_count", status.consecutive_error_count},
        {"diagnosis", to_json(status.diagnosis)},
        {"last_error_message", status.last_error_message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const MasterNodeStatus& status)
{
    return nlohmann::json{
        {"master_id", status.master_id},
        {"online", status.online},
        {"last_collect_success", status.last_collect_success},
        {"communication_quality", to_string(status.communication_quality)},
        {"last_poll_time_ms", status.last_poll_time_ms},
        {"last_success_time_ms", status.last_success_time_ms},
        {"last_failure_time_ms", status.last_failure_time_ms},
        {"last_cycle_duration_ms", status.last_cycle_duration_ms},
        {"consecutive_failure_count", status.consecutive_failure_count},
        {"consecutive_timeout_count", status.consecutive_timeout_count},
        {"last_register_block", status.last_register_block},
        {"diagnosis", to_json(status.diagnosis)},
        {"last_error_message", status.last_error_message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceStatus& status)
{
    return nlohmann::json{
        {"device_id", status.device_id},
        {"device_name", status.device_name},
        {"master_id", status.master_id},
        {"template_id", status.template_id},
        {"template_name", status.template_name},
        {"online", status.online},
        {"last_collect_success", status.last_collect_success},
        {"communication_quality", to_string(status.communication_quality)},
        {"updated_at_ms", status.updated_at_ms},
        {"last_success_time_ms", status.last_success_time_ms},
        {"last_failure_time_ms", status.last_failure_time_ms},
        {"has_resistance", status.has_resistance},
        {"resistance_value", finite_number(status.resistance_value)},
        {"points", to_json_array(status.points)},
        {"diagnosis", to_json(status.diagnosis)},
        {"last_error_message", status.last_error_message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceRealtimeSnapshot& snapshot)
{
    return nlohmann::json{
        {"device_id", snapshot.device_id},
        {"device_name", snapshot.device_name},
        {"master_id", snapshot.master_id},
        {"template_id", snapshot.template_id},
        {"template_name", snapshot.template_name},
        {"sample_time_ms", snapshot.sample_time_ms},
        {"communication_quality", to_string(snapshot.communication_quality)},
        {"points", to_json_array(snapshot.points)},
        {"has_resistance", snapshot.has_resistance},
        {"resistance", snapshot.has_resistance ? to_json(snapshot.resistance) : nlohmann::json(nullptr)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const HistoryRecord& record)
{
    return nlohmann::json{
        {"device_id", record.device_id},
        {"master_id", record.master_id},
        {"channel_id", record.channel_id},
        {"template_id", record.template_id},
        {"sample_period", record.sample_period},
        {"bucket_start_ms", record.bucket_start_ms},
        {"bucket_text", record.bucket_text},
        {"timestamp_ms", record.timestamp_ms},
        {"date", record.date},
        {"point_key", record.point_key},
        {"point_name", record.point_name},
        {"unit", record.unit},
        {"precision", record.precision},
        {"value", finite_number(record.value)},
        {"raw_value", finite_number(record.raw_value)},
        {"quality", record.quality},
        {"valid", record.valid},
        {"message", record.message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const HistoryPointSummary& point)
{
    return nlohmann::json{
        {"point_key", point.point_key},
        {"point_name", point.point_name},
        {"unit", point.unit},
        {"precision", point.precision},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const HistoryOverviewSummary& summary)
{
    return nlohmann::json{
        {"device_id", summary.device_id},
        {"master_id", summary.master_id},
        {"channel_id", summary.channel_id},
        {"point_key", summary.point_key},
        {"point_name", summary.point_name},
        {"unit", summary.unit},
        {"precision", summary.precision},
        {"sample_period", summary.sample_period},
        {"record_count", summary.record_count},
        {"latest_value", finite_number(summary.latest_value)},
        {"latest_timestamp_ms", summary.latest_timestamp_ms},
        {"latest_bucket_start_ms", summary.latest_bucket_start_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceHistoryStats& stats)
{
    return nlohmann::json{
        {"record_count", static_cast<std::uint64_t>(stats.record_count)},
        {"earliest_date", stats.earliest_date},
        {"latest_date", stats.latest_date},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceHistoryView& view)
{
    // 单设备历史页一次性返回设备、主站、通道和历史点位，减少 Go Web 再发多次 IPC 查询。
    return nlohmann::json{
        {"device", to_json(view.device)},
        {"master", to_json(view.master)},
        {"channel", to_json(view.channel)},
        {"history_records", to_json_array(view.history_records)},
        {"history_points", to_json_array(view.history_points)},
        {"stats", to_json(view.stats)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DataMaintenanceSummary& summary)
{
    return nlohmann::json{
        {"raw_10min_retention_hours", summary.raw_10min_retention_hours},
        {"hour_retention_days", summary.hour_retention_days},
        {"day_retention_days", summary.day_retention_days},
        {"event_retention_days", summary.event_retention_days},
        {"log_retention_days", summary.log_retention_days},
        {"log_rotation_policy", summary.log_rotation_policy},
        {"last_cleanup_time_ms", summary.last_cleanup_time_ms},
        {"last_cleanup_success", summary.last_cleanup_success},
        {"last_cleanup_result", summary.last_cleanup_result},
        {"deleted_raw_10min_count", summary.deleted_raw_10min_count},
        {"deleted_hour_count", summary.deleted_hour_count},
        {"deleted_day_count", summary.deleted_day_count},
        {"deleted_event_count", summary.deleted_event_count},
        {"current_raw_10min_count", summary.current_raw_10min_count},
        {"current_hour_count", summary.current_hour_count},
        {"current_day_count", summary.current_day_count},
        {"current_event_count", summary.current_event_count},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const CommunicationTraceRecord& record)
{
    return nlohmann::json{
        {"sequence", record.sequence},
        {"timestamp_ms", record.timestamp_ms},
        {"channel_id", record.channel_id},
        {"master_id", record.master_id},
        {"device_index", record.device_index},
        {"device_id", record.device_id},
        {"block_key", record.block_key},
        {"block_display_name", record.block_display_name},
        {"protocol", record.protocol},
        {"slave_address", record.slave_address},
        {"function_code", record.function_code},
        {"start_register", record.start_register},
        {"register_count", record.register_count},
        {"request_hex", record.request_hex},
        {"response_hex", record.response_hex},
        {"elapsed_ms", record.elapsed_ms},
        {"result", record.result},
        {"error_message", record.error_message},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ChannelCommunicationTraces& traces)
{
    return nlohmann::json{
        {"channel_id", traces.channel_id},
        {"records", to_json_array(traces.records)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ModbusWriteMultipleRegistersResponse& response)
{
    return nlohmann::json{
        {"success", response.success},
        {"master_id", response.master_id},
        {"master_name", response.master_name},
        {"channel_id", response.channel_id},
        {"channel_name", response.channel_name},
        {"protocol", response.protocol},
        {"slave_address", response.slave_address},
        {"function_code", response.function_code},
        {"start_register", response.start_register},
        {"register_count", response.register_count},
        {"request_hex", response.request_hex},
        {"response_hex", response.response_hex},
        {"status", response.status},
        {"diagnosis_error_code", response.diagnosis_error_code},
        {"error_message", response.error_message},
        {"timestamp_ms", response.timestamp_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const ModbusReadHoldingRegistersResponse& response)
{
    return nlohmann::json{
        {"success", response.success},
        {"master_id", response.master_id},
        {"master_name", response.master_name},
        {"channel_id", response.channel_id},
        {"channel_name", response.channel_name},
        {"protocol", response.protocol},
        {"slave_address", response.slave_address},
        {"function_code", response.function_code},
        {"start_register", response.start_register},
        {"register_count", response.register_count},
        {"values", response.values},
        {"request_hex", response.request_hex},
        {"response_hex", response.response_hex},
        {"status", response.status},
        {"diagnosis_error_code", response.diagnosis_error_code},
        {"error_message", response.error_message},
        {"timestamp_ms", response.timestamp_ms},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const DeviceCommandExecuteResponse& response)
{
    return nlohmann::json{
        {"success", response.success},
        {"device_id", response.device_id},
        {"device_name", response.device_name},
        {"template_id", response.template_id},
        {"command_key", response.command_key},
        {"command_name", response.command_name},
        {"warnings", response.warnings},
        {"success_hint", response.success_hint},
        {"write_result", to_json(response.write_result)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const EM100RecordReadResponse& response)
{
    return nlohmann::json{
        {"success", response.success},
        {"device_id", response.device_id},
        {"device_name", response.device_name},
        {"template_id", response.template_id},
        {"record_type", response.record_type},
        {"valid_record", response.valid_record},
        {"unread_count", response.unread_count},
        {"title", response.title},
        {"content", response.content},
        {"data_text", response.data_text},
        {"ratio_type", response.ratio_type},
        {"ratio_value_text", response.ratio_value_text},
        {"record_time", response.record_time},
        {"raw_registers", response.raw_registers},
        {"read_result", to_json(response.read_result)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const SystemStatus& status)
{
    return nlohmann::json{
        {"config_loaded", status.config_loaded},
        {"service_ready", status.service_ready},
        {"running", status.running},
        {"polling_running", status.polling_running},
        {"polling_state", status.polling_state},
        {"started_at_ms", status.started_at_ms},
        {"stopped_at_ms", status.stopped_at_ms},
        {"last_heartbeat_ms", status.last_heartbeat_ms},
        {"last_poll_cycle_started_at_ms", status.last_poll_cycle_started_at_ms},
        {"last_poll_cycle_finished_at_ms", status.last_poll_cycle_finished_at_ms},
        {"online_channel_count", static_cast<std::uint64_t>(status.online_channel_count)},
        {"online_master_count", static_cast<std::uint64_t>(status.online_master_count)},
        {"online_device_count", static_cast<std::uint64_t>(status.online_device_count)},
        {"last_poll_cycle_master_count", static_cast<std::uint64_t>(status.last_poll_cycle_master_count)},
        {"last_poll_cycle_success_master_count", static_cast<std::uint64_t>(status.last_poll_cycle_success_master_count)},
        {"last_poll_cycle_failed_master_count", static_cast<std::uint64_t>(status.last_poll_cycle_failed_master_count)},
        {"last_poll_cycle_success_device_count", static_cast<std::uint64_t>(status.last_poll_cycle_success_device_count)},
        {"last_poll_cycle_failed_device_count", static_cast<std::uint64_t>(status.last_poll_cycle_failed_device_count)},
        {"last_poll_cycle_has_error", status.last_poll_cycle_has_error},
        {"diagnosis", to_json(status.diagnosis)},
        {"last_status_message", status.last_status_message},
        {"last_poll_cycle_error_message", status.last_poll_cycle_error_message},
        {"channel_status_list", to_json_array(status.channel_status_list)},
        {"master_status_list", to_json_array(status.master_status_list)},
        {"device_status_list", to_json_array(status.device_status_list)},
    };
}

// 将模型对象转换为JSON。
nlohmann::json to_json(const RealtimeViewSnapshot& snapshot)
{
    // DataStore 已在同一共享锁内生成不含 points 的设备健康投影，点位只通过
    // device_realtime_snapshots 序列化一次，避免构造完整 JSON 后再清空数组。
    return nlohmann::json{
        {"device_template_generation", snapshot.device_template_generation},
        {"devices", to_json_array(snapshot.devices)},
        {"channels", to_json_array(snapshot.channels)},
        {"masters", to_json_array(snapshot.masters)},
        {"system_status", to_json(snapshot.system_status)},
        {"device_realtime_snapshots", to_json_array(snapshot.device_realtime_snapshots)},
    };
}

}  // namespace edge_controller::ipc_json
