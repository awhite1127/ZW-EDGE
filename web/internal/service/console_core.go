package service

// 本文件定义 Web 控制台依赖的后端能力边界，并集中保存跨页面共享的服务状态。
// 页面服务只依赖 backendAPI，便于使用 IPC 实现或测试替身而不接触数据库和硬件。

import (
	"context"
	"edge-web/internal/model"
	"sync"
)

type backendAPI interface {
	backendRuntimeAPI
	backendEventHistoryAPI
	backendConfigurationAPI
	backendCollectionAPI
	backendUpdateAPI
}

// backendRuntimeAPI 汇总控制台高频运行态和北向服务查询。
type backendRuntimeAPI interface {
	WithTimeout(parent context.Context) (context.Context, context.CancelFunc)
	GetSystemStatus(ctx context.Context) (model.SystemStatus, error)
	GetRealtimeViewSnapshot(ctx context.Context) (model.RealtimeViewSnapshot, error)
	GetSystemOverviewSnapshot(ctx context.Context) (model.SystemOverviewSnapshot, error)
	GetOverviewPageSnapshot(ctx context.Context) (model.OverviewPageSnapshot, error)
	GetPollingSummary(ctx context.Context) (model.PollingCycleSummary, error)
	GetRecentError(ctx context.Context) (model.ServiceErrorSummary, error)
	GetModbusServerPageSnapshot(ctx context.Context) (model.ModbusServerPageSnapshot, error)
	GetModbusServerRuntimeStatus(ctx context.Context) (model.ModbusServerRuntimeStatus, error)
	ListModbusExportablePoints(ctx context.Context) ([]model.ModbusExportablePoint, error)
	ListModbusRegisterMappings(ctx context.Context) ([]model.ModbusRegisterMapping, error)
	UpdateModbusServerSettings(ctx context.Context, request model.ModbusServerSettings) (model.ModbusServerSettings, error)
	CreateModbusRegisterMapping(ctx context.Context, request model.ModbusRegisterMappingRequest) (model.ModbusRegisterMapping, error)
	UpdateModbusRegisterMapping(ctx context.Context, mappingID string, request model.ModbusRegisterMappingRequest) (model.ModbusRegisterMapping, error)
	DeleteModbusRegisterMapping(ctx context.Context, mappingID string) error
	GetDataMaintenanceSummary(ctx context.Context) (model.DataMaintenanceSummary, error)
	CleanupExpiredData(ctx context.Context) (model.DataMaintenanceSummary, error)
	StartPolling(ctx context.Context) (model.PollingCycleSummary, error)
	StopPolling(ctx context.Context) (model.PollingCycleSummary, error)
}

// backendEventHistoryAPI 统一事件、报警和历史数据能力，批量摘要是当前正式协议的一部分。
type backendEventHistoryAPI interface {
	ListRecentEvents(ctx context.Context) ([]model.ServiceEvent, error)
	ExportServiceEvents(ctx context.Context, query model.EventExportQuery) ([]model.ServiceEvent, error)
	ClearRecentEvents(ctx context.Context) (model.ActionFeedback, error)
	ListActiveAlarms(ctx context.Context) ([]model.ActiveAlarm, error)
	AcknowledgeActiveAlarm(ctx context.Context, request model.ActiveAlarmAcknowledgeRequest) (model.ActiveAlarmAcknowledgeResult, error)
	ListAlarmRules(ctx context.Context, deviceID string) ([]model.AlarmRule, error)
	UpsertAlarmRule(ctx context.Context, rule model.AlarmRule) (model.AlarmRuleUpsertResult, error)
	DeleteAlarmRule(ctx context.Context, deviceID string, pointKey string) (model.AlarmRuleDeleteResult, error)
	GetDeviceHistoryView(ctx context.Context, query model.DeviceHistoryQuery) (model.DeviceHistoryView, error)
	GetHistoryOverviewSummaries(ctx context.Context) ([]model.HistoryOverviewSummary, error)
	ExportHistoryRecords(ctx context.Context, query model.HistoryExportQuery) ([]model.HistoryRecord, error)
}

