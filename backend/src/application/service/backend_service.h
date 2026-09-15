// 声明后端聚合服务的公开能力、组件所有权和生命周期状态。
// 边界：协调跨组件状态；配置切换和耗时 I/O 必须遵守既有锁边界。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "shared/common/status_code.h"
#include "data/datastore/alarm_store.h"
#include "data/datastore/communication_store.h"
#include "data/datastore/config_store.h"
#include "data/datastore/runtime_state_store.h"
#include "data/datastore/device_template_store.h"
#include "data/datastore/event_store.h"
#include "data/datastore/history_store.h"
#include "application/manager/channel_manager.h"
#include "application/manager/topology_manager.h"
#include "data/model/channel_update.h"
#include "data/model/device_history_view.h"
#include "data/model/data_maintenance.h"
#include "data/model/device_realtime.h"
#include "data/model/master_node_update.h"
#include "data/model/modbus_server.h"
#include "data/model/modbus_write_request.h"
#include "data/model/mqtt_settings.h"
#include "data/model/network_settings.h"
#include "data/model/realtime_view_snapshot.h"
#include "data/model/serial_port_info.h"
#include "data/model/service_summary.h"
#include "data/model/system_config.h"
#include "data/model/system_settings.h"
#include "data/model/system_status.h"
#include "data/model/time_settings.h"
#include "data/model/time_change.h"
#include "application/service/time_jump_detector.h"
#include "data/model/web_auth.h"
#include "communication/mqtt/mqtt_publisher_service.h"
#include "communication/modbus_server/modbus_export_service.h"
#include "communication/modbus_server/modbus_register_bank.h"
#include "communication/modbus_server/modbus_server_runtime_status.h"
#include "communication/modbus_server/modbus_tcp_server.h"
#include "application/service/polling_runtime.h"
#include "application/service/runtime_config_compiler.h"
#include "application/service/update_service.h"
#include "application/service/alarm_evaluator.h"
#include "application/service/serial_port_enumerator.h"
#include "infrastructure/platform/linux_time_runtime.h"

namespace edge_controller {

// BackendService 是 backend 侧统一服务入口。
// Web / IPC 通过这一层访问配置、状态、轮询、实时数据和历史事件。
class BackendService {
    friend struct BackendChannelConfigTestAccess;
public:

    ~BackendService();

    BackendService() = default;

    BackendService(const BackendService&) = delete;

    BackendService& operator=(const BackendService&) = delete;

    // 从唯一 SQLite 数据目录初始化后端服务、存储和运行态。
    StatusCode initialize(const std::string& data_directory = {});
    // 重新加载配置并刷新通道、拓扑和运行状态。
    StatusCode reload_config(std::vector<std::string>* errors = nullptr);

    // 启动采集轮询服务。
    StatusCode start_polling();
    // 停止采集轮询服务。
    void stop_polling();
    // 关闭后端服务并释放采集、通道和存储资源。
    void shutdown();
    // 判断采集轮询是否正在运行。
    bool is_polling_running() const;

