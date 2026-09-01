package model

// 本文件定义 Web 与后端 IPC 之间共享的数据传输模型；JSON 字段名需与 C++ IPC 协议保持兼容。

import (
	"bytes"
	"encoding/json"
	"strings"
)

// EventSourceLabel 将后端事件来源代码转换成页面和 CSV 共用的中文显示名。
func EventSourceLabel(source string) string {
	switch strings.ToLower(strings.TrimSpace(source)) {
	case "data_alarm":
		return "数据告警"
	case "alarm_ack":
		return "告警确认"
	case "channel":
		return "通道"
	case "master":
		return "主站"
	case "device":
		return "设备"
	case "system":
		return "系统"
	case "channel_startup":
		return "通道启动"
	case "polling_cycle", "polling":
		return "轮询采集"
	case "master_collect":
		return "主站采集"
	case "config_apply":
		return "配置应用"
	case "data_maintenance":
		return "数据维护"
	case "time_adjustment":
		return "时间校准"
	case "network", "network_config":
		return "网络配置"
	case "system_startup":
		return "系统启动"
	case "reload_config":
		return "配置重载"
	case "initialize":
		return "系统初始化"
	default:
		return "系统事件"
	}
}

// IPCRequest 是 Go Web 发给后端 IPC 的标准请求体。
type IPCRequest struct {
	ID     string      `json:"id"`
	Method string      `json:"method"`
	Params interface{} `json:"params,omitempty"`
}

// IPCError 描述后端 IPC 层返回的错误。
type IPCError struct {
	Code      string                 `json:"code"`
	Domain    string                 `json:"domain"`
	Message   string                 `json:"message"`
	Params    map[string]interface{} `json:"params"`
	Retryable bool                   `json:"retryable"`
}

// IPCResponse 对应后端 IPC 的统一响应结构。
type IPCResponse struct {
	ID      json.RawMessage `json:"id"`
	Success bool            `json:"success"`
	Result  json.RawMessage `json:"result"`
	Error   *IPCError       `json:"error,omitempty"`
}

// APIError 描述 Web 返回给浏览器的统一错误结构。
type APIError struct {
	Code      string                 `json:"code"`
	Domain    string                 `json:"domain"`
	Message   string                 `json:"message"`
	Params    map[string]interface{} `json:"params"`
	Retryable bool                   `json:"retryable"`
}

// APIResponse 是 Web HTTP API 的统一返回格式。
type APIResponse struct {
	Success bool        `json:"success"`
	Data    interface{} `json:"data,omitempty"`
	Error   *APIError   `json:"error,omitempty"`
}

// ConfigSummary 承接后端返回的系统配置摘要。
type ConfigSummary struct {
	ProjectName           string                     `json:"project_name"`
	ProjectVersion        string                     `json:"project_version"`
	SiteID                string                     `json:"site_id"`
	DefaultPollIntervalMS uint32                     `json:"default_poll_interval_ms"`
	ChannelCount          uint64                     `json:"channel_count"`
	MasterCount           uint64                     `json:"master_count"`
	DeviceCount           uint64                     `json:"device_count"`
	DeviceTemplates       []DeviceTemplateDefinition `json:"device_templates"`
}

type ReferencedMaster struct {
	MasterID   string `json:"master_id"`
	MasterName string `json:"master_name"`
}

type DeviceTemplateManagementItem struct {
	TemplateID              string                        `json:"template_id"`
	TemplateName            string                        `json:"template_name"`
	Description             string                        `json:"description"`
	DefaultStartRegister    uint32                        `json:"default_start_register"`
	StartRegisterText       string                        `json:"start_register_text"`
	DeviceAddressStride     int                           `json:"device_address_stride"`
	ReadBlocks              []DeviceTemplateReadBlock     `json:"read_blocks"`
	FieldCount              int                           `json:"field_count"`
	Builtin                 bool                          `json:"builtin"`
	TemplateKindText        string                        `json:"template_kind_text,omitempty"`
	TemplateTypeText        string                        `json:"template_type_text"`
	CapabilityText          string                        `json:"capability_text"`
	CapabilityClass         string                        `json:"capability_class"`
	Editable                bool                          `json:"editable"`
	Deletable               bool                          `json:"deletable"`
	ReadonlyReason          string                        `json:"readonly_reason"`
	Referenced              bool                          `json:"referenced"`
	ReferenceCount          int                           `json:"reference_count"`
	ReferencedMasters       []ReferencedMaster            `json:"referenced_masters"`
	Fields                  []DeviceTemplateField         `json:"fields"`
	WriteCommands           []DeviceTemplateWriteCommand  `json:"write_commands"`
	RealtimeGroupingEnabled bool                          `json:"realtime_grouping_enabled"`
	RealtimeGroups          []DeviceTemplateRealtimeGroup `json:"realtime_groups"`
	EditorDefinition        DeviceTemplateDefinition      `json:"-"`
}

type DeviceTemplateManagementView struct {
	TotalTemplates        int                            `json:"total_templates"`
	ReferencedTemplates   int                            `json:"referenced_templates"`
	UnreferencedTemplates int                            `json:"unreferenced_templates"`
	Templates             []DeviceTemplateManagementItem `json:"templates"`
}

type DeviceTemplateRealtimeDisplayRequest struct {
	TemplateID     string `json:"template_id"`
	FieldKey       string `json:"field_key"`
	ShowInRealtime bool   `json:"show_in_realtime"`
}

type DeviceTemplateRealtimeDisplayResult struct {
	Message            string                       `json:"message"`
	TemplateManagement DeviceTemplateManagementView `json:"template_management"`
}

type DeviceTemplateHistoryEnabledRequest struct {
	TemplateID     string `json:"template_id"`
	FieldKey       string `json:"field_key"`
	HistoryEnabled bool   `json:"history_enabled"`
}

type DeviceTemplateHistoryEnabledResult struct {
	Message            string                       `json:"message"`
	TemplateManagement DeviceTemplateManagementView `json:"template_management"`
}

// DeviceTemplateMutationResult 是新增、编辑和删除设备模板后的统一返回结构。
type DeviceTemplateMutationResult struct {
	Message            string                       `json:"message"`
	TemplateManagement DeviceTemplateManagementView `json:"template_management"`
}

// SystemSettings 是系统设置页可维护的基础系统标识。
type SystemSettings struct {
	DeviceName   string `json:"device_name"`
	SiteLocation string `json:"site_location"`
	DisplayName  string `json:"display_name"`
}

type SystemSettingsUpdateRequest struct {
	DeviceName   string `json:"device_name"`
	SiteLocation string `json:"site_location"`
	DisplayName  string `json:"display_name"`
}

type SystemSettingsUpdateResult struct {
	Settings SystemSettings `json:"settings"`
	Message  string         `json:"message"`
}

