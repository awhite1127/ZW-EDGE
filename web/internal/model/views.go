package model

// 本文件定义服务层交给 HTML 模板的页面视图模型，不作为后端持久化或 IPC 协议结构使用。

// FlashMessage 用于页面动作后的轻量提示。
type FlashMessage struct {
	Type string
	Text string
}

// SectionState 描述某个页面区块是否成功加载。
// 页面可以据此决定是完整展示还是局部降级。
type SectionState struct {
	Available    bool
	ErrorMessage string
}

// BasePageData 是所有模板页面共用的基础字段。
type BasePageData struct {
	Title                      string
	Subtitle                   string
	ActiveNav                  string
	SystemDisplayName          string
	BackendReachable           bool
	Authenticated              bool
	CurrentUsername            string
	CurrentRoleText            string
	CSRFToken                  string
	IsAdmin                    bool
	CanModify                  bool
	CanViewOverview            bool
	CanViewRealtime            bool
	CanViewHistory             bool
	CanViewEvents              bool
	CanManageCollection        bool
	CanManageSystemSettings    bool
	CanManageNetwork           bool
	CanManageMqtt              bool
	CanManageModbusServer      bool
	CanManageModbusMappings    bool
	CanManageOperations        bool
	CanManageApplicationUpdate bool
	CanManageUsers             bool
	CanManageAlarmRules        bool
	CanAckAlarm                bool
	CanManageDataMaintenance   bool
	CanImportExportConfig      bool
	CanExportDiagnostics       bool
	CanFactoryReset            bool
	CanManageDeviceTemplates   bool
	CanManageDeviceNames       bool
	CanExecuteDeviceCommands   bool
	CurrentPath                string
	PasswordChangeRecommended  bool
	PollingKnown               bool
	PollingRunning             bool
	PollingState               string
	Flash                      *FlashMessage
	ErrorMessage               string
}

// LoginPageData 对应登录页模板。
type LoginPageData struct {
	BasePageData
	Redirect                     string
	FirstBootAdminEntryAvailable bool
}

// OverviewPageData 对应概览页，由多个区块数据拼装而成。
type OverviewPageData struct {
	BasePageData
	Settings                 SystemSettings
	SettingsState            SectionState
	SystemStatus             SystemStatus
	SystemStatusState        SectionState
	ConfigSummary            ConfigSummary
	ConfigState              SectionState
	Channels                 []ChannelConfig
	Masters                  []MasterNodeConfig
	Devices                  []DeviceConfig
	RecentError              ServiceErrorSummary
	RecentErrorState         SectionState
	RuntimeSnapshot          SystemOverviewSnapshot
	RuntimeSnapshotAvailable bool
	DiagnosisCard            OverviewDiagnosisCard
	CollectionObject         CollectionObjectOverview
	RecentEvents             []ServiceEvent
	EventsState              SectionState
	ActiveAlarms             []ActiveAlarm
	ActiveAlarmPreview       []ActiveAlarm
	AlarmObjectText          string
	RemainingAlarmCount      int
	ActiveAlarmsState        SectionState
}

// CollectionLinkOverview 是采集对象概览内部复用的配置链路判断模型。
type CollectionLinkOverview struct {
	Status       string
	StatusText   string
	StatusClass  string
	Summary      string
	BlockingText string
}

// CollectionObjectOverview 是总览页中间采集对象概览区的展示模型。
type CollectionObjectOverview struct {
	Status          string
	StatusText      string
	StatusClass     string
	ConfigHint      string
	EmptyText       string
	Metrics         []CollectionObjectMetric
	MasterSummaries []CollectionMasterSummary
	MoreMasterCount int
}

type CollectionObjectMetric struct {
	Label string
	Value string
	State string
}

type CollectionMasterSummary struct {
	Name          string
	AddressText   string
	TemplateName  string
	DeviceCount   int
	NormalDevices int
	ErrorDevices  int
	StatusText    string
	StatusState   string
	RegisterRange string
}

