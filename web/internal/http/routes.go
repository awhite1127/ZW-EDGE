package httpserver

// 路由按页面、动作和 JSON API 三类集中注册；所有非公开路径统一经过 authMiddleware。

import "net/http"

// registerRoutes 注册路由。
func (s *Server) registerRoutes(mux *http.ServeMux, staticDir string) {
	// 静态资源与页面路由。
	mux.Handle(
		"GET /static/",
		staticAssetCacheMiddleware(http.StripPrefix("/static/", staticFileServer(staticDir))),
	)

	mux.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		target := defaultAuthenticatedPath
		if keyboard := r.URL.Query().Get("keyboard"); keyboard == "0" || keyboard == "1" {
			target += "?keyboard=" + keyboard
		}
		http.Redirect(w, r, target, http.StatusSeeOther)
	})
	mux.HandleFunc("GET /overview", s.handleOverviewPage)
	mux.HandleFunc("GET /collection", s.handleCollectionPage)
	mux.HandleFunc("GET /collection/channels/{id}/communication-traces", s.handleCommunicationTracesPage)
	mux.HandleFunc("GET /history", s.handleHistoryOverviewPage)
	mux.HandleFunc("GET /history/export.csv", s.handleHistoryCSVExport)
	mux.HandleFunc("GET /device-history", s.handleDeviceHistoryPage)
	mux.HandleFunc("GET /realtime", s.handleRealtimePage)
	mux.HandleFunc("GET /events", s.handleEventsPage)
	mux.HandleFunc("GET /events/export.csv", s.handleEventsCSVExport)
	mux.HandleFunc("GET /events/diagnostics/export", s.handleDiagnosticExport)
	mux.HandleFunc("GET /settings", s.handleSettingsPage)
	mux.HandleFunc("GET /settings/device-types", s.handleSettingsDeviceTypesPage)
	mux.HandleFunc("GET /settings/device-types/editor", s.handleDeviceTemplateEditorPage)
	mux.HandleFunc("GET /settings/mqtt", s.handleSettingsMqttPage)
	mux.HandleFunc("GET /settings/modbus-server", s.handleModbusServerPage)
	mux.HandleFunc("GET /settings/config/export", s.handleSettingsConfigExport)
	mux.HandleFunc("GET /operations", s.handleOperationsPage)
	mux.HandleFunc("GET /upgrade-wait", s.handleApplicationUpdateWaitPage)
	mux.HandleFunc("GET /operations/users", s.handleOperationsUsersPage)
	// 设置、账户和登录相关操作。
	mux.HandleFunc("POST /settings", s.handleSettingsAction)
	mux.HandleFunc("POST /account/password", s.handleAccountPasswordAction)
	mux.HandleFunc("POST /account/user-password", s.handleUserPasswordAction)
	mux.HandleFunc("POST /settings/network", s.handleNetworkSettingsAction)
	mux.HandleFunc("POST /settings/network/save-apply", s.handleNetworkSettingsSaveApplyAction)
	mux.HandleFunc("POST /settings/time/save-apply", s.handleTimeSettingsSaveApplyAction)
	mux.HandleFunc("POST /settings/time/sync-now", s.handleTimeSyncNowAction)
	mux.HandleFunc("POST /settings/time/manual", s.handleManualTimeAction)
	mux.HandleFunc("POST /settings/mqtt", s.handleMqttSettingsAction)
	mux.HandleFunc("POST /settings/config/import", s.handleSettingsConfigImport)
	mux.HandleFunc("POST /settings/factory-reset", s.handleFactoryResetAction)
	mux.HandleFunc("POST /operations/users/create", s.handleCreateWebUserAction)
	mux.HandleFunc("POST /operations/users/update", s.handleUpdateWebUserAction)
	mux.HandleFunc("POST /operations/users/reset-password", s.handleResetWebUserPasswordAction)
	mux.HandleFunc("GET /login", s.handleLoginPage)
	mux.HandleFunc("POST /login", s.handleLoginAction)
	mux.HandleFunc("POST /login/initial-setup", s.handleFirstBootAdminEntryAction)
	mux.HandleFunc("POST /logout", s.handleLogoutAction)

	// 历史事件、报警和数据维护操作。
	mux.HandleFunc("POST /actions/events/clear", s.handleClearEventsAction)
	mux.HandleFunc("POST /actions/history/cleanup-expired", s.handleCleanupExpiredDataAction)
	mux.HandleFunc("POST /actions/alarms/rules/save", s.handleSaveAlarmRuleAction)
	mux.HandleFunc("POST /actions/alarms/rules/disable", s.handleDisableAlarmRuleAction)
	mux.HandleFunc("POST /actions/alarms/rules/delete", s.handleDeleteAlarmRuleAction)
	mux.HandleFunc("POST /actions/alarms/acknowledge", s.handleAcknowledgeActiveAlarmAction)

	// 系统状态、设置和数据维护 API。
	mux.HandleFunc("GET /api/system/status", s.handleGetSystemStatus)
	mux.HandleFunc("GET /api/system/overview-snapshot", s.handleGetSystemOverviewSnapshot)
	mux.HandleFunc("GET /api/system/polling-summary", s.handleGetPollingSummary)
	mux.HandleFunc("GET /api/system/error", s.handleGetRecentError)
	mux.HandleFunc("GET /api/data-maintenance", s.handleDataMaintenanceSummary)
	mux.HandleFunc("GET /api/application-update/version", s.handleGetApplicationUpdateVersion)
	mux.HandleFunc("GET /api/application-update/status", s.handleGetApplicationUpdateStatus)
	mux.HandleFunc("GET /api/application-update/recent", s.handleGetApplicationUpdateStatus)
	mux.HandleFunc("POST /api/application-update/upload", s.handleUploadApplicationUpdatePackage)
	mux.HandleFunc("POST /api/application-update/start", s.handleStartApplicationUpdate)
	mux.HandleFunc("POST /api/application-update/watch-token", s.handleCreateApplicationUpdateWatch)
	mux.HandleFunc("GET /api/application-update/watch", s.handleWatchApplicationUpdateStatus)
	mux.HandleFunc("GET /api/settings/runtime-status", s.handleSettingsRuntimeStatus)
	// Modbus 服务端配置与寄存器映射 API。
	mux.HandleFunc("GET /api/modbus-server/page-snapshot", s.handleGetModbusServerPageSnapshot)
	mux.HandleFunc("GET /api/modbus-server/runtime-status", s.handleGetModbusServerRuntimeStatus)
	mux.HandleFunc("GET /api/modbus-server/exportable-points", s.handleListModbusExportablePoints)
	mux.HandleFunc("GET /api/modbus-server/mappings", s.handleListModbusRegisterMappings)
	mux.HandleFunc("PUT /api/modbus-server/settings", s.handleUpdateModbusServerSettings)
	mux.HandleFunc("POST /api/modbus-server/mappings", s.handleCreateModbusRegisterMapping)
	mux.HandleFunc("PUT /api/modbus-server/mappings/{id}", s.handleUpdateModbusRegisterMapping)
	mux.HandleFunc("DELETE /api/modbus-server/mappings/{id}", s.handleDeleteModbusRegisterMapping)
	// 采集通道、主站、设备和设备类型 API。
	mux.HandleFunc("GET /api/system/serial-ports", s.handleListSerialPorts)
	mux.HandleFunc("GET /api/channels", s.handleListChannels)
	mux.HandleFunc("POST /api/channels", s.handleCreateChannelConfig)
	mux.HandleFunc("PUT /api/channels/{id}", s.handleUpdateChannelConfig)
	mux.HandleFunc("DELETE /api/channels/{id}", s.handleDeleteChannelConfig)
	mux.HandleFunc("GET /api/channels/{id}/communication-traces", s.handleGetChannelCommunicationTraces)
	mux.HandleFunc("POST /api/channels/{id}/communication-traces/clear", s.handleClearChannelCommunicationTraces)
	mux.HandleFunc("GET /api/masters", s.handleListMasters)
	mux.HandleFunc("POST /api/masters", s.handleCreateMasterConfig)
	mux.HandleFunc("PUT /api/masters/{id}", s.handleUpdateMasterConfig)
	mux.HandleFunc("DELETE /api/masters/{id}", s.handleDeleteMasterConfig)
	mux.HandleFunc("GET /api/devices", s.handleListDevices)
	mux.HandleFunc("PUT /api/devices/display-names/batch", s.handleUpdateDeviceDisplayNamesBatch)
	mux.HandleFunc("GET /api/devices/{id}/detail", s.handleGetDeviceDetail)
	mux.HandleFunc("PUT /api/devices/{id}/display-name", s.handleUpdateDeviceDisplayName)
	mux.HandleFunc("POST /api/devices/{id}/commands/{command_key}/execute", s.handleExecuteDeviceCommand)
	mux.HandleFunc("POST /api/devices/{id}/em100/read-event-record", s.handleReadEM100EventRecord)
	mux.HandleFunc("POST /api/devices/{id}/em100/read-test-record", s.handleReadEM100TestRecord)
	mux.HandleFunc("GET /api/realtime/devices", s.handleListDeviceRealtime)
	mux.HandleFunc("GET /api/config/summary", s.handleGetConfigSummary)
	mux.HandleFunc("POST /api/device-templates", s.handleCreateDeviceTemplate)
	mux.HandleFunc("PUT /api/device-templates/{id}", s.handleUpdateDeviceTemplate)
	mux.HandleFunc("DELETE /api/device-templates/{id}", s.handleDeleteDeviceTemplate)
	mux.HandleFunc("PUT /api/device-templates/{id}/fields/{field_key}/realtime-display", s.handleUpdateDeviceTemplateRealtimeDisplay)
	mux.HandleFunc("PUT /api/device-templates/{id}/fields/{field_key}/history-enabled", s.handleUpdateDeviceTemplateHistoryEnabled)
	// 页面局部刷新和轮询控制 API。
	mux.HandleFunc("GET /api/view/overview-diagnosis", s.handleOverviewDiagnosisView)
	mux.HandleFunc("GET /api/view/realtime", s.handleRealtimeView)
	mux.HandleFunc("POST /api/polling/start", s.handleStartPolling)
	mux.HandleFunc("POST /api/polling/stop", s.handleStopPolling)
}