// backendConfigurationAPI 覆盖设置、设备类型、账户与配置维护边界。
type backendConfigurationAPI interface {
	GetConfigSummary(ctx context.Context) (model.ConfigSummary, error)
	GetDeviceTemplateManagement(ctx context.Context) (model.DeviceTemplateManagementView, error)
	CreateDeviceTemplate(ctx context.Context, request model.DeviceTemplateDefinition) (model.DeviceTemplateMutationResult, error)
	UpdateDeviceTemplate(ctx context.Context, request model.DeviceTemplateDefinition) (model.DeviceTemplateMutationResult, error)
	DeleteDeviceTemplate(ctx context.Context, templateID string) (model.DeviceTemplateMutationResult, error)
	UpdateDeviceTemplateRealtimeDisplay(ctx context.Context, request model.DeviceTemplateRealtimeDisplayRequest) (model.DeviceTemplateRealtimeDisplayResult, error)
	UpdateDeviceTemplateHistoryEnabled(ctx context.Context, request model.DeviceTemplateHistoryEnabledRequest) (model.DeviceTemplateHistoryEnabledResult, error)
	GetSystemSettings(ctx context.Context) (model.SystemSettings, error)
	GetTimeSettings(ctx context.Context) (model.TimeSettings, error)
	GetTimeRuntimeStatus(ctx context.Context) (model.TimeRuntimeStatus, error)
	SaveAndApplyTimeSettings(ctx context.Context, request model.TimeSettingsUpdateRequest) (model.TimeApplyResult, error)
	SyncTimeNow(ctx context.Context) (model.TimeSyncResult, error)
	SetManualSystemTime(ctx context.Context, request model.ManualTimeSetRequest) (model.ManualTimeSetResult, error)
	GetNetworkSettings(ctx context.Context) (model.NetworkSettings, error)
	GetNetworkRuntimeStatus(ctx context.Context) (model.NetworkRuntimeStatus, error)
	GetMqttSettings(ctx context.Context) (model.MqttSettings, error)
	GetMqttRuntimeStatus(ctx context.Context) (model.MqttRuntimeStatus, error)
	UpdateSystemSettings(ctx context.Context, request model.SystemSettingsUpdateRequest) (model.SystemSettingsUpdateResult, error)
	UpdateNetworkSettings(ctx context.Context, request model.NetworkSettingsUpdateRequest) (model.NetworkSettingsUpdateResult, error)
	SaveAndApplyNetworkSettings(ctx context.Context, request model.NetworkSettingsUpdateRequest) (model.NetworkApplyResult, error)
	UpdateMqttSettings(ctx context.Context, request model.MqttSettingsUpdateRequest) (model.MqttSettingsUpdateResult, error)
	ExportSystemConfig(ctx context.Context) (model.ConfigExportBundle, error)
	ImportSystemConfig(ctx context.Context, bundle model.ConfigExportBundle) (model.ConfigImportResult, error)
	ApplyNetworkSettings(ctx context.Context) (model.NetworkApplyResult, error)
	RequestFactoryReset(ctx context.Context) (model.FactoryResetResult, error)
	GetWebAuthStatus(ctx context.Context) (model.WebAuthStatus, error)
	VerifyWebLogin(ctx context.Context, request model.WebLoginRequest) (model.WebLoginResult, error)
	ChangeWebPassword(ctx context.Context, request model.WebPasswordChangeRequest) (model.WebPasswordChangeResult, error)
	SetWebViewerPassword(ctx context.Context, request model.WebViewerPasswordSetRequest) (model.WebPasswordChangeResult, error)
	ListWebUsers(ctx context.Context) ([]model.WebUser, error)
	CreateWebUser(ctx context.Context, request model.WebUserCreateRequest) (model.WebUserMutationResult, error)
	UpdateWebUser(ctx context.Context, request model.WebUserUpdateRequest) (model.WebUserMutationResult, error)
	ResetWebUserPassword(ctx context.Context, request model.WebUserPasswordResetRequest) (model.WebUserMutationResult, error)
}

// backendCollectionAPI 是采集配置、诊断与设备操作的协议边界。
type backendCollectionAPI interface {
	ListChannels(ctx context.Context) ([]model.ChannelConfig, error)
	CreateChannelConfig(ctx context.Context, request model.ChannelConfigUpdateRequest) (model.ChannelConfigUpdateResult, error)
	UpdateChannelConfig(ctx context.Context, request model.ChannelConfigUpdateRequest) (model.ChannelConfigUpdateResult, error)
	DeleteChannelConfig(ctx context.Context, channelID string) (model.ChannelConfigDeleteResult, error)
	GetChannelCommunicationTraces(ctx context.Context, query model.CommunicationTraceQuery) (model.ChannelCommunicationTraces, error)
	ClearChannelCommunicationTraces(ctx context.Context, channelID string) (model.CommunicationTraceClearResult, error)
	ExecuteDeviceCommand(ctx context.Context, request model.DeviceCommandExecuteRequest) (model.DeviceCommandExecuteResponse, error)
	ReadEM100EventRecord(ctx context.Context, deviceID string) (model.EM100RecordReadResponse, error)
	ReadEM100TestRecord(ctx context.Context, deviceID string) (model.EM100RecordReadResponse, error)
	ListSerialPorts(ctx context.Context) ([]model.SerialPortInfo, error)
	ListMasters(ctx context.Context) ([]model.MasterNodeConfig, error)
	CreateMasterConfig(ctx context.Context, request model.MasterConfigUpdateRequest) (model.MasterConfigUpdateResult, error)
	UpdateMasterConfig(ctx context.Context, request model.MasterConfigUpdateRequest) (model.MasterConfigUpdateResult, error)
	DeleteMasterConfig(ctx context.Context, masterID string) (model.MasterConfigDeleteResult, error)
	ListDevices(ctx context.Context) ([]model.DeviceConfig, error)
	UpdateDeviceDisplayName(ctx context.Context, request model.DeviceDisplayNameUpdateRequest) (model.DeviceConfig, error)
	UpdateDeviceDisplayNamesBatch(ctx context.Context, request model.DeviceDisplayNameBatchRequest) (model.DeviceDisplayNameBatchResult, error)
}