    // 获取当前配置摘要，供页面展示配置总览。
    ConfigSummary get_config_summary() const;
    // 获取最近一次配置重载产生的提示信息。
    std::vector<std::string> get_config_reload_notes() const;
    // 获取系统基础显示设置。
    SystemSettings get_system_settings() const;
    // 查询当前安装版本，返回升级引擎的 JSON 对象。
    StatusCode get_update_current_version(std::string* result_json, std::string* error_message = nullptr) const;
    // 查询当前或最近一次持久升级状态。
    StatusCode get_update_status(std::string* result_json, std::string* error_message = nullptr) const;
    // 校验 incoming 中的指定安全包名并创建 READY 任务。
    StatusCode validate_update_package(
        const std::string& package_identifier,
        std::string* result_json,
        std::string* error_message = nullptr) const;
    // 安全接管 edge-web 上传暂存区中的指定文件，并复用正式升级校验创建 READY 任务。
    StatusCode import_application_upgrade_package(
        const std::string& upload_id,
        const std::string& package_identifier,
        std::string* result_json,
        std::string* error_message = nullptr) const;
    // 请求 systemd 独立 root 任务接管指定 READY 升级任务。
    StatusCode start_update_job(
        const std::string& job_id,
        std::string* result_json,
        std::string* error_message = nullptr) const;
    // 获取时间设置。
    TimeSettings get_time_settings() const;
    // 获取当前时间运行状态。
    TimeRuntimeStatus get_time_runtime_status() const;
    // 保存并应用时间设置。
    StatusCode save_and_apply_time_settings(
        const TimeSettingsUpdateRequest& request,
        TimeApplyResult* result,
        std::string* error_message = nullptr);
    // 立即执行一次时间同步。
    StatusCode sync_time_now(TimeSyncResult* result, std::string* error_message = nullptr);
    // 手动设置系统时间。
    StatusCode set_manual_system_time(
        const ManualTimeSetRequest& request,
        ManualTimeSetResult* result,
        std::string* error_message = nullptr);
    // 启动时恢复并应用时间设置。
    StatusCode restore_time_settings_on_startup(std::string* error_message = nullptr);
    // 显式校时和运行时监测共用的时间调整协调入口。
    void handle_system_time_changed(
        TimeAdjustmentInfo adjustment,
        const std::string& warning_message = {});
    // 获取网络配置。
    NetworkSettings get_network_settings() const;
    // 获取当前网络运行状态。
    NetworkRuntimeStatus get_network_runtime_status() const;
    // 获取 MQTT 北向配置。
    MqttSettings get_mqtt_settings() const;
    // 更新 MQTT 北向配置并应用 MQTT 发布配置。
    StatusCode update_mqtt_settings(
        const MqttSettingsUpdateRequest& request,
        MqttSettingsUpdateResult* result,
        std::string* error_message = nullptr);
    // 获取 MQTT 北向运行状态。
    MqttRuntimeStatus get_mqtt_runtime_status() const;
    // 保存并应用 Modbus TCP Server 设置；监听重启在 BackendService 全局锁外执行。
    StatusCode apply_modbus_server_settings(
        const ModbusServerSettings& settings,
        std::string* error_message = nullptr);
    // 从 ConfigStore 重载映射，完成新 Bank 回填后无中间态切换。
    StatusCode reload_modbus_register_mappings(std::string* error_message = nullptr);
    // 获取纯内存 Modbus Server 运行状态快照。
    ModbusServerRuntimeStatus get_modbus_server_runtime_status() const;
    // 获取 Modbus 服务页面聚合快照。
    StatusCode get_modbus_server_page_snapshot(
        ModbusServerPageSnapshot* snapshot,
        std::string* error_message = nullptr) const;
    // 列出可导出的设备点位。
    StatusCode list_modbus_exportable_points(
        std::vector<ModbusExportablePoint>* points,
        std::string* error_message = nullptr) const;
    // 列出 Modbus 寄存器映射。
    StatusCode list_modbus_register_mappings(
        std::vector<ModbusRegisterMapping>* mappings,
        std::string* error_message = nullptr) const;
    // 创建 Modbus 寄存器映射。
    StatusCode create_modbus_register_mapping(
        const ModbusRegisterMapping& request,
        ModbusRegisterMapping* created,
        std::string* error_message = nullptr);
    // 更新 Modbus 寄存器映射。
    StatusCode update_modbus_register_mapping(
        const std::string& mapping_id,
        const ModbusRegisterMapping& request,
        ModbusRegisterMapping* updated,
        std::string* error_message = nullptr);
    // 删除 Modbus 寄存器映射。
    StatusCode delete_modbus_register_mapping(
        const std::string& mapping_id,
        std::string* error_message = nullptr);
    // 获取系统概览运行快照。
    SystemOverviewSnapshot get_system_overview_snapshot() const;
    // 获取系统概览 HTML 首屏所需的轻量聚合数据。
    StatusCode get_overview_page_snapshot(
        OverviewPageSnapshot* snapshot,
        std::string* error_message = nullptr) const;
    // 导出当前系统配置备份包。
    StatusCode export_system_config(
        ConfigExportBundle* bundle,
        std::string* error_message = nullptr);
    // 导入系统配置备份包并刷新运行态。
    StatusCode import_system_config(
        const ConfigImportRequest& request,
        ConfigImportResult* result,
        std::string* error_message = nullptr);
    // 更新系统基础显示设置并写入配置存储。
    StatusCode update_system_settings(
        const SystemSettingsUpdateRequest& request,
        SystemSettingsUpdateResult* result,
        std::string* error_message = nullptr);
    // 获取 Web 管理员认证状态。
    StatusCode get_web_auth_status(
        WebAuthStatus* status,
        std::string* error_message = nullptr);
    // 读取首次部署管理员免密入口状态。
    StatusCode get_first_boot_admin_entry_available(
        bool* available,
        std::string* error_message = nullptr) const;
    // 消费首次部署管理员免密入口。
    StatusCode consume_first_boot_admin_entry(
        bool* consumed,
        std::string* error_message = nullptr);
    // 校验 Web 管理员登录。
    StatusCode verify_web_login(
        const WebLoginRequest& request,
        WebLoginResult* result,
        std::string* error_message = nullptr);
    // 修改当前固定 Web 账户密码。
    StatusCode change_web_password(
        const WebPasswordChangeRequest& request,
        WebPasswordChangeResult* result,
        std::string* error_message = nullptr);
    // 管理员设置固定只读账户密码。
    StatusCode set_web_viewer_password(
        const WebViewerPasswordSetRequest& request,
        WebPasswordChangeResult* result,
        std::string* error_message = nullptr);
    // 列出 Web 用户。
    StatusCode list_web_users(
        std::vector<WebUserView>* users,
        std::string* error_message = nullptr);
    // 创建 Web 用户。
    StatusCode create_web_user(
        const WebUserCreateRequest& request,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 更新 Web 用户。
    StatusCode update_web_user(
        const WebUserUpdateRequest& request,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 重置 Web 用户密码。
    StatusCode reset_web_user_password(
        const WebUserPasswordResetRequest& request,
        WebUserMutationResult* result,
        std::string* error_message = nullptr);
    // 更新网络配置并写入配置存储。
    StatusCode update_network_settings(
        const NetworkSettingsUpdateRequest& request,
        NetworkSettingsUpdateResult* result,
        std::string* error_message = nullptr);
    // 保存并应用网络配置；应用失败时恢复原持久化配置和运行模式。
    StatusCode save_and_apply_network_settings(
        const NetworkSettingsUpdateRequest& request,
        NetworkApplyResult* result,
        std::string* error_message = nullptr);
    // 将已保存的网络配置应用到系统运行环境。
    StatusCode apply_network_settings(
        NetworkApplyResult* result,
        std::string* error_message = nullptr);
    // 后端启动时尝试恢复已保存的 static/DHCP 网络模式。
    StatusCode apply_network_settings_on_startup(std::string* error_message = nullptr);

    // 获取通道配置列表。
    std::vector<ChannelConfig> get_channels() const;
    // 新增通道配置并重载运行态。
    StatusCode create_channel_config(
        const ChannelConfigUpdateRequest& request,
        ChannelConfigUpdateResult* result,
        std::string* error_message = nullptr);
    // 更新通道配置并重载运行态。
    StatusCode update_channel_config(
        const ChannelConfigUpdateRequest& request,
        ChannelConfigUpdateResult* result,
        std::string* error_message = nullptr);
    // 删除通道配置并重载运行态。
    StatusCode delete_channel_config(
        const ChannelId& channel_id,
        ChannelConfigDeleteResult* result,
        std::string* error_message = nullptr);
    // 枚举当前系统可用串口。
    StatusCode list_serial_ports(std::vector<SerialPortInfo>* ports, std::string* error_message = nullptr) const;
    // 获取主站配置列表。
    std::vector<MasterNodeConfig> get_masters() const;
    // 获取设备模板管理视图。
    DeviceTemplateManagementView get_device_template_management() const;
    // 新增设备模板。
    StatusCode create_device_template(
        const DeviceTemplateDefinition& device_template,
        DeviceTemplateManagementView* result,
        std::string* error_message = nullptr);
    // 更新设备模板。
    StatusCode update_device_template(
        const DeviceTemplateDefinition& device_template,
        DeviceTemplateManagementView* result,
        std::string* error_message = nullptr);
    // 更新设备模板字段的实时页展示开关。
    StatusCode update_device_template_realtime_display(
        const std::string& template_id,
        const std::string& field_key,
        bool show_in_realtime,
        DeviceTemplateManagementView* result,
        std::string* error_message = nullptr);
    // 更新设备类型的历史记录偏好。
    StatusCode update_device_template_history_enabled(
        const std::string& template_id,
        const std::string& field_key,
        bool history_enabled,
        DeviceTemplateManagementView* result,
        std::string* error_message = nullptr);
    // 删除未被引用的设备模板。
    StatusCode delete_device_template(
        const std::string& template_id,
        DeviceTemplateManagementView* result,
        std::string* error_message = nullptr);
    // 新增主站配置并重载运行态。
    StatusCode create_master_config(
        const MasterNodeConfigUpdateRequest& request,
        MasterNodeConfigUpdateResult* result,
        std::string* error_message = nullptr);
    // 更新主站配置并重载运行态。
    StatusCode update_master_config(
        const MasterNodeConfigUpdateRequest& request,
        MasterNodeConfigUpdateResult* result,
        std::string* error_message = nullptr);
    // 删除主站配置并同步其自动推导设备。
    StatusCode delete_master_config(
        const MasterNodeId& master_id,
        MasterNodeConfigDeleteResult* result,
        std::string* error_message = nullptr);
    // 获取自动推导后的设备配置列表。
    std::vector<DeviceConfig> get_devices() const;
    // 仅更新设备显示名称；空名称恢复系统推导名称，不重建采集链路。
    StatusCode update_device_display_name(
        const DeviceId& device_id,
        const std::string& display_name,
        DeviceConfig* result,
        std::string* error_message = nullptr);
    // 批量更新设备显示名称；逐项返回业务校验失败，持久化与运行态同步各执行一次。
    StatusCode update_device_display_names_batch(
        const std::vector<DeviceDisplayNameBatchItem>& items,
        DeviceDisplayNameBatchResult* result,
        std::string* error_message = nullptr);
    // 获取指定通道下的主站配置列表。
    std::vector<MasterNodeConfig> get_masters_by_channel(const ChannelId& channel_id) const;
    // 获取指定主站下的设备配置列表。
    std::vector<DeviceConfig> get_devices_by_master(const MasterNodeId& master_id) const;

    // 获取系统整体运行状态。
    SystemStatus get_system_status() const;
    // 获取全部通道运行状态。
    std::vector<ChannelStatus> get_channel_statuses() const;
    // 获取全部主站运行状态。
    std::vector<MasterNodeStatus> get_master_statuses() const;
    // 获取全部设备运行状态。
    std::vector<DeviceStatus> get_device_statuses() const;
    // 获取指定主站下的设备运行状态。
    std::vector<DeviceStatus> get_device_statuses_by_master(const MasterNodeId& master_id) const;
    // 获取单个通道运行状态。
    std::optional<ChannelStatus> get_channel_status(const ChannelId& channel_id) const;
    // 获取单个主站运行状态。
    std::optional<MasterNodeStatus> get_master_status(const MasterNodeId& master_id) const;
    // 获取单个设备运行状态。
    std::optional<DeviceStatus> get_device_status(const DeviceId& device_id) const;
    // 获取单个设备实时快照。
    std::optional<DeviceRealtimeSnapshot> get_device_realtime(const DeviceId& device_id) const;
    // 获取全部设备实时快照。
    std::vector<DeviceRealtimeSnapshot> get_all_device_realtime() const;
    // 获取指定主站下的设备实时快照。
    std::vector<DeviceRealtimeSnapshot> get_device_realtime_by_master(const MasterNodeId& master_id) const;
    // 获取实时监控页面所需聚合视图。
    RealtimeViewSnapshot get_realtime_view_snapshot() const;
    // 获取单设备历史页所需聚合视图。
    StatusCode get_device_history_view(
        const DeviceHistoryQuery& query,
        DeviceHistoryView* view,
        std::string* error_message = nullptr) const;
    // 批量获取历史总览页所需的轻量周期摘要。
    StatusCode get_history_overview_summaries(
        std::vector<HistoryOverviewSummary>* summaries,
        std::string* error_message = nullptr) const;
    // 按导出筛选条件分页读取历史采样记录。
    StatusCode export_history_records(
        const HistoryExportQuery& query,
        std::vector<HistoryRecord>* records,
        std::string* error_message = nullptr) const;
    // 获取数据维护摘要。
    StatusCode get_data_maintenance_summary(
        DataMaintenanceSummary* summary,
        std::string* error_message = nullptr) const;
    // 清理过期数据。
    StatusCode cleanup_expired_data(
        DataMaintenanceSummary* summary,
        std::string* error_message = nullptr);
    // 获取指定通道最近的 Modbus 通讯报文。
    std::vector<CommunicationTraceRecord> get_channel_communication_traces(
        const ChannelId& channel_id,
        std::size_t limit = 10) const;
    // 清空指定通道的通讯报文缓存。
    void clear_channel_communication_traces(const ChannelId& channel_id);
    // 执行设备模板定义的写命令。
    StatusCode execute_device_command(
        const DeviceCommandExecuteRequest& request,
        DeviceCommandExecuteResponse* response,
        std::string* error_message = nullptr);
    // 读取 EM100 事件记录。
    StatusCode read_em100_event_record(
        const DeviceId& device_id,
        EM100RecordReadResponse* response,
        std::string* error_message = nullptr);
    // 读取 EM100 测试记录。
    StatusCode read_em100_test_record(
        const DeviceId& device_id,
        EM100RecordReadResponse* response,
        std::string* error_message = nullptr);

    // 获取最近一次轮询周期摘要。
    PollingCycleSummary get_recent_polling_summary() const;
    // 获取后端最近错误摘要。
    ServiceErrorSummary get_recent_error_summary() const;
    // 获取后端最近事件列表。
    std::vector<ServiceEvent> get_recent_events(std::size_t limit = 100) const;
    // 获取最近事件。
    StatusCode get_recent_events(std::size_t limit, std::vector<ServiceEvent>* events, std::string* error_message = nullptr) const;
    // 按页面筛选条件读取历史事件和统计。
    StatusCode query_service_events(
        const EventHistoryQuery& query,
        EventHistoryResult* result,
        std::string* error_message = nullptr) const;
    // 按导出筛选条件分页读取持久化历史事件。
    StatusCode export_service_events(
        const EventExportQuery& query,
        std::vector<ServiceEvent>* events,
        std::string* error_message = nullptr) const;
    // 清空后端最近事件缓存。
    StatusCode clear_recent_events(std::string* error_message = nullptr);
    // 列出告警规则。
    StatusCode list_alarm_rules(std::vector<AlarmRule>* rules, std::string* error_message = nullptr) const;
    // 列出设备告警规则。
    StatusCode list_device_alarm_rules(const DeviceId& device_id, std::vector<AlarmRule>* rules, std::string* error_message = nullptr) const;
    // 新增或更新告警规则。
    StatusCode upsert_alarm_rule(const AlarmRule& rule, AlarmRule* saved_rule, std::string* message, std::string* error_message = nullptr);
    // 删除告警规则。
    StatusCode delete_alarm_rule(const DeviceId& device_id, const std::string& point_key, std::string* message, std::string* error_message = nullptr);
    // 列出活动告警。
    StatusCode list_active_alarms(std::vector<ActiveAlarmView>* alarms, std::string* error_message = nullptr) const;
    // 确认活动告警。
    StatusCode acknowledge_active_alarm(
        const DeviceId& device_id,
        const std::string& point_key,
        const std::string& acknowledged_by,
        const std::optional<TimestampMs>& active_since_ms,
        ActiveAlarmView* alarm,
        std::string* message,
        std::string* error_message = nullptr);
    // 请求恢复出厂数据并重新初始化配置。
    StatusCode request_factory_reset(FactoryResetResult* result, std::string* error_message = nullptr);

private:
    void deliver_alarm_event(const ServiceEvent& event);
    std::mutex alarm_delivery_mutex_;
    StatusCode write_multiple_holding_registers_locked(
        const ModbusWriteMultipleRegistersRequest& request,
        ModbusWriteMultipleRegistersResponse* response,
        std::string* error_message);
    struct ConfigApplyOutcome {
        std::string message;
        std::string warning_message;
        bool polling_restarted{false};
    };

    // 仅供 EM100 事件/测试记录命令读取设备协议规定的显式 FC03 地址范围。
    // 该维护命令不代表“按设备类型即时读取”；通用设备读取必须走 read_blocks 多区块模型。
    StatusCode read_em100_holding_register_range_once(
        const MasterNodeId& master_id,
        const DeviceId& device_id,
        std::uint16_t start_register,
        std::uint16_t register_count,
        ModbusReadHoldingRegistersResponse* response,
        std::string* error_message = nullptr);

    // 在持锁状态下判断轮询服务是否正在运行。
    bool is_polling_running_locked() const;
    // 在持锁状态下判断是否存在启用采集目标。
    bool has_enabled_collection_target_locked() const;
    // 在持锁状态下启动轮询。
    StatusCode start_polling_locked(std::string* error_message = nullptr);
    // 在持锁状态下分离当前轮询服务实例。
    std::shared_ptr<PollingService> detach_polling_service_locked(
        const std::string& polling_state,
        const std::string& status_message);
    // 停止轮询用于配置应用。
    void stop_polling_for_config_apply(
        std::unique_lock<std::shared_mutex>& lock,
        std::shared_ptr<PollingService>* polling_to_stop);
    // 在持锁状态下合并已停止轮询服务的最终状态。
    void merge_stopped_polling_service_locked(
        const PollingService* stopped_service,
        const std::string& polling_state,
        const std::string& status_message);
    // 在持锁状态下设置配置应用状态。
    void set_config_apply_status_locked(
        const std::string& polling_state,
        const std::string& status_message);
    // 在持锁状态下完成配置应用并生成结果。
    ConfigApplyOutcome finish_config_apply_locked(
        bool was_polling_running,
        const std::string& warning_message = {});
    // 在持锁状态下处理配置应用失败并恢复运行态。
    StatusCode fail_config_apply_locked(
        StatusCode status,
        const std::string& message,
        bool was_polling_running,
        std::string* error_message,
        bool recovery_succeeded = true);
    // 执行内部配置加载流程。
    StatusCode load_config_internal(std::vector<std::string>* errors);
    // 准备运行状态配置。
    // 在持锁状态下应用已准备运行状态配置。
    void apply_prepared_runtime_config_locked(PreparedRuntimeConfig&& prepared);
    // 重新加载配置用于应用。
    StatusCode reload_config_for_apply(
        std::unique_lock<std::shared_mutex>& lock,
        std::vector<std::string>* errors);
    // 构造默认运行状态。
    void build_default_runtime_status();
    // 刷新通道状态列表。
    void refresh_channel_statuses();

    // 配置变更前置检查。
    StatusCode ensure_config_mutation_ready_locked(
        const char* missing_result_message,
        const void* result,
        std::string* error_message) const;

    // 配置对象查找。
    StatusCode require_channel_config_locked(
        const ChannelId& channel_id,
        const ChannelConfig** channel,
        std::size_t* index,
        std::string* error_message) const;
    // 在持锁状态下读取并校验主站配置。
    StatusCode require_master_config_locked(
        const MasterNodeId& master_id,
        const MasterNodeConfig** master,
        std::size_t* index,
        std::string* error_message) const;
    // 配置唯一性校验。
    StatusCode ensure_channel_id_available_locked(const ChannelId& channel_id, std::string* error_message) const;

    // 在持锁状态下按主站配置同步自动推导设备。
    StatusCode sync_devices_for_master_configs_locked(
        const std::vector<MasterNodeConfig>& masters,
        std::vector<DeviceConfig>& devices,
        std::string* error_message) const;
    // 配置保存与运行态刷新。
    StatusCode apply_channel_configs_locked(
        std::unique_lock<std::shared_mutex>& lock,
        const std::vector<ChannelConfig>& channels,
        std::string* error_message);
    // 在持锁状态下写入 SQLite 主站配置并重新加载运行态。
    StatusCode write_sqlite_masters_and_reload_locked(
        std::unique_lock<std::shared_mutex>& lock,
        const std::vector<MasterNodeConfig>& masters,
        bool restore_polling,
        std::string* error_message);
    // 应用网络设置内部。
    StatusCode apply_network_settings_internal(
        bool startup_apply,
        NetworkApplyResult* result,
        std::string* error_message);
    // 调用者必须持有 modbus_management_mutex_；不得持有 service_mutex_ 等待 Server 线程。
    StatusCode reload_modbus_register_mappings_managed(std::string* error_message);

    // 查询视图和运行协调路径使用的只读配置定位。
    const ChannelConfig* find_channel_config(const ChannelId& channel_id) const;
    // 查找主站配置。
    const MasterNodeConfig* find_master_config(const MasterNodeId& master_id) const;
    // 查找设备配置。
    const DeviceConfig* find_device_config(const DeviceId& device_id) const;
    // 设置最近错误信息。
    void set_last_error(
        const std::string& source,
        const std::string& target_id,
        const std::string& message,
        TimestampMs timestamp_ms);
    // 追加事件。
    void append_event(
        const std::string& level,
        const std::string& source,
        const std::string& target_id,
        const std::string& summary,
        const std::string& detail,
        TimestampMs timestamp_ms,
        const DiagnosisStatus& diagnosis = DiagnosisStatus{});
    // 在现有数据维护线程中重试已生成事件，不重新执行告警状态机。
    void retry_pending_events();
    // 持久化成功后统一执行一次 MQTT 事件发布。
    void publish_persisted_event(const ServiceEvent& event);
    // 在持锁状态下构造告警点位上下文。
    std::vector<AlarmPointContext> build_alarm_point_contexts_locked() const;
    // 查找指定告警点位上下文。
    const AlarmPointContext* find_alarm_point_context(const std::vector<AlarmPointContext>& contexts, const DeviceId& device_id, const std::string& point_key) const;
    // 在持锁状态下同步告警拓扑。
    void synchronize_alarm_topology_locked(const std::string& reason);
    // 确保主站绑定的通道已经就绪。
    StatusCode ensure_channel_ready_for_master(const MasterNodeConfig& master_config, std::string* error_message);
    // 启动时间跳变监测。
    StatusCode start_time_jump_monitor(std::string* error_message = nullptr);
    // 停止时间跳变监测。
    void stop_time_jump_monitor();
    // 运行时间跳变监测循环。
    void time_jump_monitor_loop();
    // 启动数据维护。
    StatusCode start_data_maintenance(std::string* error_message = nullptr);
    // 停止数据维护。
    void stop_data_maintenance();
    // 运行定期数据维护循环。
    void data_maintenance_loop();

    // 锁层级约定：配置与运行态所有权切换使用独占锁；页面/IPC 快照和手动设备 I/O 使用共享锁。
    // 需要同时获取组件内部锁时先持有本锁，再进入 RuntimeStateStore、MQTT、Alarm 等组件；
    // 耗时停止/join 在锁外执行。
    mutable std::shared_mutex service_mutex_;
    // 网络配置保存、应用和失败回滚必须完整串行；锁顺序固定为本锁 -> service_mutex_。
    mutable std::mutex network_operation_mutex_;
    // 通道应用与关闭串行；锁顺序为本锁 -> manual_modbus_mutex_ -> service_mutex_。
    mutable std::mutex channel_operation_mutex_;
    // 手动 Modbus 与通道应用串行访问 ChannelManager；先于 service_mutex_ 获取。
    mutable std::mutex manual_modbus_mutex_;
    mutable std::mutex error_mutex_;
    mutable std::mutex time_service_mutex_;
    mutable std::mutex time_monitor_mutex_;
    mutable std::mutex time_adjustment_mutex_;
    mutable std::mutex maintenance_mutex_;
    // 序列化可能并发触发的 Server 设置应用、映射重载和关闭动作。
    mutable std::mutex modbus_management_mutex_;
    std::condition_variable time_monitor_wakeup_;
    mutable std::mutex data_maintenance_thread_mutex_;
    std::condition_variable data_maintenance_wakeup_;

    bool initialized_{false};
    SystemConfig system_config_{};
    TimeSettings applied_time_settings_{};
    bool applied_time_settings_initialized_{false};
    RuntimeStateStore data_store_{};
    HistoryStore history_store_{};
    AlarmStore alarm_store_{};
    EventStore event_store_{};
    AlarmEvaluator alarm_evaluator_{};
    ConfigStore config_store_{};
    RuntimeConfigCompiler runtime_config_compiler_{config_store_};
    DeviceTemplateStore device_template_store_{};
    CommunicationTraceStore communication_trace_store_{};
    MqttPublisherService mqtt_publisher_service_{};
    std::shared_ptr<ModbusRegisterBank> modbus_register_bank_{};
    std::shared_ptr<ModbusExportService> modbus_export_service_{std::make_shared<ModbusExportService>()};
    ModbusTcpServer modbus_tcp_server_{};
    ModbusServerSettings modbus_server_settings_{};
    std::vector<ModbusRegisterMapping> modbus_register_mappings_{};
    ChannelManager channel_manager_{};
    TopologyManager topology_manager_{};
    SerialPortEnumerator serial_port_enumerator_{};
    UpdateService update_service_{};
    LinuxTimeRuntime time_runtime_{};
    TimeJumpDetector time_jump_detector_{};
    std::thread time_monitor_thread_{};
    std::thread data_maintenance_thread_{};
    bool data_maintenance_stop_requested_{true};
    bool time_monitor_stop_requested_{true};
    std::string last_time_adjustment_source_;
    TimestampMs last_time_adjustment_after_ms_{0};
    std::int64_t last_time_adjustment_delta_ms_{0};
    PollingRuntime polling_runtime_{};
    std::atomic_bool config_apply_in_progress_{false};
    std::atomic_bool event_persistence_suppressed_{false};
    DataMaintenanceSummary data_maintenance_summary_{};
    ServiceErrorSummary last_error_summary_{};
};

}  // namespace edge_controller