type TimeSettings struct {
	Timezone     string `json:"timezone"`
	NTPEnabled   bool   `json:"ntp_enabled"`
	NTPPrimary   string `json:"ntp_primary"`
	NTPSecondary string `json:"ntp_secondary"`
	UpdatedAt    uint64 `json:"updated_at"`
}

type TimeSettingsUpdateRequest struct {
	Timezone     string `json:"timezone"`
	NTPEnabled   bool   `json:"ntp_enabled"`
	NTPPrimary   string `json:"ntp_primary"`
	NTPSecondary string `json:"ntp_secondary"`
}

type TimeApplyResult struct {
	Settings       TimeSettings `json:"settings"`
	Message        string       `json:"message"`
	WarningMessage string       `json:"warning_message"`
}

type TimeRuntimeStatus struct {
	CurrentTimeMS        uint64 `json:"current_time_ms"`
	CurrentTimeText      string `json:"current_time_text"`
	Timezone             string `json:"timezone"`
	UTCOffsetText        string `json:"utc_offset_text"`
	NTPEnabled           bool   `json:"ntp_enabled"`
	NTPProcessRunning    bool   `json:"ntp_process_running"`
	NTPProcessState      string `json:"ntp_process_state"`
	NTPProcessStateText  string `json:"ntp_process_state_text"`
	SyncState            string `json:"sync_state"`
	SyncStateText        string `json:"sync_state_text"`
	RTCAvailable         bool   `json:"rtc_available"`
	RTCTimeText          string `json:"rtc_time_text"`
	LastSyncTimeMS       uint64 `json:"last_sync_time_ms"`
	LastErrorMessage     string `json:"last_error_message"`
	SettingsPendingApply bool   `json:"settings_pending_apply"`
}

type TimeSyncResult struct {
	Synchronized   bool   `json:"synchronized"`
	SyncTimeMS     uint64 `json:"sync_time_ms"`
	RTCWritten     bool   `json:"rtc_written"`
	Message        string `json:"message"`
	WarningMessage string `json:"warning_message"`
}

type ManualTimeSetRequest struct {
	EpochMS uint64 `json:"epoch_ms"`
	Source  string `json:"source"`
}

type ManualTimeSetResult struct {
	TimeSet        bool   `json:"time_set"`
	PreviousTimeMS uint64 `json:"previous_time_ms"`
	CurrentTimeMS  uint64 `json:"current_time_ms"`
	RTCWritten     bool   `json:"rtc_written"`
	Message        string `json:"message"`
	WarningMessage string `json:"warning_message"`
}

// NetworkSettings 是系统设置页维护的单网口 IPv4 配置。
type NetworkSettings struct {
	Mode          string   `json:"mode"`
	InterfaceName string   `json:"interface_name"`
	IPAddress     string   `json:"ip_address"`
	Netmask       string   `json:"netmask"`
	Gateway       string   `json:"gateway"`
	DNSServers    []string `json:"dns_servers"`
	ApplyModeText string   `json:"apply_mode_text"`
}

type NetworkSettingsUpdateRequest struct {
	Mode          string   `json:"mode"`
	InterfaceName string   `json:"interface_name"`
	IPAddress     string   `json:"ip_address"`
	Netmask       string   `json:"netmask"`
	Gateway       string   `json:"gateway"`
	DNSServers    []string `json:"dns_servers"`
}

type NetworkSettingsUpdateResult struct {
	Settings NetworkSettings `json:"settings"`
	Message  string          `json:"message"`
}

type NetworkApplyResult struct {
	Applied          bool                 `json:"applied"`
	Mode             string               `json:"mode"`
	InterfaceName    string               `json:"interface_name"`
	IPAddress        string               `json:"ip_address"`
	CurrentIPAddress string               `json:"current_ip_address"`
	ErrorMessage     string               `json:"error_message"`
	RuntimeStatus    NetworkRuntimeStatus `json:"runtime_status"`
	Message          string               `json:"message"`
}

// NetworkRuntimeStatus 是 Linux 当前真实网口运行状态的只读快照。
type NetworkRuntimeStatus struct {
	ConfiguredMode    string `json:"configured_mode"`
	InterfaceName     string `json:"interface_name"`
	InterfaceExists   bool   `json:"interface_exists"`
	Operstate         string `json:"operstate"`
	LinkState         string `json:"link_state"`
	LinkStateText     string `json:"link_state_text"`
	IPAddress         string `json:"ip_address"`
	DefaultGateway    string `json:"default_gateway"`
	DHCPClientRunning bool   `json:"dhcp_client_running"`
	Message           string `json:"message"`
}

// MqttSettings 是 MQTT 北向发布的独立配置。后端不会回显 Password。
type MqttSettings struct {
	Enabled                bool   `json:"enabled"`
	BrokerHost             string `json:"broker_host"`
	BrokerPort             uint16 `json:"broker_port"`
	ClientID               string `json:"client_id"`
	NodeID                 string `json:"node_id"`
	Username               string `json:"username"`
	Password               string `json:"password,omitempty"`
	TopicPrefix            string `json:"topic_prefix"`
	PublishIntervalSeconds uint32 `json:"publish_interval_seconds"`
	QoS                    int    `json:"qos"`
	RetainStatus           bool   `json:"retain_status"`
	KeepAliveSeconds       uint32 `json:"keep_alive_seconds"`
	TLSEnabled             bool   `json:"tls_enabled"`
	TLSCAFile              string `json:"tls_ca_file"`
	TLSClientCertFile      string `json:"tls_client_cert_file"`
	TLSClientKeyFile       string `json:"tls_client_key_file"`
	TLSInsecure            bool   `json:"tls_insecure"`
}

type MqttSettingsUpdateRequest struct {
	Enabled                bool   `json:"enabled"`
	BrokerHost             string `json:"broker_host"`
	BrokerPort             uint16 `json:"broker_port"`
	ClientID               string `json:"client_id"`
	NodeID                 string `json:"node_id"`
	Username               string `json:"username"`
	Password               string `json:"password,omitempty"`
	TopicPrefix            string `json:"topic_prefix"`
	PublishIntervalSeconds uint32 `json:"publish_interval_seconds"`
	QoS                    int    `json:"qos"`
	RetainStatus           bool   `json:"retain_status"`
	KeepAliveSeconds       uint32 `json:"keep_alive_seconds"`
	ClearPassword          bool   `json:"clear_password,omitempty"`
	TLSEnabled             bool   `json:"tls_enabled"`
	TLSCAFile              string `json:"tls_ca_file"`
	TLSClientCertFile      string `json:"tls_client_cert_file"`
	TLSClientKeyFile       string `json:"tls_client_key_file"`
	TLSInsecure            bool   `json:"tls_insecure"`
}

type MqttSettingsUpdateResult struct {
	Settings   MqttSettings `json:"settings"`
	Message    string       `json:"message"`
	Applied    bool         `json:"applied"`
	ApplyError string       `json:"apply_error"`
}