// OverviewDiagnosisCard 是总览页左侧系统运行诊断区的展示模型。
type OverviewDiagnosisCard struct {
	HasIssue            bool   `json:"has_issue"`
	StateText           string `json:"state_text"`
	StateClass          string `json:"state_class"`
	CollectStateText    string `json:"collect_state_text"`
	TargetText          string `json:"target_text"`
	TypeText            string `json:"type_text"`
	ErrorCode           string `json:"error_code"`
	ConsecutiveFailures uint32 `json:"consecutive_failures"`
	LastSuccessTimeMS   uint64 `json:"last_success_time_ms"`
	LastErrorTimeMS     uint64 `json:"last_error_time_ms"`
	Suggestion          string `json:"suggestion"`
	Note                string `json:"note"`
}

// OverviewDiagnosisResponse 是总览页诊断区域的局部刷新响应。
type OverviewDiagnosisResponse struct {
	BackendReachable bool                  `json:"backend_reachable"`
	ErrorMessage     string                `json:"error_message"`
	Card             OverviewDiagnosisCard `json:"card"`
}

// EventsPageData 对应事件告警中心页。
type EventsPageData struct {
	BasePageData
	ActiveTab                string
	ActiveAlarms             []ActiveAlarm
	ActiveAlarmPreview       []ActiveAlarm
	RemainingAlarmCount      int
	ActiveAlarmsState        SectionState
	AlarmRulesState          SectionState
	AlarmRuleItems           []AlarmRuleConfigItem
	AlarmRuleDevices         []AlarmRuleDeviceOption
	AlarmRulesEmptyText      string
	ActiveAlarmCount         int
	UnacknowledgedAlarmCount int
	EnabledRuleCount         int
	ConfigurableCount        int
	Events                   []ServiceEvent
	EventsState              SectionState
	TotalEventCount          int
	FilteredEventCount       int
	HasAnyEvents             bool
	ErrorCount               int
	WarningCount             int
	InfoCount                int
	LevelFilter              string
	AllEventsURL             string
	ErrorEventsURL           string
	WarningEventsURL         string
	InfoEventsURL            string
	SourceFilter             string
	SearchQuery              string
	TimeRange                string
	SourceOptions            []EventFilterOption
	ExportURL                string
	Page                     int
	PageSize                 int
	PageSizeOptions          []int
	TotalPages               int
	PrevPage                 int
	NextPage                 int
	HasPrevPage              bool
	HasNextPage              bool
	PageStart                int
	PageEnd                  int
}

// AlarmRuleConfigItem 是告警配置列表中“主站 + 数据项”的展示行。
type AlarmRuleConfigItem struct {
	MasterID           string
	MasterName         string
	TemplateID         string
	TemplateName       string
	PointKey           string
	PointName          string
	Unit               string
	Precision          uint32
	CoveredDeviceCount int
	CoveredDeviceNames string
	ExistingRuleCount  int
	HasRule            bool
	Rule               AlarmRule
	StatusText         string
	LimitTypeText      string
	CoverageText       string
	LevelText          string
	HighText           string
	LowText            string
	TriggerText        string
	RecoveryText       string
}

type AlarmRuleDeviceOption struct {
	MasterID   string
	MasterName string
	Points     []AlarmRulePointOption
}

type AlarmRulePointOption struct {
	MasterID  string
	PointKey  string
	PointName string
	Unit      string
	Precision uint32
}

// SettingsPageData 对应系统设置页。
type SettingsPageData struct {
	BasePageData
	Settings                 SystemSettings
	SettingsState            SectionState
	TimeSettings             TimeSettings
	TimeSettingsState        SectionState
	TimeRuntime              TimeRuntimeStatus
	TimeRuntimeState         SectionState
	Network                  NetworkSettings
	NetworkState             SectionState
	NetworkRuntime           NetworkRuntimeStatus
	NetworkRuntimeState      SectionState
	NetworkDNSInput          string
	Mqtt                     MqttSettings
	MqttState                SectionState
	MqttRuntime              MqttRuntimeStatus
	MqttRuntimeState         SectionState
	DeviceTemplateManagement DeviceTemplateManagementView
	DeviceTemplateState      SectionState
	DeviceTemplatePagination DeviceTemplatePagination
	SettingsReturnPath       string
}

