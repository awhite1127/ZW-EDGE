package service

// 本文件是 Go Web 到 C++ 后端 IPC 的薄适配层：负责方法名、请求模型和响应模型映射，
// 不在此复制采集、配置应用或状态判断等后端业务逻辑。

import (
	"context"
	"time"

	"edge-web/internal/ipc"
	"edge-web/internal/model"
)

// BackendService 是 Go Web 侧对后端 IPC 方法的一层薄封装。
// 这一层只负责把 method、params、result 映射成稳定的 Go 接口。
type BackendService struct {
	client  *ipc.Client
	timeout time.Duration
}

func NewBackendServiceWithTimeout(client *ipc.Client, timeout time.Duration) *BackendService {
	if timeout <= 0 {
		timeout = 15 * time.Second
	}
	return &BackendService{client: client, timeout: timeout}
}

// WithTimeout 为上层提供统一的后端调用默认超时。
func (s *BackendService) WithTimeout(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(parent, s.timeout)
}

func (s *BackendService) GetSystemStatus(ctx context.Context) (model.SystemStatus, error) {
	var result model.SystemStatus
	err := s.client.Call(ctx, "get_system_status", nil, &result)
	return result, err
}

// GetUpdateCurrentVersion 查询当前安装版本，供后续运维升级页面使用。
func (s *BackendService) GetUpdateCurrentVersion(ctx context.Context) (model.UpdateVersion, error) {
	var result model.UpdateVersion
	err := s.client.Call(ctx, "get_update_current_version", nil, &result)
	return result, err
}

func (s *BackendService) GetUpdateStatus(ctx context.Context) (model.UpdateStatus, error) {
	var result model.UpdateStatus
	err := s.client.Call(ctx, "get_update_status", nil, &result)
	return result, err
}

// ImportApplicationUpgradePackage 请求 root 后端接管指定上传标识，并复用正式校验创建任务。
func (s *BackendService) ImportApplicationUpgradePackage(
	ctx context.Context,
	uploadID string,
	packageIdentifier string,
) (model.UpdateStatus, error) {
	var result model.UpdateStatus
	err := s.client.CallWithTimeout(
		ctx,
		"import_application_upgrade_package",
		map[string]string{"upload_id": uploadID, "package": packageIdentifier},
		&result,
		5*time.Minute,
	)
	return result, err
}

// StartUpdateJob 请求 systemd 独立 root runner 接管 READY 任务。
func (s *BackendService) StartUpdateJob(ctx context.Context, jobID string) (model.UpdateStatus, error) {
	var result model.UpdateStatus
	err := s.client.CallWithTimeout(
		ctx,
		"start_update_job",
		map[string]string{"job_id": jobID},
		&result,
		30*time.Second,
	)
	return result, err
}

func (s *BackendService) GetDataMaintenanceSummary(ctx context.Context) (model.DataMaintenanceSummary, error) {
	var result model.DataMaintenanceSummary
	err := s.client.Call(ctx, "get_data_maintenance_summary", nil, &result)
	return result, err
}

func (s *BackendService) CleanupExpiredData(ctx context.Context) (model.DataMaintenanceSummary, error) {
	var result model.DataMaintenanceSummary
	err := s.client.Call(ctx, "cleanup_expired_data", nil, &result)
	return result, err
}

func (s *BackendService) GetRealtimeViewSnapshot(ctx context.Context) (model.RealtimeViewSnapshot, error) {
	var result model.RealtimeViewSnapshot
	err := s.client.Call(ctx, "get_realtime_view_snapshot", nil, &result)
	if err == nil {
		hydrateRealtimeStatusPoints(&result)
	}
	return result, err
}