// ConfigBundleMqttSettings 是配置导出文件中的 MQTT 基础参数，不包含密码和证书内容。
type ConfigBundleMqttSettings struct {
	Enabled                bool   `json:"enabled"`
	BrokerHost             string `json:"broker_host"`
	BrokerPort             uint16 `json:"broker_port"`
	ClientID               string `json:"client_id"`
	NodeID                 string `json:"node_id"`
	Username               string `json:"username"`
	TopicPrefix            string `json:"topic_prefix"`
	PublishIntervalSeconds uint32 `json:"publish_interval_seconds"`
	QoS                    int    `json:"qos"`
	RetainStatus           bool   `json:"retain_status"`
	KeepAliveSeconds       uint32 `json:"keep_alive_seconds"`
	TLSEnabled             bool   `json:"tls_enabled"`
	TLSCAFile              string `json:"tls_ca_file"`
	TLSClientCertFile      string `json:"tls_client_cert_file"`
	TLSClientKeyFile       string `json:"tls_client_key_file"`
	TLSInsecure            bool   `json:"tls_insecure"`
	PasswordExported       bool   `json:"password_exported"`
}

type ConfigExportBundle struct {
	Format                 string                     `json:"format"`
	Version                uint32                     `json:"version"`
	ExportedAtMS           uint64                     `json:"exported_at_ms"`
	SystemSettings         SystemSettings             `json:"system_settings"`
	TimeSettings           *TimeSettings              `json:"time_settings,omitempty"`
	NetworkSettings        NetworkSettings            `json:"network_settings"`
	MqttSettings           ConfigBundleMqttSettings   `json:"mqtt_settings"`
	CustomDeviceTypes      []DeviceTemplateDefinition `json:"custom_device_types"`
	Channels               []ChannelConfig            `json:"channels"`
	Masters                []MasterNodeConfig         `json:"masters"`
	AlarmRulesIncluded     bool                       `json:"alarm_rules_included"`
	AlarmRules             []AlarmRule                `json:"alarm_rules"`
	ModbusServerSettings   ModbusServerSettings       `json:"modbus_server_settings,omitempty"`
	ModbusRegisterMappings []ModbusRegisterMapping    `json:"modbus_register_mappings"`
}

// UnmarshalJSON 将配置包字段 value_offset 归一到 Web 页面模型使用的 Offset。
func (bundle *ConfigExportBundle) UnmarshalJSON(data []byte) error {
	var root map[string]json.RawMessage
	if err := json.Unmarshal(data, &root); err != nil {
		return err
	}
	if rawTemplates, ok := root["custom_device_types"]; ok {
		var templates []map[string]json.RawMessage
		if err := json.Unmarshal(rawTemplates, &templates); err == nil {
			for _, template := range templates {
				var fields []map[string]json.RawMessage
				if err := json.Unmarshal(template["fields"], &fields); err != nil {
					continue
				}
				for _, field := range fields {
					if valueOffset, hasValueOffset := field["value_offset"]; hasValueOffset {
						field["offset"] = valueOffset
						delete(field, "value_offset")
					}
				}
				template["fields"], _ = json.Marshal(fields)
			}
			root["custom_device_types"], _ = json.Marshal(templates)
		}
	}
	normalized, err := json.Marshal(root)
	if err != nil {
		return err
	}
	type bundleAlias ConfigExportBundle
	var decoded bundleAlias
	decoder := json.NewDecoder(bytes.NewReader(normalized))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&decoded); err != nil {
		return err
	}
	*bundle = ConfigExportBundle(decoded)
	return nil
}

type ConfigImportResult struct {
	Imported         bool   `json:"imported"`
	Message          string `json:"message"`
	PollingRestarted bool   `json:"polling_restarted"`
}

// MqttRuntimeStatus 是 MQTT 发布服务当前运行状态的只读快照。
type MqttRuntimeStatus struct {
	Enabled                 bool   `json:"enabled"`
	Connected               bool   `json:"connected"`
	State                   string `json:"state"`
	BrokerEndpoint          string `json:"broker_endpoint"`
	TLSEnabled              bool   `json:"tls_enabled"`
	TLSInsecure             bool   `json:"tls_insecure"`
	StatusTopic             string `json:"status_topic"`
	RealtimeTopic           string `json:"realtime_topic"`
	EventTopic              string `json:"event_topic"`
	AlarmTopic              string `json:"alarm_topic"`
	LastErrorMessage        string `json:"last_error_message"`
	LastConnectTimeMS       uint64 `json:"last_connect_time_ms"`
	LastDisconnectTimeMS    uint64 `json:"last_disconnect_time_ms"`
	LastPublishTimeMS       uint64 `json:"last_publish_time_ms"`
	LastPublishTopic        string `json:"last_publish_topic"`
	LastPublishPayloadBytes uint64 `json:"last_publish_payload_bytes"`
	LastPublishErrorMessage string `json:"last_publish_error_message"`
	LastPublishSequence     uint64 `json:"last_publish_sequence"`
	LastEventPublishTimeMS  uint64 `json:"last_event_publish_time_ms"`
	LastAlarmPublishTimeMS  uint64 `json:"last_alarm_publish_time_ms"`
	LastEventPublishTopic   string `json:"last_event_publish_topic"`
	LastAlarmPublishTopic   string `json:"last_alarm_publish_topic"`
	EventPublishCount       uint64 `json:"event_publish_count"`
	AlarmPublishCount       uint64 `json:"alarm_publish_count"`
	PublishedMessageCount   uint64 `json:"published_message_count"`
	FailedPublishCount      uint64 `json:"failed_publish_count"`
}

type SystemHealthSummary struct {
	Level   string `json:"level"`
	Message string `json:"message"`
}

type SystemProcessMetrics struct {
	Available            bool    `json:"available"`
	ErrorMessage         string  `json:"error_message"`
	SampledAtMS          uint64  `json:"sampled_at_ms"`
	BackendUptimeSeconds uint64  `json:"backend_uptime_seconds"`
	VmRSSKB              uint64  `json:"vm_rss_kb"`
	VmSizeKB             uint64  `json:"vm_size_kb"`
	ThreadCount          uint32  `json:"thread_count"`
	OpenFDCount          uint32  `json:"open_fd_count"`
	LoadAverageAvailable bool    `json:"load_average_available"`
	LoadAverage1M        float64 `json:"load_average_1m"`
	LoadAverage5M        float64 `json:"load_average_5m"`
	LoadAverage15M       float64 `json:"load_average_15m"`
}

type DatabaseStorageMetrics struct {
	Role         string `json:"role"`
	Path         string `json:"path"`
	SizeBytes    uint64 `json:"size_bytes"`
	WALSizeBytes uint64 `json:"wal_size_bytes"`
	SHMSizeBytes uint64 `json:"shm_size_bytes"`
}