type DeviceTemplateEditorPageData struct {
	BasePageData
	Definition DeviceTemplateDefinition
	Editing    bool
	ReturnPath string
}

type ModbusServerPageData struct {
	BasePageData
	Snapshot         ModbusServerPageSnapshot
	ExportablePoints []ModbusExportablePoint
	SnapshotState    SectionState
	PointsState      SectionState
}

type OperationsPageData struct {
	BasePageData
	Users            []WebUser
	UserCount        int
	EnabledUserCount int
	UsersState       SectionState
	Maintenance      DataMaintenanceSummary
	MaintenanceState SectionState
	Roles            []RolePermissionView
}

type RolePermissionView struct {
	Role        string
	Name        string
	Description string
	Permissions []RolePermissionItem
}

type RolePermissionItem struct {
	Key     string
	Name    string
	Allowed bool
}

// SettingsRuntimeStatusResponse 是设置页前端轮询运行状态使用的聚合响应。
type SettingsRuntimeStatusResponse struct {
	BackendReachable        bool                 `json:"backend_reachable"`
	NetworkRuntime          NetworkRuntimeStatus `json:"network_runtime"`
	NetworkRuntimeAvailable bool                 `json:"network_runtime_available"`
	NetworkRuntimeError     string               `json:"network_runtime_error"`
	MqttRuntime             MqttRuntimeStatus    `json:"mqtt_runtime"`
	MqttRuntimeAvailable    bool                 `json:"mqtt_runtime_available"`
	MqttRuntimeError        string               `json:"mqtt_runtime_error"`
	TimeRuntime             TimeRuntimeStatus    `json:"time_runtime"`
	TimeRuntimeAvailable    bool                 `json:"time_runtime_available"`
	TimeRuntimeError        string               `json:"time_runtime_error"`
}

// DeviceTemplatePagination 描述系统设置页内置模板的固定分页状态。
type DeviceTemplatePagination struct {
	Page        int
	TotalPages  int
	Pages       []int
	HasPrevious bool
	HasNext     bool
	Previous    int
	Next        int
	RangeStart  int
	RangeEnd    int
	TotalItems  int
}

// EventFilterOption 表示历史事件页筛选下拉项。
type EventFilterOption struct {
	Value string
	Label string
}

// EventsPageQuery 表示历史事件页的筛选和分页条件。
type EventsPageQuery struct {
	Tab       string
	Level     string
	Source    string
	Search    string
	TimeRange string
	Page      int
	PageSize  int
}

// RealtimeLoadResult 是实时页专用聚合结果。
type RealtimeLoadResult struct {
	Rows                 []RealtimeRow
	Dashboard            RealtimeDashboard
	WarningMessage       string
	BackendReachable     bool
	PrimaryDataAvailable bool
}

// ChannelSerialOption 是通道页中单个串口候选项的展示模型。
type ChannelSerialOption struct {
	Path          string
	DisplayName   string
	Kind          string
	Available     bool
	Busy          bool
	Description   string
	SymlinkByID   string
	SymlinkByPath string
	Driver        string
	PhysicalHint  string
	IsCurrent     bool
}

// ChannelSerialBindingView 同时描述当前绑定和系统发现的候选串口。
type ChannelSerialBindingView struct {
	CurrentPortPath  string
	CurrentPortFound bool
	Candidates       []ChannelSerialOption
}

// ChannelsLoadResult 是通道页专用的聚合结果。
// 除行数据外，还包含系统串口发现区块的状态。
type ChannelsLoadResult struct {
	Rows                 []ChannelRow
	SerialPorts          []SerialPortInfo
	SerialPortsState     SectionState
	WarningMessage       string
	BackendReachable     bool
	PrimaryDataAvailable bool
}

// ChannelRow 把通道配置、运行状态和串口候选绑定视图合并到一行。
type ChannelRow struct {
	Config        ChannelConfig
	Status        ChannelStatus
	HasStatus     bool
	SerialBinding ChannelSerialBindingView
	EndpointText  string
	ParamText     string
	TypeText      string
}

