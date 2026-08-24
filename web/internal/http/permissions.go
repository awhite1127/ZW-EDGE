// Web 权限模型：集中声明权限码、角色能力和页面/API 判定，避免授权规则散落在各 Handler。
// 前端隐藏按钮只改善交互，所有敏感操作仍必须经过本文件定义的服务端权限检查。
package httpserver

import (
	"net/http"
	"strings"
)

// 权限码作为会话、路由和页面模型之间的稳定契约，修改时必须同步检查所有调用方。
const (
	permissionViewOverview            = "view_overview"
	permissionViewRealtime            = "view_realtime"
	permissionViewHistory             = "view_history"
	permissionViewEvents              = "view_events"
	permissionAckAlarm                = "ack_alarm"
	permissionManageAlarmRules        = "manage_alarm_rules"
	permissionManageCollection        = "manage_collection"
	permissionManageDeviceTemplates   = "manage_device_templates"
	permissionManageDeviceNames       = "manage_device_names"
	permissionExecuteDeviceCommands   = "execute_device_commands"
	permissionManageSystemSettings    = "manage_system_settings"
	permissionManageNetwork           = "manage_network"
	permissionManageMqtt              = "manage_mqtt"
	permissionManageModbusServer      = "manage_modbus_server"
	permissionManageModbusMappings    = "manage_modbus_mappings"
	permissionManageDataMaintenance   = "manage_data_maintenance"
	permissionImportExportConfig      = "import_export_config"
	permissionExportDiagnostics       = "export_diagnostics"
	permissionManageUsers             = "manage_users"
	permissionFactoryReset            = "factory_reset"
	permissionManageApplicationUpdate = "manage_application_update"
)

var rolePermissions = map[string]map[string]bool{
	roleSuperAdmin: permissionSet(
		permissionViewOverview, permissionViewRealtime, permissionViewHistory, permissionViewEvents,
		permissionAckAlarm, permissionManageAlarmRules, permissionManageCollection, permissionManageDeviceTemplates,
		permissionManageDeviceNames, permissionExecuteDeviceCommands, permissionManageSystemSettings,
		permissionManageNetwork, permissionManageMqtt, permissionManageDataMaintenance, permissionImportExportConfig,
		permissionExportDiagnostics, permissionManageUsers, permissionFactoryReset,
		permissionManageModbusServer, permissionManageModbusMappings,
		permissionManageApplicationUpdate,
	),
	roleEngineer: permissionSet(
		permissionViewOverview, permissionViewRealtime, permissionViewHistory, permissionViewEvents,
		permissionAckAlarm, permissionManageAlarmRules, permissionManageCollection, permissionManageDeviceTemplates,
		permissionManageDeviceNames, permissionExportDiagnostics,
		permissionManageModbusMappings,
	),
	roleOperator: permissionSet(
		permissionViewOverview, permissionViewRealtime, permissionViewHistory, permissionViewEvents,
		permissionAckAlarm, permissionExportDiagnostics,
	),
	roleViewer: permissionSet(
		permissionViewOverview, permissionViewRealtime, permissionViewHistory, permissionViewEvents,
	),
}

// permissionSet 将权限列表转换为快速查询集合。
func permissionSet(values ...string) map[string]bool {
	result := make(map[string]bool, len(values))
	for _, value := range values {
		result[value] = true
	}
	return result
}

// permissionsForRole 返回新的权限集合，调用方可以安全读取而不会修改全局角色定义。
func permissionsForRole(role string) map[string]bool {
	source := rolePermissions[normalizeRole(role)]
	result := make(map[string]bool, len(source))
	for permission, allowed := range source {
		result[permission] = allowed
	}
	return result
}

// hasPermission 判断是否具有权限。
func hasPermission(role string, permission string) bool {
	return rolePermissions[normalizeRole(role)][permission]
}

// hasAnyPermission 判断是否具有任一权限。
func hasAnyPermission(role string, permissions ...string) bool {
	for _, permission := range permissions {
		if hasPermission(role, permission) {
			return true
		}
	}
	return false
}