// hydrateRealtimeStatusPoints 把聚合报文中唯一一份点位切片挂回设备状态。
// 快照在一次请求内只读，两个视图共享底层切片可避免为每台设备再次分配和复制。
func hydrateRealtimeStatusPoints(snapshot *model.RealtimeViewSnapshot) {
	if snapshot == nil || len(snapshot.SystemStatus.DeviceStatusList) == 0 || len(snapshot.DeviceRealtimeSnapshots) == 0 {
		return
	}
	pointsByDevice := make(map[string][]model.PointValue, len(snapshot.DeviceRealtimeSnapshots))
	for _, realtime := range snapshot.DeviceRealtimeSnapshots {
		if realtime.DeviceID != "" && len(realtime.Points) > 0 {
			pointsByDevice[realtime.DeviceID] = realtime.Points
		}
	}
	for index := range snapshot.SystemStatus.DeviceStatusList {
		status := &snapshot.SystemStatus.DeviceStatusList[index]
		if len(status.Points) != 0 {
			continue
		}
		if points, ok := pointsByDevice[status.DeviceID]; ok {
			status.Points = points
		}
	}
}

func (s *BackendService) GetSystemOverviewSnapshot(ctx context.Context) (model.SystemOverviewSnapshot, error) {
	var result model.SystemOverviewSnapshot
	err := s.client.Call(ctx, "get_system_overview_snapshot", nil, &result)
	return result, err
}

func (s *BackendService) GetModbusServerPageSnapshot(ctx context.Context) (model.ModbusServerPageSnapshot, error) {
	var result model.ModbusServerPageSnapshot
	err := s.client.Call(ctx, "get_modbus_server_page_snapshot", nil, &result)
	return result, err
}

func (s *BackendService) GetModbusServerRuntimeStatus(ctx context.Context) (model.ModbusServerRuntimeStatus, error) {
	var result model.ModbusServerRuntimeStatus
	err := s.client.Call(ctx, "get_modbus_server_runtime_status", nil, &result)
	return result, err
}

func (s *BackendService) ListModbusExportablePoints(ctx context.Context) ([]model.ModbusExportablePoint, error) {
	var result []model.ModbusExportablePoint
	err := s.client.Call(ctx, "list_modbus_exportable_points", nil, &result)
	return result, err
}

func (s *BackendService) ListModbusRegisterMappings(ctx context.Context) ([]model.ModbusRegisterMapping, error) {
	var result []model.ModbusRegisterMapping
	err := s.client.Call(ctx, "list_modbus_register_mappings", nil, &result)
	return result, err
}

func (s *BackendService) UpdateModbusServerSettings(
	ctx context.Context,
	request model.ModbusServerSettings,
) (model.ModbusServerSettings, error) {
	var result model.ModbusServerSettings
	err := s.client.Call(ctx, "update_modbus_server_settings", request, &result)
	return result, err
}

func (s *BackendService) CreateModbusRegisterMapping(
	ctx context.Context,
	request model.ModbusRegisterMappingRequest,
) (model.ModbusRegisterMapping, error) {
	var result model.ModbusRegisterMapping
	err := s.client.Call(ctx, "create_modbus_register_mapping", request, &result)
	return result, err
}

func (s *BackendService) UpdateModbusRegisterMapping(
	ctx context.Context,
	mappingID string,
	request model.ModbusRegisterMappingRequest,
) (model.ModbusRegisterMapping, error) {
	params := struct {
		model.ModbusRegisterMappingRequest
		MappingID string `json:"mapping_id"`
	}{ModbusRegisterMappingRequest: request, MappingID: mappingID}
	var result model.ModbusRegisterMapping
	err := s.client.Call(ctx, "update_modbus_register_mapping", params, &result)
	return result, err
}

func (s *BackendService) DeleteModbusRegisterMapping(ctx context.Context, mappingID string) error {
	return s.client.Call(ctx, "delete_modbus_register_mapping", map[string]string{"mapping_id": mappingID}, nil)
}

func (s *BackendService) GetOverviewPageSnapshot(ctx context.Context) (model.OverviewPageSnapshot, error) {
	var result model.OverviewPageSnapshot
	err := s.client.Call(ctx, "get_overview_page_snapshot", nil, &result)
	return result, err
}