type SystemStorageMetrics struct {
	Available                bool                     `json:"available"`
	ErrorMessage             string                   `json:"error_message"`
	StoragePath              string                   `json:"storage_path"`
	DataDirectorySizeBytes   uint64                   `json:"data_directory_size_bytes"`
	SQLiteDatabaseSizeBytes  uint64                   `json:"sqlite_database_size_bytes"`
	SQLiteAuxiliarySizeBytes uint64                   `json:"sqlite_auxiliary_size_bytes"`
	Databases                []DatabaseStorageMetrics `json:"databases"`
	StorageTotalBytes        uint64                   `json:"storage_total_bytes"`
	StorageFreeBytes         uint64                   `json:"storage_free_bytes"`
	StorageAvailableBytes    uint64                   `json:"storage_available_bytes"`
	StorageUsedPercent       float64                  `json:"storage_used_percent"`
}

type SystemOverviewSnapshot struct {
	GeneratedAtMS       uint64                    `json:"generated_at_ms"`
	Health              SystemHealthSummary       `json:"health"`
	Process             SystemProcessMetrics      `json:"process"`
	Storage             SystemStorageMetrics      `json:"storage"`
	SystemStatus        SystemStatus              `json:"system_status"`
	Polling             PollingCycleSummary       `json:"polling"`
	CurrentError        ServiceErrorSummary       `json:"current_error"`
	MqttRuntime         MqttRuntimeStatus         `json:"mqtt_runtime"`
	ModbusServerRuntime ModbusServerRuntimeStatus `json:"modbus_server_runtime"`
}

// OverviewPageSnapshot 是系统概览 HTML 首屏的一次性聚合响应。
type OverviewPageSnapshot struct {
	SystemSettings         SystemSettings         `json:"system_settings"`
	ConfigSummary          ConfigSummary          `json:"config_summary"`
	SystemOverviewSnapshot SystemOverviewSnapshot `json:"system_overview_snapshot"`
	RecentEvents           []ServiceEvent         `json:"recent_events"`
	ActiveAlarms           []ActiveAlarm          `json:"active_alarms"`
	Channels               []ChannelConfig        `json:"channels"`
	Masters                []MasterNodeConfig     `json:"masters"`
	Devices                []DeviceConfig         `json:"devices"`
}

type FactoryResetResult struct {
	ResetCompleted bool   `json:"reset_completed"`
	Message        string `json:"message"`
}

type WebAuthStatus struct {
	Username                  string `json:"username"`
	PasswordChangeRecommended bool   `json:"password_change_recommended"`
	AuthInitialized           bool   `json:"auth_initialized"`
}

type FirstBootAdminEntryStatus struct {
	Available bool `json:"available"`
}

type FirstBootAdminEntryResult struct {
	Consumed bool `json:"consumed"`
}

type WebLoginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type WebLoginResult struct {
	Success                   bool   `json:"success"`
	Username                  string `json:"username"`
	Role                      string `json:"role"`
	PasswordChangeRecommended bool   `json:"password_change_recommended"`
	Message                   string `json:"message"`
}

type WebPasswordChangeRequest struct {
	Username        string `json:"username"`
	CurrentPassword string `json:"current_password"`
	NewPassword     string `json:"new_password"`
}

type WebViewerPasswordSetRequest struct {
	AdminPassword   string `json:"admin_password"`
	NewUserPassword string `json:"new_user_password"`
}

type WebPasswordChangeResult struct {
	Success                   bool   `json:"success"`
	Message                   string `json:"message"`
	PasswordChangeRecommended bool   `json:"password_change_recommended"`
}

type WebUser struct {
	Username                  string `json:"username"`
	DisplayName               string `json:"display_name"`
	Role                      string `json:"role"`
	Enabled                   bool   `json:"enabled"`
	CreatedAtMS               uint64 `json:"created_at_ms"`
	UpdatedAtMS               uint64 `json:"updated_at_ms"`
	LastLoginAtMS             uint64 `json:"last_login_at_ms"`
	PasswordChangeRecommended bool   `json:"password_change_recommended"`
}

type WebUserCreateRequest struct {
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Role        string `json:"role"`
	Password    string `json:"password"`
	Enabled     bool   `json:"enabled"`
}

type WebUserUpdateRequest struct {
	Username    string `json:"username"`
	DisplayName string `json:"display_name"`
	Role        string `json:"role"`
	Enabled     bool   `json:"enabled"`
}

type WebUserPasswordResetRequest struct {
	Username    string `json:"username"`
	NewPassword string `json:"new_password"`
}

type WebUserMutationResult struct {
	Success bool    `json:"success"`
	Message string  `json:"message"`
	User    WebUser `json:"user"`
}

// PollingCycleSummary 表示最近一次轮询周期的摘要信息。
type PollingCycleSummary struct {
	PollingRunning        bool            `json:"polling_running"`
	PollingState          string          `json:"polling_state"`
	LastCycleStartedAtMS  uint64          `json:"last_cycle_started_at_ms"`
	LastCycleFinishedAtMS uint64          `json:"last_cycle_finished_at_ms"`
	LastCycleMasterCount  uint64          `json:"last_cycle_master_count"`
	SuccessMasterCount    uint64          `json:"last_cycle_success_master_count"`
	FailedMasterCount     uint64          `json:"last_cycle_failed_master_count"`
	SuccessDeviceCount    uint64          `json:"last_cycle_success_device_count"`
	FailedDeviceCount     uint64          `json:"last_cycle_failed_device_count"`
	LastCycleHasError     bool            `json:"last_cycle_has_error"`
	Diagnosis             DiagnosisStatus `json:"diagnosis"`
	LastCycleErrorMessage string          `json:"last_cycle_error_message"`
	LastHeartbeatMS       uint64          `json:"last_heartbeat_ms"`
}

// DiagnosisStatus 是后端统一维护的采集诊断状态。
type DiagnosisStatus struct {
	Level               string `json:"level"`
	TargetID            string `json:"target_id"`
	TargetName          string `json:"target_name"`
	Status              string `json:"status"`
	ErrorCode           string `json:"error_code"`
	Message             string `json:"message"`
	Suggestion          string `json:"suggestion"`
	LastSuccessTimeMS   uint64 `json:"last_success_time_ms"`
	LastErrorTimeMS     uint64 `json:"last_error_time_ms"`
	ConsecutiveFailures uint32 `json:"consecutive_failures"`
}

// ServiceErrorSummary 保存最近一次可供页面直接展示的服务错误。
type ServiceErrorSummary struct {
	HasError    bool            `json:"has_error"`
	Source      string          `json:"source"`
	TargetID    string          `json:"target_id"`
	Diagnosis   DiagnosisStatus `json:"diagnosis"`
	Message     string          `json:"message"`
	TimestampMS uint64          `json:"timestamp_ms"`
}