// ChannelsPageData 对应通道页模板。
type ChannelsPageData struct {
	BasePageData
	Rows              []ChannelRow
	SerialPorts       []SerialPortInfo
	SerialPortsState  SectionState
	ShowPollingNotice bool
}

// CommunicationTracesPageData 对应单通道通讯报文查看页。
type CommunicationTracesPageData struct {
	BasePageData
	ChannelID string
	BackURL   string
}

// MasterRow 把主站配置与运行状态合并到一行。
type MasterRow struct {
	Config              MasterNodeConfig
	Status              MasterNodeStatus
	HasStatus           bool
	ProtocolText        string
	TargetLabel         string
	TargetValue         string
	DeviceTemplateName  string
	DeviceTemplateNotes string
}

// MasterChannelOption 是主站页“关联通道”下拉框的单个候选项。
type MasterChannelOption struct {
	ChannelID    string
	ChannelName  string
	ChannelType  string
	PortPath     string
	Enabled      bool
	HasStatus    bool
	IsFault      bool
	DisplayLabel string
}

// MastersLoadResult 是主站页专用的聚合结果。
type MastersLoadResult struct {
	Rows                 []MasterRow
	ChannelOptions       []MasterChannelOption
	DeviceTemplates      []DeviceTemplateDefinition
	WarningMessage       string
	BackendReachable     bool
	PrimaryDataAvailable bool
}

// MastersPageData 对应主站页模板。
type MastersPageData struct {
	BasePageData
	Rows              []MasterRow
	Channels          []MasterChannelOption
	DeviceTemplates   []DeviceTemplateDefinition
	ShowPollingNotice bool
}

// DeviceRow 把设备配置与运行状态合并到一行。
type DeviceRow struct {
	Config                 DeviceConfig
	Status                 DeviceStatus
	HasStatus              bool
	MasterLabel            string
	MasterDetailText       string
	MasterInvalid          bool
	ChannelName            string
	ChannelText            string
	ChannelDetailText      string
	MasterStatusText       string
	SlaveAddressText       string
	SequenceText           string
	RealStartText          string
	TemplateName           string
	TemplateReadBlockCount int
	TemplateAddressStride  int
	TemplateFields         []DeviceTemplateField
	SummaryPoints          []RealtimePointRow
}

// DevicesLoadResult 是设备页专用的聚合结果。
type DevicesLoadResult struct {
	Rows                 []DeviceRow
	WarningMessage       string
	BackendReachable     bool
	PrimaryDataAvailable bool
}

// DevicesPageData 对应设备页模板。
type DevicesPageData struct {
	BasePageData
	Rows              []DeviceRow
	ShowPollingNotice bool
	ChannelCount      int
	MasterCount       int
}

// CollectionPageData 对应采集管理页，聚合通道、主站和设备三个配置区块。
type CollectionPageData struct {
	BasePageData
	Channels       ChannelsPageData
	Masters        MastersPageData
	Devices        DevicesPageData
	WarningMessage string
	ChannelCount   int
	MasterCount    int
	DeviceCount    int
}

type DeviceDetailReadProfile struct {
	TrueStartRegister   uint32                    `json:"true_start_register"`
	DeviceAddressStride int                       `json:"device_address_stride"`
	ReadBlocks          []DeviceTemplateReadBlock `json:"read_blocks"`
	SummaryFields       []DeviceTemplateField     `json:"summary_fields"`
}

type DeviceDetailStatus struct {
	Enabled              bool            `json:"enabled"`
	Online               bool            `json:"online"`
	HasStatus            bool            `json:"has_status"`
	CommunicationQuality string          `json:"communication_quality"`
	LastCollectTime      string          `json:"last_collect_time"`
	LastCollectTimeMS    uint64          `json:"last_collect_time_ms"`
	StatusSummary        string          `json:"status_summary"`
	Diagnosis            DiagnosisStatus `json:"diagnosis"`
}

type DeviceDetailHistory struct {
	HasHistory     bool                     `json:"has_history"`
	HistoryRecords []DeviceHistoryRecordRow `json:"recent_records"`
	Summary        DeviceHistorySummary     `json:"summary"`
	FullHistoryURL string                   `json:"full_history_url"`
}