// backendUpdateAPI 只暴露升级状态机当前 Web 流程实际使用的入口。
type backendUpdateAPI interface {
	GetUpdateCurrentVersion(ctx context.Context) (model.UpdateVersion, error)
	GetUpdateStatus(ctx context.Context) (model.UpdateStatus, error)
	ImportApplicationUpgradePackage(ctx context.Context, uploadID string, packageIdentifier string) (model.UpdateStatus, error)
	StartUpdateJob(ctx context.Context, jobID string) (model.UpdateStatus, error)
}

type ConsoleService struct {
	backend                    backendAPI
	realtimeTemplateMu         sync.RWMutex
	realtimeTemplateGeneration uint64
	realtimeTemplates          []model.DeviceTemplateDefinition
}

type collectionObjectState string

const (
	collectionObjectNormal           collectionObjectState = "normal"
	collectionObjectIssue            collectionObjectState = "issue"
	collectionObjectUnknown          collectionObjectState = "unknown"
	collectionObjectDisabled         collectionObjectState = "disabled"
	collectionObjectConfigIncomplete collectionObjectState = "config_incomplete"
)

const (
	communicationTraceDefaultLimit = 10
	communicationTraceMaxLimit     = 50
)

func NewConsoleService(backend backendAPI) *ConsoleService {
	return &ConsoleService{backend: backend}
}

// WithTimeout 为控制台调用创建统一超时上下文。
func (s *ConsoleService) WithTimeout(parent context.Context) (context.Context, context.CancelFunc) {
	return s.backend.WithTimeout(parent)
}

func (s *ConsoleService) GetSystemStatus(ctx context.Context) (model.SystemStatus, error) {
	return s.backend.GetSystemStatus(ctx)
}

func (s *ConsoleService) GetSystemOverviewSnapshot(ctx context.Context) (model.SystemOverviewSnapshot, error) {
	return s.backend.GetSystemOverviewSnapshot(ctx)
}

func (s *ConsoleService) GetModbusServerPageSnapshot(ctx context.Context) (model.ModbusServerPageSnapshot, error) {
	return s.backend.GetModbusServerPageSnapshot(ctx)
}

func (s *ConsoleService) GetModbusServerRuntimeStatus(ctx context.Context) (model.ModbusServerRuntimeStatus, error) {
	return s.backend.GetModbusServerRuntimeStatus(ctx)
}

func (s *ConsoleService) ListModbusExportablePoints(ctx context.Context) ([]model.ModbusExportablePoint, error) {
	return s.backend.ListModbusExportablePoints(ctx)
}

func (s *ConsoleService) ListModbusRegisterMappings(ctx context.Context) ([]model.ModbusRegisterMapping, error) {
	return s.backend.ListModbusRegisterMappings(ctx)
}

func (s *ConsoleService) UpdateModbusServerSettings(ctx context.Context, request model.ModbusServerSettings) (model.ModbusServerSettings, error) {
	return s.backend.UpdateModbusServerSettings(ctx, request)
}

func (s *ConsoleService) CreateModbusRegisterMapping(ctx context.Context, request model.ModbusRegisterMappingRequest) (model.ModbusRegisterMapping, error) {
	return s.backend.CreateModbusRegisterMapping(ctx, request)
}

func (s *ConsoleService) UpdateModbusRegisterMapping(ctx context.Context, mappingID string, request model.ModbusRegisterMappingRequest) (model.ModbusRegisterMapping, error) {
	return s.backend.UpdateModbusRegisterMapping(ctx, mappingID, request)
}

func (s *ConsoleService) DeleteModbusRegisterMapping(ctx context.Context, mappingID string) error {
	return s.backend.DeleteModbusRegisterMapping(ctx, mappingID)
}

func (s *ConsoleService) GetPollingSummary(ctx context.Context) (model.PollingCycleSummary, error) {
	return s.backend.GetPollingSummary(ctx)
}

func (s *ConsoleService) GetRecentError(ctx context.Context) (model.ServiceErrorSummary, error) {
	return s.backend.GetRecentError(ctx)
}
