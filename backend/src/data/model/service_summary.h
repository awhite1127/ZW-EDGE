// 服务运行摘要、最近错误、概览资源与存储指标模型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "shared/common/types.h"
#include "data/model/alarm.h"
#include "data/model/channel_config.h"
#include "data/model/diagnosis_status.h"
#include "data/model/device_config.h"
#include "data/model/device_template.h"
#include "data/model/master_node_config.h"
#include "data/model/mqtt_settings.h"
#include "data/model/modbus_server.h"
#include "data/model/network_settings.h"
#include "data/model/system_status.h"
#include "data/model/system_settings.h"
#include "data/model/time_settings.h"
#include "communication/modbus_server/modbus_server_runtime_status.h"

namespace edge_controller {

struct ConfigSummary {
    std::string project_name;
    std::string project_version;
    std::string site_id;
    std::uint32_t default_poll_interval_ms{0};
    std::size_t channel_count{0};
    std::size_t master_count{0};
    std::size_t device_count{0};
    std::vector<DeviceTemplateDefinition> device_templates;
};

struct ReferencedMasterSummary {
    std::string master_id;
    std::string master_name;
};

struct DeviceTemplateManagementItem {
    std::string template_id;
    std::string template_name;
    std::string description;
    std::uint32_t default_start_register{0};
    std::uint32_t device_address_stride{1};
    std::vector<DeviceTemplateReadBlockDefinition> read_blocks;
    std::size_t field_count{0};
    bool builtin{false};
    bool editable{false};
    bool deletable{false};
    std::string readonly_reason;
    bool referenced{false};
    std::size_t reference_count{0};
    std::vector<ReferencedMasterSummary> referenced_masters;
    std::vector<DeviceTemplateFieldDefinition> fields;
    std::vector<DeviceTemplateWriteCommandDefinition> write_commands;
    bool realtime_grouping_enabled{false};
    std::vector<DeviceTemplateRealtimeGroupDefinition> realtime_groups;
};

struct DeviceTemplateManagementView {
    std::size_t total_templates{0};
    std::size_t referenced_templates{0};
    std::size_t unreferenced_templates{0};
    std::vector<DeviceTemplateManagementItem> templates;
};

struct PollingCycleSummary {
    bool polling_running{false};
    std::string polling_state{"not_started"};
    TimestampMs last_cycle_started_at_ms{0};
    TimestampMs last_cycle_finished_at_ms{0};
    std::size_t last_cycle_master_count{0};
    std::size_t last_cycle_success_master_count{0};
    std::size_t last_cycle_failed_master_count{0};
    std::size_t last_cycle_success_device_count{0};
    std::size_t last_cycle_failed_device_count{0};
    bool last_cycle_has_error{false};
    DiagnosisStatus diagnosis;
    std::string last_cycle_error_message;
    TimestampMs last_heartbeat_ms{0};
};

struct ServiceErrorSummary {
    bool has_error{false};
    std::string source;
    std::string target_id;
    DiagnosisStatus diagnosis;
    std::string message;
    TimestampMs timestamp_ms{0};
};

struct SystemProcessMetrics {
    bool available{false};
    std::string error_message;
    TimestampMs sampled_at_ms{0};
    std::uint64_t backend_uptime_seconds{0};
    std::uint64_t vm_rss_kb{0};
    std::uint64_t vm_size_kb{0};
    std::uint32_t thread_count{0};
    std::uint32_t open_fd_count{0};
    bool load_average_available{false};
    double load_average_1m{0.0};
    double load_average_5m{0.0};
    double load_average_15m{0.0};
};

struct DatabaseStorageMetrics {
    std::string role;
    std::string path;
    std::uint64_t size_bytes{0};
    std::uint64_t wal_size_bytes{0};
    std::uint64_t shm_size_bytes{0};
};

struct SystemStorageMetrics {
    bool available{false};
    std::string error_message;
    std::string storage_path;
    std::uint64_t data_directory_size_bytes{0};
    std::uint64_t sqlite_database_size_bytes{0};
    std::uint64_t sqlite_auxiliary_size_bytes{0};
    std::vector<DatabaseStorageMetrics> databases;
    std::uint64_t storage_total_bytes{0};
    std::uint64_t storage_free_bytes{0};
    std::uint64_t storage_available_bytes{0};
    double storage_used_percent{0.0};
};

struct SystemHealthSummary {
    std::string level{"normal"};
    std::string message{"系统运行状态正常"};
};

struct SystemOverviewSnapshot {
    TimestampMs generated_at_ms{0};
    SystemHealthSummary health;
    SystemProcessMetrics process;
    SystemStorageMetrics storage;
    SystemStatus system_status;
    PollingCycleSummary polling;
    ServiceErrorSummary current_error;
    MqttRuntimeStatus mqtt_runtime;
    ModbusServerRuntimeStatus modbus_server_runtime;
};

struct ModbusServerPageSnapshot {
    ModbusServerSettings settings;
    ModbusServerRuntimeStatus runtime_status;
    std::vector<ModbusRegisterMapping> mappings;
};

struct ConfigExportBundle {
    std::string format{"edge-controller-config"};
    std::uint32_t version{4};
    TimestampMs exported_at_ms{0};
    SystemSettings system_settings;
    TimeSettings time_settings;
    NetworkSettings network_settings;
    MqttSettings mqtt_settings;
    bool password_exported{false};
    bool alarm_rules_included{true};
    // 配置包只携带用户创建的设备类型；内置类型由目标环境自身提供。
    std::vector<DeviceTemplateDefinition> custom_device_types;
    std::vector<ChannelConfig> channels;
    std::vector<MasterNodeConfig> masters;
    std::vector<AlarmRule> alarm_rules;
    ModbusServerSettings modbus_server_settings;
    std::vector<ModbusRegisterMapping> modbus_register_mappings;
};

struct ConfigImportRequest {
    ConfigExportBundle bundle;
};

struct ConfigImportResult {
    bool imported{false};
    std::string message;
    bool polling_restarted{false};
};

struct ServiceEvent {
    std::string event_id;
    TimestampMs first_timestamp_ms{0};
    TimestampMs timestamp_ms{0};
    std::string level;
    std::string source;
    std::string target_id;
    DiagnosisStatus diagnosis;
    std::string summary;
    std::string detail;
    std::uint64_t occurrence_count{1};
};

// 系统概览 HTML 首屏所需的轻量聚合数据；各字段仍来自原有服务和存储。
struct OverviewPageSnapshot {
    SystemSettings system_settings;
    ConfigSummary config_summary;
    SystemOverviewSnapshot system_overview_snapshot;
    std::vector<ServiceEvent> recent_events;
    std::vector<ActiveAlarmView> active_alarms;
    std::vector<ChannelConfig> channels;
    std::vector<MasterNodeConfig> masters;
    std::vector<DeviceConfig> devices;
};

struct FactoryResetResult {
    bool reset_completed{false};
    std::string message;
};

}  // namespace edge_controller