// ServiceEvent 是后端持久化的关键事件。
type ServiceEvent struct {
	EventID          string          `json:"event_id"`
	FirstTimestampMS uint64          `json:"first_timestamp_ms"`
	TimestampMS      uint64          `json:"timestamp_ms"`
	Level            string          `json:"level"`
	Source           string          `json:"source"`
	TargetID         string          `json:"target_id"`
	Diagnosis        DiagnosisStatus `json:"diagnosis"`
	Summary          string          `json:"summary"`
	Detail           string          `json:"detail"`
	OccurrenceCount  uint64          `json:"occurrence_count"`
}

// EventHistoryQuery 是历史事件页传给后端的筛选与分页条件。
type EventHistoryQuery struct {
	Level     string `json:"level,omitempty"`
	Source    string `json:"source,omitempty"`
	TimeRange string `json:"time_range,omitempty"`
	Search    string `json:"search,omitempty"`
	Page      int    `json:"page"`
	PageSize  int    `json:"page_size"`
}

type EventLevelStats struct {
	Error   int `json:"error"`
	Warning int `json:"warning"`
	Info    int `json:"info"`
}

type EventSourceStat struct {
	Source string `json:"source"`
	Count  int    `json:"count"`
}

type EventHistoryResult struct {
	Rows        []ServiceEvent    `json:"rows"`
	Total       int               `json:"total"`
	LevelStats  EventLevelStats   `json:"level_stats"`
	SourceStats []EventSourceStat `json:"source_stats"`
}

// EventExportQuery 是历史事件 CSV 导出时传给后端的分页筛选条件。
type EventExportQuery struct {
	Level     string `json:"level,omitempty"`
	Source    string `json:"source,omitempty"`
	TimeRange string `json:"time_range,omitempty"`
	Search    string `json:"search,omitempty"`
	Limit     int    `json:"limit,omitempty"`
	Offset    int    `json:"offset,omitempty"`
}

// AlarmRule 承接后端设备数据项级高低限告警规则。
type AlarmRule struct {
	DeviceID      string  `json:"device_id"`
	PointKey      string  `json:"point_key"`
	Enabled       bool    `json:"enabled"`
	HighEnabled   bool    `json:"high_enabled"`
	HighThreshold float64 `json:"high_threshold"`
	LowEnabled    bool    `json:"low_enabled"`
	LowThreshold  float64 `json:"low_threshold"`
	Level         string  `json:"level"`
	Hysteresis    float64 `json:"hysteresis"`
	TriggerCount  uint32  `json:"trigger_count"`
	RecoveryCount uint32  `json:"recovery_count"`
	UpdatedAtMS   uint64  `json:"updated_at_ms"`
}

type AlarmRuleUpsertResult struct {
	Message string    `json:"message"`
	Rule    AlarmRule `json:"rule"`
}

type AlarmRuleDeleteResult struct {
	Message string `json:"message"`
}

// MasterAlarmRuleRequest 是 Web 层按“主站 + 数据项”批量展开到设备规则的请求。
type MasterAlarmRuleRequest struct {
	MasterID      string
	PointKey      string
	Enabled       bool
	HighEnabled   bool
	HighThreshold float64
	LowEnabled    bool
	LowThreshold  float64
	Level         string
	Hysteresis    float64
	TriggerCount  uint32
	RecoveryCount uint32
}

// ActiveAlarm 承接后端当前活动告警展示视图。
type ActiveAlarm struct {
	DeviceID          string  `json:"device_id"`
	DeviceName        string  `json:"device_name"`
	MasterID          string  `json:"master_id"`
	TemplateID        string  `json:"template_id"`
	PointKey          string  `json:"point_key"`
	PointName         string  `json:"point_name"`
	Unit              string  `json:"unit"`
	Precision         uint32  `json:"precision"`
	Direction         string  `json:"direction"`
	Level             string  `json:"level"`
	CurrentValue      float64 `json:"current_value"`
	ThresholdValue    float64 `json:"threshold_value"`
	ActiveSinceMS     uint64  `json:"active_since_ms"`
	LastEvaluatedAtMS uint64  `json:"last_evaluated_at_ms"`
	Acknowledged      bool    `json:"acknowledged"`
	AcknowledgedAtMS  uint64  `json:"acknowledged_at_ms"`
	AcknowledgedBy    string  `json:"acknowledged_by"`
}

// ActiveAlarmAcknowledgeRequest 是活动告警确认 IPC 请求。
type ActiveAlarmAcknowledgeRequest struct {
	DeviceID       string  `json:"device_id"`
	PointKey       string  `json:"point_key"`
	AcknowledgedBy string  `json:"acknowledged_by"`
	ActiveSinceMS  *uint64 `json:"active_since_ms,omitempty"`
}

// ActiveAlarmAcknowledgeResult 返回确认后的活动告警快照。
type ActiveAlarmAcknowledgeResult struct {
	Message string      `json:"message"`
	Alarm   ActiveAlarm `json:"alarm"`
}

// PointValue 表示一个可展示的数据项值。
type PointValue struct {
	Key            string  `json:"key"`
	Name           string  `json:"name"`
	Value          float64 `json:"value"`
	Unit           string  `json:"unit"`
	Precision      uint32  `json:"precision"`
	Summary        bool    `json:"summary"`
	HistoryEnabled bool    `json:"history_enabled"`
	DisplayOrder   int     `json:"display_order"`
	Quality        string  `json:"quality"`
	Valid          bool    `json:"valid"`
	RawValue       float64 `json:"raw_value"`
	DisplayText    string  `json:"display_text"`
	Message        string  `json:"message"`
	SampleTimeMS   uint64  `json:"sample_time_ms"`
}

// SerialPortInfo 是后端串口发现接口返回的单个候选项。
type SerialPortInfo struct {
	Path          string `json:"path"`
	Name          string `json:"name"`
	Kind          string `json:"kind"`
	DisplayName   string `json:"display_name"`
	Available     bool   `json:"available"`
	Busy          bool   `json:"busy"`
	Description   string `json:"description"`
	SymlinkByID   string `json:"symlink_by_id"`
	SymlinkByPath string `json:"symlink_by_path"`
	Driver        string `json:"driver"`
	PhysicalHint  string `json:"physical_hint"`
}

// ChannelConfig 承接后端通道配置。
// RTU 使用 `device_path` / `port_name`，TCP 使用 `tcp_host` / `tcp_port`。
type ChannelConfig struct {
	ChannelID         string `json:"channel_id"`
	ChannelName       string `json:"channel_name"`
	Enabled           bool   `json:"enabled"`
	ChannelType       string `json:"channel_type"`
	DevicePath        string `json:"device_path"`
	PortName          string `json:"port_name"`
	TCPHost           string `json:"tcp_host"`
	TCPPort           uint16 `json:"tcp_port"`
	ConnectTimeoutMS  uint32 `json:"connect_timeout_ms"`
	BaudRate          uint32 `json:"baud_rate"`
	DataBits          uint8  `json:"data_bits"`
	Parity            string `json:"parity"`
	StopBits          uint8  `json:"stop_bits"`
	ResponseTimeoutMS uint32 `json:"response_timeout_ms"`
	RetryCount        uint32 `json:"retry_count"`
}