func (s *BackendService) GetPollingSummary(ctx context.Context) (model.PollingCycleSummary, error) {
	var result model.PollingCycleSummary
	err := s.client.Call(ctx, "get_recent_polling_summary", nil, &result)
	return result, err
}

func (s *BackendService) GetRecentError(ctx context.Context) (model.ServiceErrorSummary, error) {
	var result model.ServiceErrorSummary
	err := s.client.Call(ctx, "get_recent_error_summary", nil, &result)
	return result, err
}

func (s *BackendService) ListRecentEvents(ctx context.Context) ([]model.ServiceEvent, error) {
	var result []model.ServiceEvent
	err := s.client.Call(ctx, "list_recent_events", nil, &result)
	return result, err
}

func (s *BackendService) ExportServiceEvents(
	ctx context.Context,
	query model.EventExportQuery,
) ([]model.ServiceEvent, error) {
	var result []model.ServiceEvent
	err := s.client.Call(ctx, "export_service_events", query, &result)
	return result, err
}

func (s *BackendService) ClearRecentEvents(ctx context.Context) (model.ActionFeedback, error) {
	var result struct {
		Message string `json:"message"`
	}
	err := s.client.Call(ctx, "clear_recent_events", nil, &result)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}, err
	}
	message := result.Message
	if message == "" {
		message = "历史事件已清除"
	}
	return model.ActionFeedback{Success: true, Message: message}, nil
}

func (s *BackendService) ListActiveAlarms(ctx context.Context) ([]model.ActiveAlarm, error) {
	var result []model.ActiveAlarm
	err := s.client.Call(ctx, "list_active_alarms", nil, &result)
	return result, err
}

func (s *BackendService) AcknowledgeActiveAlarm(
	ctx context.Context,
	request model.ActiveAlarmAcknowledgeRequest,
) (model.ActiveAlarmAcknowledgeResult, error) {
	var result model.ActiveAlarmAcknowledgeResult
	err := s.client.Call(ctx, "acknowledge_active_alarm", request, &result)
	return result, err
}

func (s *BackendService) ListAlarmRules(ctx context.Context, deviceID string) ([]model.AlarmRule, error) {
	var result []model.AlarmRule
	var params interface{}
	if deviceID != "" {
		params = map[string]string{"device_id": deviceID}
	}
	err := s.client.Call(ctx, "list_alarm_rules", params, &result)
	return result, err
}

func (s *BackendService) UpsertAlarmRule(ctx context.Context, rule model.AlarmRule) (model.AlarmRuleUpsertResult, error) {
	var result model.AlarmRuleUpsertResult
	err := s.client.Call(ctx, "upsert_alarm_rule", rule, &result)
	return result, err
}

func (s *BackendService) DeleteAlarmRule(ctx context.Context, deviceID string, pointKey string) (model.AlarmRuleDeleteResult, error) {
	var result model.AlarmRuleDeleteResult
	err := s.client.Call(ctx, "delete_alarm_rule", map[string]string{
		"device_id": deviceID,
		"point_key": pointKey,
	}, &result)
	return result, err
}

func (s *BackendService) GetConfigSummary(ctx context.Context) (model.ConfigSummary, error) {
	var result model.ConfigSummary
	err := s.client.Call(ctx, "get_config_summary", nil, &result)
	return result, err
}

func (s *BackendService) GetDeviceTemplateManagement(ctx context.Context) (model.DeviceTemplateManagementView, error) {
	var result model.DeviceTemplateManagementView
	err := s.client.Call(ctx, "get_device_template_management", nil, &result)
	return result, err
}

func (s *BackendService) CreateDeviceTemplate(ctx context.Context, request model.DeviceTemplateDefinition) (model.DeviceTemplateMutationResult, error) {
	var result model.DeviceTemplateMutationResult
	err := s.client.Call(ctx, "create_device_template", request, &result)
	return result, err
}