type DeviceDetailResponse struct {
	Device        DeviceConfig                 `json:"device"`
	DeviceLabel   string                       `json:"device_label"`
	TemplateID    string                       `json:"template_id"`
	TemplateName  string                       `json:"template_name"`
	Master        MasterNodeConfig             `json:"master"`
	MasterLabel   string                       `json:"master_label"`
	Channel       ChannelConfig                `json:"channel"`
	ChannelLabel  string                       `json:"channel_label"`
	ProtocolText  string                       `json:"protocol_text"`
	SlaveAddress  uint16                       `json:"slave_address"`
	Status        DeviceDetailStatus           `json:"status"`
	ReadProfile   DeviceDetailReadProfile      `json:"read_profile"`
	Points        []PointValue                 `json:"points"`
	WriteCommands []DeviceTemplateWriteCommand `json:"write_commands"`
	History       DeviceDetailHistory          `json:"history"`
}

// HistoryOverviewPageData 对应历史数据总览页。
type HistoryOverviewPageData struct {
	BasePageData
	Rows                    []HistoryOverviewRow
	FilterTree              HistoryFilterTree
	DeviceOptions           []HistoryOverviewDeviceOption
	CurrentDeviceID         string
	CurrentDeviceName       string
	ShowDeviceSelector      bool
	CurrentFilterText       string
	RefreshURL              string
	ExportURL               string
	HasHistoryFilter        bool
	InvalidFilterMessage    string
	HasDevices              bool
	EmptyStateText          string
	EmptyStateDescription   string
	TableStatusText         string
	ViewableDeviceCount     int
	HistoryPointCount       int
	TotalHistoryRecordCount int
	LatestHistoryTimeText   string
	Maintenance             DataMaintenanceSummary
	MaintenanceState        SectionState
}

// HistoryOverviewDeviceOption 是主站含多台设备时显示在内容区的设备入口。
type HistoryOverviewDeviceOption struct {
	ID       string
	Label    string
	URL      string
	Selected bool
}

// HistoryOverviewQuery 表示历史数据总览页的设备层级筛选参数。
type HistoryOverviewQuery struct {
	ChannelID string
	MasterID  string
	DeviceID  string
}

// HistoryFilterTree 是历史数据页左侧设备筛选树。
type HistoryFilterTree struct {
	AllURL    string
	AllActive bool
	Channels  []HistoryFilterNode
}

// HistoryFilterNode 表示通道、主站或设备筛选节点。
type HistoryFilterNode struct {
	Type        string
	ID          string
	Label       string
	URL         string
	Active      bool
	HasIssue    bool
	DeviceCount int
	IssueCount  int
	Expanded    bool
	Children    []HistoryFilterNode
}

// HistoryOverviewRow 是历史总览页每台设备的摘要行。
type HistoryOverviewRow struct {
	DeviceID                 string
	DeviceName               string
	MasterID                 string
	MasterName               string
	ChannelID                string
	ChannelName              string
	PointKey                 string
	PointName                string
	PointDisplayName         string
	Unit                     string
	Precision                uint32
	LatestValueText          string
	RecordCount              int
	LastUpdatedText          string
	LastUpdatedMS            uint64
	DetailURL                string
	StatusText               string
	CommunicationQualityText string
	HasHistory               bool
}

// DeviceHistoryLoadResult 是单设备历史页的数据加载结果。
type DeviceHistoryLoadResult struct {
	PageData             DeviceHistoryPageData
	BackendReachable     bool
	PrimaryDataAvailable bool
}

// DeviceHistoryPageData 对应单设备历史数据详情页。
type DeviceHistoryPageData struct {
	BasePageData
	BackURL               string
	DeviceID              string
	DeviceName            string
	MasterID              string
	MasterName            string
	MasterAddressText     string
	ChannelID             string
	ChannelName           string
	HasDevice             bool
	HasHistory            bool
	HistoryState          SectionState
	ExportURL             string
	CurrentPeriod         string
	CurrentPeriodLabel    string
	CurrentPointKey       string
	CurrentPointName      string
	CurrentPointUnit      string
	CurrentPointPrecision uint32
	AvailablePoints       []DeviceHistoryPointOption
	PeriodOptions         []DeviceHistoryPeriodOption
	Summary               DeviceHistorySummary
	Chart                 DeviceHistoryChart
	AlarmThresholds       []DeviceHistoryThresholdLine
	HistoryRecords        []DeviceHistoryRecordRow
}