// ChannelConfigUpdateRequest 是页面提交单个通道配置时使用的请求模型。
type ChannelConfigUpdateRequest struct {
	ChannelID         string `json:"channel_id"`
	ChannelName       string `json:"channel_name"`
	Enabled           bool   `json:"enabled"`
	ChannelType       string `json:"channel_type"`
	PortName          string `json:"port_name"`
	TCPHost           string `json:"tcp_host"`
	TCPPort           uint16 `json:"tcp_port"`
	ConnectTimeoutMS  uint32 `json:"connect_timeout_ms"`
	BaudRate          uint32 `json:"baud_rate"`
	DataBits          uint8  `json:"data_bits"`
	Parity            string `json:"parity"`
	StopBits          uint8  `json:"stop_bits"`
	ResponseTimeoutMS uint32 `json:"response_timeout_ms"`
	RetryCount        uint32 `json:"retry_count"`
}

// ChannelConfigUpdateResult 描述后端保存并重载通道配置后的结果。
type ChannelConfigUpdateResult struct {
	ChannelID        string        `json:"channel_id"`
	ChannelConfig    ChannelConfig `json:"channel_config"`
	Message          string        `json:"message"`
	WarningMessage   string        `json:"warning_message"`
	PollingRestarted bool          `json:"polling_restarted"`
}

type ChannelConfigDeleteResult struct {
	ChannelID        string `json:"channel_id"`
	Message          string `json:"message"`
	PollingRestarted bool   `json:"polling_restarted"`
}

// MasterNodeConfig 承接主站配置。
type MasterNodeConfig struct {
	MasterID           string `json:"master_id"`
	MasterName         string `json:"master_name"`
	Enabled            bool   `json:"enabled"`
	Protocol           string `json:"protocol"`
	ChannelID          string `json:"channel_id"`
	TargetAddress      uint16 `json:"target_address"`
	PollIntervalMS     uint32 `json:"poll_interval_ms"`
	ResponseTimeoutMS  uint32 `json:"response_timeout_ms"`
	RetryCount         uint32 `json:"retry_count"`
	Remark             string `json:"remark"`
	DeviceTemplate     string `json:"device_template"`
	BlockStartRegister uint16 `json:"block_start_register"`
	DeviceCount        int    `json:"device_count"`
}

type MasterConfigUpdateRequest struct {
	MasterID           string `json:"master_id"`
	MasterName         string `json:"master_name"`
	Enabled            bool   `json:"enabled"`
	Protocol           string `json:"protocol"`
	ChannelID          string `json:"channel_id"`
	TargetAddress      uint16 `json:"target_address"`
	PollIntervalMS     uint32 `json:"poll_interval_ms"`
	ResponseTimeoutMS  uint32 `json:"response_timeout_ms"`
	RetryCount         uint32 `json:"retry_count"`
	Remark             string `json:"remark"`
	DeviceTemplate     string `json:"device_template"`
	BlockStartRegister uint16 `json:"block_start_register"`
	DeviceCount        int    `json:"device_count"`
}

type MasterConfigUpdateResult struct {
	MasterID         string           `json:"master_id"`
	MasterConfig     MasterNodeConfig `json:"master_config"`
	Message          string           `json:"message"`
	WarningMessage   string           `json:"warning_message"`
	PollingRestarted bool             `json:"polling_restarted"`
}

type MasterConfigDeleteResult struct {
	MasterID         string `json:"master_id"`
	Message          string `json:"message"`
	PollingRestarted bool   `json:"polling_restarted"`
}

// DeviceConfig 承接设备配置。
type DeviceConfig struct {
	DeviceID       string `json:"device_id"`
	SystemName     string `json:"system_name"`
	DeviceName     string `json:"device_name"`
	MasterID       string `json:"master_id"`
	Enabled        bool   `json:"enabled"`
	RegisterOffset uint16 `json:"register_offset"`
}

type DeviceDisplayNameUpdateRequest struct {
	DeviceID    string `json:"device_id"`
	DisplayName string `json:"display_name"`
}

type DeviceDisplayNameBatchItem struct {
	DeviceID    string `json:"device_id"`
	DisplayName string `json:"display_name"`
}

type DeviceDisplayNameBatchRequest struct {
	Items []DeviceDisplayNameBatchItem `json:"items"`
}

type DeviceDisplayNameBatchFailure struct {
	DeviceID string `json:"device_id"`
	Message  string `json:"message"`
}

type DeviceDisplayNameBatchResult struct {
	SuccessCount int                             `json:"success_count"`
	FailureCount int                             `json:"failure_count"`
	Failures     []DeviceDisplayNameBatchFailure `json:"failures"`
}

// ChannelStatus 表示通道运行状态。
type ChannelStatus struct {
	ChannelID             string          `json:"channel_id"`
	Configured            bool            `json:"configured"`
	Enabled               bool            `json:"enabled"`
	Opened                bool            `json:"opened"`
	Status                string          `json:"status"`
	DevicePath            string          `json:"device_path"`
	LastOpenTimeMS        uint64          `json:"last_open_time_ms"`
	LastCloseTimeMS       uint64          `json:"last_close_time_ms"`
	LastSendTimeMS        uint64          `json:"last_send_time_ms"`
	LastReceiveTimeMS     uint64          `json:"last_receive_time_ms"`
	LastChangeTimeMS      uint64          `json:"last_change_time_ms"`
	ConsecutiveErrorCount uint32          `json:"consecutive_error_count"`
	Diagnosis             DiagnosisStatus `json:"diagnosis"`
	LastErrorMessage      string          `json:"last_error_message"`
}

// MasterNodeStatus 表示主站运行状态。
type MasterNodeStatus struct {
	MasterID                string          `json:"master_id"`
	Online                  bool            `json:"online"`
	LastCollectSuccess      bool            `json:"last_collect_success"`
	CommunicationQuality    string          `json:"communication_quality"`
	LastPollTimeMS          uint64          `json:"last_poll_time_ms"`
	LastSuccessTimeMS       uint64          `json:"last_success_time_ms"`
	LastFailureTimeMS       uint64          `json:"last_failure_time_ms"`
	LastCycleDurationMS     uint64          `json:"last_cycle_duration_ms"`
	ConsecutiveFailureCount uint32          `json:"consecutive_failure_count"`
	ConsecutiveTimeoutCount uint32          `json:"consecutive_timeout_count"`
	LastRegisterBlock       []uint16        `json:"last_register_block"`
	Diagnosis               DiagnosisStatus `json:"diagnosis"`
	LastErrorMessage        string          `json:"last_error_message"`
}