func (s *BackendService) UpdateDeviceTemplate(ctx context.Context, request model.DeviceTemplateDefinition) (model.DeviceTemplateMutationResult, error) {
	var result model.DeviceTemplateMutationResult
	err := s.client.Call(ctx, "update_device_template", request, &result)
	return result, err
}

func (s *BackendService) DeleteDeviceTemplate(ctx context.Context, templateID string) (model.DeviceTemplateMutationResult, error) {
	var result model.DeviceTemplateMutationResult
	err := s.client.Call(ctx, "delete_device_template", map[string]string{"template_id": templateID}, &result)
	return result, err
}

func (s *BackendService) UpdateDeviceTemplateRealtimeDisplay(
	ctx context.Context,
	request model.DeviceTemplateRealtimeDisplayRequest,
) (model.DeviceTemplateRealtimeDisplayResult, error) {
	var result model.DeviceTemplateRealtimeDisplayResult
	err := s.client.Call(ctx, "update_device_template_realtime_display", request, &result)
	return result, err
}

func (s *BackendService) UpdateDeviceTemplateHistoryEnabled(
	ctx context.Context,
	request model.DeviceTemplateHistoryEnabledRequest,
) (model.DeviceTemplateHistoryEnabledResult, error) {
	var result model.DeviceTemplateHistoryEnabledResult
	err := s.client.Call(ctx, "update_device_template_history_enabled", request, &result)
	return result, err
}

func (s *BackendService) GetSystemSettings(ctx context.Context) (model.SystemSettings, error) {
	var result model.SystemSettings
	err := s.client.Call(ctx, "get_system_settings", nil, &result)
	return result, err
}

func (s *BackendService) GetTimeSettings(ctx context.Context) (model.TimeSettings, error) {
	var result model.TimeSettings
	err := s.client.Call(ctx, "get_time_settings", nil, &result)
	return result, err
}

func (s *BackendService) GetTimeRuntimeStatus(ctx context.Context) (model.TimeRuntimeStatus, error) {
	var result model.TimeRuntimeStatus
	err := s.client.Call(ctx, "get_time_runtime_status", nil, &result)
	return result, err
}

func (s *BackendService) SaveAndApplyTimeSettings(ctx context.Context, request model.TimeSettingsUpdateRequest) (model.TimeApplyResult, error) {
	var result model.TimeApplyResult
	err := s.client.Call(ctx, "save_and_apply_time_settings", request, &result)
	return result, err
}

func (s *BackendService) SyncTimeNow(ctx context.Context) (model.TimeSyncResult, error) {
	var result model.TimeSyncResult
	err := s.client.CallWithTimeout(ctx, "sync_time_now", nil, &result, 80*time.Second)
	return result, err
}

func (s *BackendService) SetManualSystemTime(ctx context.Context, request model.ManualTimeSetRequest) (model.ManualTimeSetResult, error) {
	var result model.ManualTimeSetResult
	err := s.client.Call(ctx, "set_manual_system_time", request, &result)
	return result, err
}

func (s *BackendService) GetNetworkSettings(ctx context.Context) (model.NetworkSettings, error) {
	var result model.NetworkSettings
	err := s.client.Call(ctx, "get_network_settings", nil, &result)
	return result, err
}

func (s *BackendService) GetNetworkRuntimeStatus(ctx context.Context) (model.NetworkRuntimeStatus, error) {
	var result model.NetworkRuntimeStatus
	err := s.client.Call(ctx, "get_network_runtime_status", nil, &result)
	return result, err
}

func (s *BackendService) GetMqttSettings(ctx context.Context) (model.MqttSettings, error) {
	var result model.MqttSettings
	err := s.client.Call(ctx, "get_mqtt_settings", nil, &result)
	return result, err
}

func (s *BackendService) GetMqttRuntimeStatus(ctx context.Context) (model.MqttRuntimeStatus, error) {
	var result model.MqttRuntimeStatus
	err := s.client.Call(ctx, "get_mqtt_runtime_status", nil, &result)
	return result, err
}