// requirePermission 校验权限。
func (s *Server) requirePermission(w http.ResponseWriter, r *http.Request, permission string) bool {
	session, authenticated := s.sessionForRequest(r)
	if !authenticated {
		writeError(w, http.StatusUnauthorized, "unauthorized", "请先登录后再操作")
		return false
	}
	if hasPermission(session.Role, permission) {
		return true
	}
	message := "当前账户无权执行此操作"
	if wantsJSON(r) || strings.HasPrefix(r.URL.Path, "/api/") {
		writeError(w, http.StatusForbidden, "forbidden", message)
		return false
	}
	http.Error(w, message, http.StatusForbidden)
	return false
}

// pagePermission 返回页面路由要求的权限。
func pagePermission(path string) string {
	metadata, ok := pageRouteForPath(path)
	if !ok || len(metadata.AnyPermissions) != 1 {
		return ""
	}
	return metadata.AnyPermissions[0]
}

var apiGETPermissionMux = func() *http.ServeMux {
	mux := http.NewServeMux()
	for _, spec := range apiGETRoutes {
		mux.HandleFunc(spec.pattern, func(http.ResponseWriter, *http.Request) {})
	}
	return mux
}()

func apiGETRouteForRequest(r *http.Request) (*apiGETRouteSpec, bool) {
	if r == nil || r.Method != http.MethodGet || !strings.HasPrefix(r.URL.Path, "/api/") {
		return nil, false
	}
	_, pattern := apiGETPermissionMux.Handler(r)
	if pattern == "" {
		return nil, false
	}
	for index := range apiGETRoutes {
		if apiGETRoutes[index].pattern == pattern {
			return &apiGETRoutes[index], true
		}
	}
	return nil, false
}

// apiGETPermissions 使用与实际路由相同的清单匹配只读 API。
// 返回 declared=false 表示路由没有权限声明，authMiddleware 会默认拒绝。
func apiGETPermissions(r *http.Request) (permissions []string, declared bool) {
	spec, declared := apiGETRouteForRequest(r)
	if !declared {
		return nil, false
	}
	return spec.anyPermissions, true
}

// requestPermission 返回接口请求要求的权限。
func requestPermission(r *http.Request) string {
	path := r.URL.Path
	method := r.Method
	if method == http.MethodGet {
		switch {
		case path == "/history/export.csv":
			return permissionViewHistory
		case path == "/events/export.csv":
			return permissionViewEvents
		case path == "/events/diagnostics/export":
			return permissionExportDiagnostics
		case path == "/settings/config/export":
			return permissionImportExportConfig
		}
		return ""
	}
	if method == http.MethodPost && (path == "/logout" || path == "/account/password") {
		return ""
	}
	switch {
	case strings.HasPrefix(path, "/api/application-update/"):
		return permissionManageApplicationUpdate
	case path == "/account/user-password":
		return permissionManageUsers
	case path == "/settings":
		return permissionManageSystemSettings
	case strings.HasPrefix(path, "/settings/network"):
		return permissionManageNetwork
	case strings.HasPrefix(path, "/settings/time"):
		return permissionManageSystemSettings
	case path == "/settings/mqtt":
		return permissionManageMqtt
	case path == "/api/modbus-server/settings":
		return permissionManageModbusServer
	case strings.HasPrefix(path, "/api/modbus-server/mappings"):
		return permissionManageModbusMappings
	case path == "/settings/config/import":
		return permissionImportExportConfig
	case path == "/settings/factory-reset":
		return permissionFactoryReset
	case path == "/actions/history/cleanup-expired":
		return permissionManageDataMaintenance
	case path == "/actions/alarms/acknowledge":
		return permissionAckAlarm
	case strings.HasPrefix(path, "/actions/alarms/rules/"):
		return permissionManageAlarmRules
	case path == "/actions/events/clear":
		return permissionManageDataMaintenance
	case path == "/api/polling/start" || path == "/api/polling/stop":
		return permissionManageCollection
	case strings.HasPrefix(path, "/api/channels"):
		return permissionManageCollection
	case strings.HasPrefix(path, "/api/masters"):
		return permissionManageCollection
	case path == "/api/devices/display-names/batch" || strings.HasSuffix(path, "/display-name"):
		return permissionManageDeviceNames
	case strings.Contains(path, "/commands/") || strings.Contains(path, "/read-event-record") || strings.Contains(path, "/read-test-record"):
		return permissionExecuteDeviceCommands
	case strings.HasPrefix(path, "/api/device-templates"):
		return permissionManageDeviceTemplates
	case strings.HasPrefix(path, "/operations/users"):
		return permissionManageUsers
	}
	return ""
}