// DeviceStatus 表示设备运行状态。
type DeviceStatus struct {
	DeviceID             string          `json:"device_id"`
	DeviceName           string          `json:"device_name"`
	MasterID             string          `json:"master_id"`
	Online               bool            `json:"online"`
	LastCollectSuccess   bool            `json:"last_collect_success"`
	CommunicationQuality string          `json:"communication_quality"`
	UpdatedAtMS          uint64          `json:"updated_at_ms"`
	LastSuccessTimeMS    uint64          `json:"last_success_time_ms"`
	LastFailureTimeMS    uint64          `json:"last_failure_time_ms"`
	HasResistance        bool            `json:"has_resistance"`
	ResistanceValue      float64         `json:"resistance_value"`
	Points               []PointValue    `json:"points"`
	Diagnosis            DiagnosisStatus `json:"diagnosis"`
	LastErrorMessage     string          `json:"last_error_message"`
}

// DeviceRealtimeSnapshot 表示单个设备的实时采样快照。
type DeviceRealtimeSnapshot struct {
	DeviceID             string       `json:"device_id"`
	DeviceName           string       `json:"device_name"`
	MasterID             string       `json:"master_id"`
	TemplateID           string       `json:"template_id"`
	TemplateName         string       `json:"template_name"`
	SampleTimeMS         uint64       `json:"sample_time_ms"`
	CommunicationQuality string       `json:"communication_quality"`
	Points               []PointValue `json:"points"`
	HasResistance        bool         `json:"has_resistance"`
	Resistance           *PointValue  `json:"resistance"`
}

// HistoryRecord 表示后端保存的设备数据项级趋势值。
type HistoryRecord struct {
	DeviceID      string  `json:"device_id"`
	MasterID      string  `json:"master_id"`
	ChannelID     string  `json:"channel_id"`
	TemplateID    string  `json:"template_id"`
	SamplePeriod  string  `json:"sample_period"`
	BucketStartMS uint64  `json:"bucket_start_ms"`
	BucketText    string  `json:"bucket_text"`
	Date          string  `json:"date"`
	PointKey      string  `json:"point_key"`
	PointName     string  `json:"point_name"`
	Unit          string  `json:"unit"`
	Precision     uint32  `json:"precision"`
	Value         float64 `json:"value"`
	RawValue      float64 `json:"raw_value"`
	TimestampMS   uint64  `json:"timestamp_ms"`
	Quality       string  `json:"quality"`
	Valid         bool    `json:"valid"`
	Message       string  `json:"message"`
}

type DataMaintenanceSummary struct {
	Raw10MinRetentionHours uint32 `json:"raw_10min_retention_hours"`
	HourRetentionDays      uint32 `json:"hour_retention_days"`
	DayRetentionDays       uint32 `json:"day_retention_days"`
	EventRetentionDays     uint32 `json:"event_retention_days"`
	LogRetentionDays       uint32 `json:"log_retention_days"`
	LogRotationPolicy      string `json:"log_rotation_policy"`
	LastCleanupTimeMS      uint64 `json:"last_cleanup_time_ms"`
	LastCleanupSuccess     bool   `json:"last_cleanup_success"`
	LastCleanupResult      string `json:"last_cleanup_result"`
	DeletedRaw10MinCount   uint64 `json:"deleted_raw_10min_count"`
	DeletedHourCount       uint64 `json:"deleted_hour_count"`
	DeletedDayCount        uint64 `json:"deleted_day_count"`
	DeletedEventCount      uint64 `json:"deleted_event_count"`
	CurrentRaw10MinCount   uint64 `json:"current_raw_10min_count"`
	CurrentHourCount       uint64 `json:"current_hour_count"`
	CurrentDayCount        uint64 `json:"current_day_count"`
	CurrentEventCount      uint64 `json:"current_event_count"`
}

type DeviceHistoryQuery struct {
	DeviceID     string `json:"device_id"`
	Days         uint32 `json:"days,omitempty"`
	StartDate    string `json:"start_date,omitempty"`
	EndDate      string `json:"end_date,omitempty"`
	PointKey     string `json:"point_key,omitempty"`
	SamplePeriod string `json:"sample_period,omitempty"`
}

// HistoryExportQuery 是历史数据 CSV 导出时传给后端的分页筛选条件。
type HistoryExportQuery struct {
	ChannelID    string `json:"channel_id,omitempty"`
	MasterID     string `json:"master_id,omitempty"`
	DeviceID     string `json:"device_id,omitempty"`
	PointKey     string `json:"point_key,omitempty"`
	SamplePeriod string `json:"sample_period,omitempty"`
	Limit        int    `json:"limit,omitempty"`
	Offset       int    `json:"offset,omitempty"`
}

type DeviceHistoryStats struct {
	RecordCount  uint64 `json:"record_count"`
	EarliestDate string `json:"earliest_date"`
	LatestDate   string `json:"latest_date"`
}

type HistoryPointSummary struct {
	PointKey  string `json:"point_key"`
	PointName string `json:"point_name"`
	Unit      string `json:"unit"`
	Precision uint32 `json:"precision"`
}

// HistoryOverviewSummary 是历史总览批量接口返回的单设备、单点位、单周期轻量摘要。
type HistoryOverviewSummary struct {
	DeviceID            string  `json:"device_id"`
	MasterID            string  `json:"master_id"`
	ChannelID           string  `json:"channel_id"`
	PointKey            string  `json:"point_key"`
	PointName           string  `json:"point_name"`
	Unit                string  `json:"unit"`
	Precision           uint32  `json:"precision"`
	SamplePeriod        string  `json:"sample_period"`
	RecordCount         uint64  `json:"record_count"`
	LatestValue         float64 `json:"latest_value"`
	LatestTimestampMS   uint64  `json:"latest_timestamp_ms"`
	LatestBucketStartMS uint64  `json:"latest_bucket_start_ms"`
}

// DeviceHistoryView 是单设备历史页的后端聚合视图。
type DeviceHistoryView struct {
	Device         DeviceConfig          `json:"device"`
	Master         MasterNodeConfig      `json:"master"`
	Channel        ChannelConfig         `json:"channel"`
	HistoryRecords []HistoryRecord       `json:"history_records"`
	HistoryPoints  []HistoryPointSummary `json:"history_points"`
	Stats          DeviceHistoryStats    `json:"stats"`
}

// CommunicationTraceRecord 承接后端内存中的单次 Modbus 通讯事务。
type CommunicationTraceRecord struct {
	Sequence         uint64 `json:"sequence"`
	TimestampMS      uint64 `json:"timestamp_ms"`
	ChannelID        string `json:"channel_id"`
	MasterID         string `json:"master_id"`
	DeviceIndex      int64  `json:"device_index"`
	DeviceID         string `json:"device_id"`
	BlockKey         string `json:"block_key"`
	BlockDisplayName string `json:"block_display_name"`
	Protocol         string `json:"protocol"`
	SlaveAddress     uint32 `json:"slave_address"`
	FunctionCode     uint32 `json:"function_code"`
	StartRegister    uint32 `json:"start_register"`
	RegisterCount    uint32 `json:"register_count"`
	RequestHex       string `json:"request_hex"`
	ResponseHex      string `json:"response_hex"`
	ElapsedMS        uint32 `json:"elapsed_ms"`
	Result           string `json:"result"`
	ErrorMessage     string `json:"error_message"`
}