func (s *BackendService) UpdateSystemSettings(
	ctx context.Context,
	request model.SystemSettingsUpdateRequest,
) (model.SystemSettingsUpdateResult, error) {
	var result model.SystemSettingsUpdateResult
	err := s.client.Call(ctx, "update_system_settings", request, &result)
	return result, err
}

func (s *BackendService) UpdateNetworkSettings(
	ctx context.Context,
	request model.NetworkSettingsUpdateRequest,
) (model.NetworkSettingsUpdateResult, error) {
	var result model.NetworkSettingsUpdateResult
	err := s.client.Call(ctx, "update_network_settings", request, &result)
	return result, err
}

func (s *BackendService) SaveAndApplyNetworkSettings(
	ctx context.Context,
	request model.NetworkSettingsUpdateRequest,
) (model.NetworkApplyResult, error) {
	var result model.NetworkApplyResult
	err := s.client.Call(ctx, "save_and_apply_network_settings", request, &result)
	return result, err
}

func (s *BackendService) UpdateMqttSettings(
	ctx context.Context,
	request model.MqttSettingsUpdateRequest,
) (model.MqttSettingsUpdateResult, error) {
	var result model.MqttSettingsUpdateResult
	err := s.client.Call(ctx, "update_mqtt_settings", request, &result)
	return result, err
}

func (s *BackendService) ExportSystemConfig(ctx context.Context) (model.ConfigExportBundle, error) {
	var result model.ConfigExportBundle
	err := s.client.Call(ctx, "export_system_config", nil, &result)
	return result, err
}

func (s *BackendService) ImportSystemConfig(
	ctx context.Context,
	bundle model.ConfigExportBundle,
) (model.ConfigImportResult, error) {
	var result model.ConfigImportResult
	err := s.client.Call(ctx, "import_system_config", bundle, &result)
	return result, err
}

func (s *BackendService) ApplyNetworkSettings(ctx context.Context) (model.NetworkApplyResult, error) {
	var result model.NetworkApplyResult
	err := s.client.Call(ctx, "apply_network_settings", nil, &result)
	return result, err
}

func (s *BackendService) RequestFactoryReset(ctx context.Context) (model.FactoryResetResult, error) {
	var result model.FactoryResetResult
	err := s.client.Call(ctx, "request_factory_reset", nil, &result)
	return result, err
}

func (s *BackendService) GetWebAuthStatus(ctx context.Context) (model.WebAuthStatus, error) {
	var result model.WebAuthStatus
	err := s.client.Call(ctx, "get_web_auth_status", nil, &result)
	return result, err
}

func (s *BackendService) GetFirstBootAdminEntryStatus(ctx context.Context) (model.FirstBootAdminEntryStatus, error) {
	var result model.FirstBootAdminEntryStatus
	err := s.client.Call(ctx, "get_first_boot_admin_entry_status", nil, &result)
	return result, err
}

func (s *BackendService) ConsumeFirstBootAdminEntry(ctx context.Context) (model.FirstBootAdminEntryResult, error) {
	var result model.FirstBootAdminEntryResult
	err := s.client.Call(ctx, "consume_first_boot_admin_entry", nil, &result)
	return result, err
}

func (s *BackendService) VerifyWebLogin(
	ctx context.Context,
	request model.WebLoginRequest,
) (model.WebLoginResult, error) {
	var result model.WebLoginResult
	err := s.client.Call(ctx, "verify_web_login", request, &result)
	return result, err
}

func (s *BackendService) ChangeWebPassword(
	ctx context.Context,
	request model.WebPasswordChangeRequest,
) (model.WebPasswordChangeResult, error) {
	var result model.WebPasswordChangeResult
	err := s.client.Call(ctx, "change_web_password", request, &result)
	return result, err
}