type DeviceHistoryPointOption struct {
	Key       string
	Name      string
	Unit      string
	Precision uint32
	Selected  bool
	URL       string
}

type DeviceHistoryPeriodOption struct {
	Value    string
	Label    string
	Selected bool
	URL      string
}

// DeviceHistorySummary 是当前周期的历史数据摘要。
type DeviceHistorySummary struct {
	HasData          bool
	LatestValueText  string
	LatestDate       string
	MaxValueText     string
	MinValueText     string
	AverageValueText string
	RecordCount      int
}

// DeviceHistoryChart 是轻量 SVG 趋势图渲染所需的数据。
type DeviceHistoryChart struct {
	HasData        bool
	HasLine        bool
	PolylinePoints string
	Segments       []string
	Points         []DeviceHistoryChartPoint
	Labels         []DeviceHistoryChartLabel
	YTicks         []DeviceHistoryChartTick
	AxisMin        float64
	AxisMax        float64
	MinLabel       string
	MaxLabel       string
	StartDate      string
	EndDate        string
	Message        string
}

type DeviceHistoryChartTick struct {
	Y     string
	Label string
}

type DeviceHistoryChartLabel struct {
	X         string
	Y         string
	ValueText string
	ClassName string
}

type DeviceHistoryThresholdLine struct {
	Kind       string
	Label      string
	Y          string
	LabelClass string
	OutOfView  bool
}

type DeviceHistoryChartPoint struct {
	X         string
	Y         string
	Date      string
	ValueText string
}

type DeviceHistoryRecordRow struct {
	TimeText     string `json:"time_text"`
	Date         string `json:"date"`
	PointName    string `json:"point_name"`
	DisplayValue string `json:"display_value"`
	ValueText    string `json:"value_text"`
	Quality      string `json:"quality"`
	TimestampMS  uint64 `json:"timestamp_ms"`
	MasterID     string `json:"master_id"`
	ChannelID    string `json:"channel_id"`
	SamplePeriod string `json:"sample_period"`
	SampleInfo   string `json:"sample_info"`
}

// RealtimePointRow 是实时页关键数值区可直接渲染的数据项展示模型。
type RealtimePointRow struct {
	Key          string `json:"key"`
	StableKey    string `json:"-"`
	Name         string `json:"name"`
	Text         string `json:"text"`
	ValueText    string `json:"-"`
	Unit         string `json:"-"`
	Quality      string `json:"quality"`
	Valid        bool   `json:"valid"`
	Message      string `json:"message,omitempty"`
	StateClass   string `json:"state_class"`
	Summary      bool   `json:"summary"`
	DisplayOrder int    `json:"display_order"`
}

type RealtimePointGroup struct {
	ID     string             `json:"id"`
	Name   string             `json:"name"`
	Order  int                `json:"order"`
	Points []RealtimePointRow `json:"points"`
}

// RealtimeRow 是实时页最终直接渲染的视图模型。
// 它已经合并了配置、状态和实时快照中的主要字段。
type RealtimeRow struct {
	DeviceID                string                `json:"device_id"`
	StableKey               string                `json:"-"`
	SummaryKey              string                `json:"-"`
	DeviceName              string                `json:"device_name"`
	ChannelID               string                `json:"channel_id"`
	ChannelName             string                `json:"channel_name"`
	MasterID                string                `json:"master_id"`
	MasterName              string                `json:"master_name"`
	TemplateID              string                `json:"template_id"`
	TemplateName            string                `json:"template_name"`
	TemplateFields          []DeviceTemplateField `json:"template_fields"`
	Online                  bool                  `json:"online"`
	HasStatus               bool                  `json:"has_status"`
	HasRealtime             bool                  `json:"has_realtime"`
	UpdatedAtMS             uint64                `json:"updated_at_ms"`
	Diagnosis               DiagnosisStatus       `json:"diagnosis"`
	ErrorMessage            string                `json:"error_message"`
	CommunicationQuality    string                `json:"communication_quality"`
	SummaryPoints           []RealtimePointRow    `json:"summary_points"`
	RealtimeGroupingEnabled bool                  `json:"realtime_grouping_enabled"`
	RealtimeGroups          []RealtimePointGroup  `json:"realtime_groups"`
	SummaryText             string                `json:"summary_text"`
	ResistanceText          string                `json:"resistance_text"`
}