type ChannelCommunicationTraces struct {
	ChannelID string                     `json:"channel_id"`
	Records   []CommunicationTraceRecord `json:"records"`
}

type CommunicationTraceQuery struct {
	ChannelID string `json:"channel_id"`
	Limit     int    `json:"limit,omitempty"`
}

type CommunicationTraceClearResult struct {
	ChannelID string `json:"channel_id"`
	Cleared   bool   `json:"cleared"`
}

type ModbusWriteMultipleRegistersResponse struct {
	Success            bool   `json:"success"`
	MasterID           string `json:"master_id"`
	MasterName         string `json:"master_name"`
	ChannelID          string `json:"channel_id"`
	ChannelName        string `json:"channel_name"`
	Protocol           string `json:"protocol"`
	SlaveAddress       uint32 `json:"slave_address"`
	FunctionCode       uint32 `json:"function_code"`
	StartRegister      uint32 `json:"start_register"`
	RegisterCount      uint32 `json:"register_count"`
	RequestHex         string `json:"request_hex"`
	ResponseHex        string `json:"response_hex"`
	Status             string `json:"status"`
	DiagnosisErrorCode string `json:"diagnosis_error_code"`
	ErrorMessage       string `json:"error_message"`
	TimestampMS        uint64 `json:"timestamp_ms"`
}

type ModbusReadHoldingRegistersResponse struct {
	Success            bool     `json:"success"`
	MasterID           string   `json:"master_id"`
	MasterName         string   `json:"master_name"`
	ChannelID          string   `json:"channel_id"`
	ChannelName        string   `json:"channel_name"`
	Protocol           string   `json:"protocol"`
	SlaveAddress       uint32   `json:"slave_address"`
	FunctionCode       uint32   `json:"function_code"`
	StartRegister      uint32   `json:"start_register"`
	RegisterCount      uint32   `json:"register_count"`
	Values             []uint16 `json:"values"`
	RequestHex         string   `json:"request_hex"`
	ResponseHex        string   `json:"response_hex"`
	Status             string   `json:"status"`
	DiagnosisErrorCode string   `json:"diagnosis_error_code"`
	ErrorMessage       string   `json:"error_message"`
	TimestampMS        uint64   `json:"timestamp_ms"`
}

type DeviceCommandExecuteRequest struct {
	DeviceID   string            `json:"device_id"`
	CommandKey string            `json:"command_key"`
	Values     map[string]uint16 `json:"values"`
}

type DeviceCommandExecuteResponse struct {
	Success     bool                                 `json:"success"`
	DeviceID    string                               `json:"device_id"`
	DeviceName  string                               `json:"device_name"`
	TemplateID  string                               `json:"template_id"`
	CommandKey  string                               `json:"command_key"`
	CommandName string                               `json:"command_name"`
	Warnings    []string                             `json:"warnings"`
	SuccessHint string                               `json:"success_hint"`
	WriteResult ModbusWriteMultipleRegistersResponse `json:"write_result"`
}

type EM100RecordReadResponse struct {
	Success        bool                               `json:"success"`
	DeviceID       string                             `json:"device_id"`
	DeviceName     string                             `json:"device_name"`
	TemplateID     string                             `json:"template_id"`
	RecordType     string                             `json:"record_type"`
	ValidRecord    bool                               `json:"valid_record"`
	UnreadCount    uint32                             `json:"unread_count"`
	Title          string                             `json:"title"`
	Content        string                             `json:"content"`
	DataText       string                             `json:"data_text"`
	RatioType      string                             `json:"ratio_type"`
	RatioValueText string                             `json:"ratio_value_text"`
	RecordTime     string                             `json:"record_time"`
	RawRegisters   []uint16                           `json:"raw_registers"`
	ReadResult     ModbusReadHoldingRegistersResponse `json:"read_result"`
}

// RealtimeViewSnapshot 是后端为实时监控页提供的一次性聚合快照。
type RealtimeViewSnapshot struct {
	DeviceTemplateGeneration uint64                   `json:"device_template_generation"`
	Devices                  []DeviceConfig           `json:"devices"`
	Channels                 []ChannelConfig          `json:"channels"`
	Masters                  []MasterNodeConfig       `json:"masters"`
	SystemStatus             SystemStatus             `json:"system_status"`
	DeviceRealtimeSnapshots  []DeviceRealtimeSnapshot `json:"device_realtime_snapshots"`
}

// SystemStatus 是后端整体运行状态快照。
type SystemStatus struct {
	ConfigLoaded              bool               `json:"config_loaded"`
	ServiceReady              bool               `json:"service_ready"`
	Running                   bool               `json:"running"`
	PollingRunning            bool               `json:"polling_running"`
	PollingState              string             `json:"polling_state"`
	StartedAtMS               uint64             `json:"started_at_ms"`
	StoppedAtMS               uint64             `json:"stopped_at_ms"`
	LastHeartbeatMS           uint64             `json:"last_heartbeat_ms"`
	LastPollCycleStartedAtMS  uint64             `json:"last_poll_cycle_started_at_ms"`
	LastPollCycleFinishedAtMS uint64             `json:"last_poll_cycle_finished_at_ms"`
	OnlineChannelCount        uint64             `json:"online_channel_count"`
	OnlineMasterCount         uint64             `json:"online_master_count"`
	OnlineDeviceCount         uint64             `json:"online_device_count"`
	LastPollCycleMasterCount  uint64             `json:"last_poll_cycle_master_count"`
	SuccessMasterCount        uint64             `json:"last_poll_cycle_success_master_count"`
	FailedMasterCount         uint64             `json:"last_poll_cycle_failed_master_count"`
	SuccessDeviceCount        uint64             `json:"last_poll_cycle_success_device_count"`
	FailedDeviceCount         uint64             `json:"last_poll_cycle_failed_device_count"`
	LastPollCycleHasError     bool               `json:"last_poll_cycle_has_error"`
	Diagnosis                 DiagnosisStatus    `json:"diagnosis"`
	LastStatusMessage         string             `json:"last_status_message"`
	LastPollCycleErrorMessage string             `json:"last_poll_cycle_error_message"`
	ChannelStatusList         []ChannelStatus    `json:"channel_status_list"`
	MasterStatusList          []MasterNodeStatus `json:"master_status_list"`
	DeviceStatusList          []DeviceStatus     `json:"device_status_list"`
}