func (s *BackendService) SetWebViewerPassword(
	ctx context.Context,
	request model.WebViewerPasswordSetRequest,
) (model.WebPasswordChangeResult, error) {
	var result model.WebPasswordChangeResult
	err := s.client.Call(ctx, "set_web_viewer_password", request, &result)
	return result, err
}

func (s *BackendService) ListWebUsers(ctx context.Context) ([]model.WebUser, error) {
	var result []model.WebUser
	err := s.client.Call(ctx, "list_web_users", nil, &result)
	return result, err
}

func (s *BackendService) CreateWebUser(
	ctx context.Context,
	request model.WebUserCreateRequest,
) (model.WebUserMutationResult, error) {
	var result model.WebUserMutationResult
	err := s.client.Call(ctx, "create_web_user", request, &result)
	return result, err
}

func (s *BackendService) UpdateWebUser(
	ctx context.Context,
	request model.WebUserUpdateRequest,
) (model.WebUserMutationResult, error) {
	var result model.WebUserMutationResult
	err := s.client.Call(ctx, "update_web_user", request, &result)
	return result, err
}

func (s *BackendService) ResetWebUserPassword(
	ctx context.Context,
	request model.WebUserPasswordResetRequest,
) (model.WebUserMutationResult, error) {
	var result model.WebUserMutationResult
	err := s.client.Call(ctx, "reset_web_user_password", request, &result)
	return result, err
}

func (s *BackendService) ListChannels(ctx context.Context) ([]model.ChannelConfig, error) {
	var result []model.ChannelConfig
	err := s.client.Call(ctx, "list_channels", nil, &result)
	return result, err
}

func (s *BackendService) CreateChannelConfig(
	ctx context.Context,
	request model.ChannelConfigUpdateRequest,
) (model.ChannelConfigUpdateResult, error) {
	var result model.ChannelConfigUpdateResult
	err := s.client.Call(ctx, "create_channel_config", request, &result)
	return result, err
}

func (s *BackendService) UpdateChannelConfig(
	ctx context.Context,
	request model.ChannelConfigUpdateRequest,
) (model.ChannelConfigUpdateResult, error) {
	var result model.ChannelConfigUpdateResult
	err := s.client.Call(ctx, "update_channel_config", request, &result)
	return result, err
}

func (s *BackendService) DeleteChannelConfig(
	ctx context.Context,
	channelID string,
) (model.ChannelConfigDeleteResult, error) {
	var result model.ChannelConfigDeleteResult
	err := s.client.Call(ctx, "delete_channel_config", map[string]string{"channel_id": channelID}, &result)
	return result, err
}

func (s *BackendService) GetChannelCommunicationTraces(
	ctx context.Context,
	query model.CommunicationTraceQuery,
) (model.ChannelCommunicationTraces, error) {
	var result model.ChannelCommunicationTraces
	err := s.client.Call(ctx, "get_channel_communication_traces", query, &result)
	return result, err
}

func (s *BackendService) ClearChannelCommunicationTraces(
	ctx context.Context,
	channelID string,
) (model.CommunicationTraceClearResult, error) {
	var result model.CommunicationTraceClearResult
	err := s.client.Call(
		ctx,
		"clear_channel_communication_traces",
		map[string]string{"channel_id": channelID},
		&result,
	)
	return result, err
}

func (s *BackendService) ExecuteDeviceCommand(
	ctx context.Context,
	request model.DeviceCommandExecuteRequest,
) (model.DeviceCommandExecuteResponse, error) {
	var result model.DeviceCommandExecuteResponse
	err := s.client.Call(ctx, "execute_device_command", request, &result)
	return result, err
}

func (s *BackendService) ReadEM100EventRecord(ctx context.Context, deviceID string) (model.EM100RecordReadResponse, error) {
	var result model.EM100RecordReadResponse
	err := s.client.Call(ctx, "read_em100_event_record", map[string]string{"device_id": deviceID}, &result)
	return result, err
}