// RealtimeLayerItem 表示实时页通道层或主站层摘要。
type RealtimeLayerItem struct {
	ID           string          `json:"id"`
	Name         string          `json:"name"`
	OnlineCount  int             `json:"online_count"`
	TotalCount   int             `json:"total_count"`
	StateText    string          `json:"state_text"`
	StateClass   string          `json:"state_class"`
	Diagnosis    DiagnosisStatus `json:"diagnosis"`
	ErrorMessage string          `json:"error_message,omitempty"`
}

// RealtimeDeviceSummary 表示实时页设备层摘要。
type RealtimeDeviceSummary struct {
	OnlineCount        int             `json:"online_count"`
	OfflineCount       int             `json:"offline_count"`
	AlarmCount         int             `json:"alarm_count"`
	AlarmDeviceNames   []string        `json:"alarm_device_names"`
	AlarmDevicePreview []string        `json:"alarm_device_preview"`
	AlarmDeviceText    string          `json:"alarm_device_text"`
	TotalCount         int             `json:"total_count"`
	StateText          string          `json:"state_text"`
	StateClass         string          `json:"state_class"`
	DetailText         string          `json:"detail_text"`
	Diagnosis          DiagnosisStatus `json:"diagnosis"`
	ErrorMessage       string          `json:"error_message,omitempty"`
}

type RealtimeLayerSummary struct {
	TotalCount    int    `json:"total_count"`
	OnlineCount   int    `json:"online_count"`
	AbnormalCount int    `json:"abnormal_count"`
	Text          string `json:"text"`
}

// RealtimeSystemCore 表示实时页系统核心摘要。
type RealtimeSystemCore struct {
	BackendStateText string          `json:"backend_state_text"`
	PollingStateText string          `json:"polling_state_text"`
	LinkStateText    string          `json:"link_state_text"`
	StateClass       string          `json:"state_class"`
	Diagnosis        DiagnosisStatus `json:"diagnosis"`
	LastErrorText    string          `json:"last_error_text"`
}

// RealtimeDashboard 表示实时页四个概览卡片的数据。
type RealtimeDashboard struct {
	SystemCore          RealtimeSystemCore    `json:"system_core"`
	Channels            []RealtimeLayerItem   `json:"channels"`
	Masters             []RealtimeLayerItem   `json:"masters"`
	ChannelSummary      RealtimeLayerSummary  `json:"channel_summary"`
	MasterSummary       RealtimeLayerSummary  `json:"master_summary"`
	DeviceSummary       RealtimeDeviceSummary `json:"device_summary"`
	EmptyStateText      string                `json:"empty_state_text"`
	TableStatusText     string                `json:"table_status_text"`
	AutoRefreshText     string                `json:"auto_refresh_text"`
	LatestRefreshStatus string                `json:"latest_refresh_status"`
}

// RealtimePageData 对应实时页首屏模板数据。
type RealtimePageData struct {
	BasePageData
	Rows        []RealtimeRow
	Dashboard   RealtimeDashboard
	RefreshedAt string
}

// RealtimeViewResponse 供前端 JS 轮询刷新实时页时使用。
type RealtimeViewResponse struct {
	Rows             []RealtimeRow     `json:"rows"`
	Dashboard        RealtimeDashboard `json:"dashboard"`
	RefreshedAt      string            `json:"refreshed_at"`
	BackendReachable bool              `json:"backend_reachable"`
	ErrorMessage     string            `json:"error_message,omitempty"`
}

// ActionFeedback 是页面按钮动作的统一反馈结果。
type ActionFeedback struct {
	Success bool
	Message string
	Type    string
}