func (s *BackendService) ReadEM100TestRecord(ctx context.Context, deviceID string) (model.EM100RecordReadResponse, error) {
	var result model.EM100RecordReadResponse
	err := s.client.Call(ctx, "read_em100_test_record", map[string]string{"device_id": deviceID}, &result)
	return result, err
}

func (s *BackendService) ListSerialPorts(ctx context.Context) ([]model.SerialPortInfo, error) {
	var result []model.SerialPortInfo
	err := s.client.Call(ctx, "list_serial_ports", nil, &result)
	return result, err
}

func (s *BackendService) ListMasters(ctx context.Context) ([]model.MasterNodeConfig, error) {
	var result []model.MasterNodeConfig
	err := s.client.Call(ctx, "list_masters", nil, &result)
	return result, err
}

func (s *BackendService) CreateMasterConfig(
	ctx context.Context,
	request model.MasterConfigUpdateRequest,
) (model.MasterConfigUpdateResult, error) {
	var result model.MasterConfigUpdateResult
	err := s.client.Call(ctx, "create_master_config", request, &result)
	return result, err
}

func (s *BackendService) UpdateMasterConfig(
	ctx context.Context,
	request model.MasterConfigUpdateRequest,
) (model.MasterConfigUpdateResult, error) {
	var result model.MasterConfigUpdateResult
	err := s.client.Call(ctx, "update_master_config", request, &result)
	return result, err
}

func (s *BackendService) DeleteMasterConfig(
	ctx context.Context,
	masterID string,
) (model.MasterConfigDeleteResult, error) {
	var result model.MasterConfigDeleteResult
	err := s.client.Call(ctx, "delete_master_config", map[string]string{"master_id": masterID}, &result)
	return result, err
}

func (s *BackendService) ListDevices(ctx context.Context) ([]model.DeviceConfig, error) {
	var result []model.DeviceConfig
	err := s.client.Call(ctx, "list_devices", nil, &result)
	return result, err
}

func (s *BackendService) UpdateDeviceDisplayName(
	ctx context.Context,
	request model.DeviceDisplayNameUpdateRequest,
) (model.DeviceConfig, error) {
	var result model.DeviceConfig
	err := s.client.Call(ctx, "update_device_display_name", request, &result)
	return result, err
}

func (s *BackendService) UpdateDeviceDisplayNamesBatch(
	ctx context.Context,
	request model.DeviceDisplayNameBatchRequest,
) (model.DeviceDisplayNameBatchResult, error) {
	var result model.DeviceDisplayNameBatchResult
	err := s.client.Call(ctx, "update_device_display_names_batch", request, &result)
	return result, err
}

func (s *BackendService) GetDeviceHistoryView(
	ctx context.Context,
	query model.DeviceHistoryQuery,
) (model.DeviceHistoryView, error) {
	var result model.DeviceHistoryView
	err := s.client.Call(ctx, "get_device_history_view", query, &result)
	return result, err
}

func (s *BackendService) GetHistoryOverviewSummaries(ctx context.Context) ([]model.HistoryOverviewSummary, error) {
	var result []model.HistoryOverviewSummary
	err := s.client.Call(ctx, "get_history_overview_summaries", nil, &result)
	return result, err
}

func (s *BackendService) ExportHistoryRecords(
	ctx context.Context,
	query model.HistoryExportQuery,
) ([]model.HistoryRecord, error) {
	var result []model.HistoryRecord
	err := s.client.Call(ctx, "export_history_records", query, &result)
	return result, err
}

func (s *BackendService) StartPolling(ctx context.Context) (model.PollingCycleSummary, error) {
	var result model.PollingCycleSummary
	err := s.client.Call(ctx, "start_polling", nil, &result)
	return result, err
}

func (s *BackendService) StopPolling(ctx context.Context) (model.PollingCycleSummary, error) {
	var result model.PollingCycleSummary
	err := s.client.Call(ctx, "stop_polling", nil, &result)
	return result, err
}
