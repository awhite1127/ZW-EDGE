package httpserver

// 页面元数据是登录回跳、二级页面返回、页面权限、标题和导航归属的唯一来源。
// 实际注册新页面时必须同时在此声明，避免页面可访问但无法安全回跳。
type pageRouteMetadata struct {
	Path                     string
	Title                    string
	ActiveNav                string
	AllowLoginRedirect       bool
	AllowSecondaryPageReturn bool
	AnyPermissions           []string
	DefaultFallback          string
}

var pageRoutes = map[string]pageRouteMetadata{
	"/overview": {
		Path: "/overview", Title: "系统概览", ActiveNav: "overview",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionViewOverview}, DefaultFallback: defaultAuthenticatedPath,
	},
	"/collection": {
		Path: "/collection", Title: "采集管理", ActiveNav: "collection",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionManageCollection}, DefaultFallback: defaultAuthenticatedPath,
	},
	"/history": {
		Path: "/history", Title: "历史数据", ActiveNav: "history",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionViewHistory}, DefaultFallback: defaultAuthenticatedPath,
	},
	"/device-history": {
		Path: "/device-history", Title: "设备历史数据", ActiveNav: "history",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionViewHistory}, DefaultFallback: "/history",
	},
	"/realtime": {
		Path: "/realtime", Title: "实时监控", ActiveNav: "realtime",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionViewRealtime}, DefaultFallback: defaultAuthenticatedPath,
	},
	"/events": {
		Path: "/events", Title: "事件告警", ActiveNav: "events",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionViewEvents}, DefaultFallback: defaultAuthenticatedPath,
	},
	"/settings": {
		Path: "/settings", Title: "系统设置", ActiveNav: "settings",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions:  []string{permissionManageSystemSettings, permissionManageDeviceTemplates},
		DefaultFallback: defaultAuthenticatedPath,
	},
	"/settings/device-types": {
		Path: "/settings/device-types", Title: "设备类型管理", ActiveNav: "settings",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionManageDeviceTemplates}, DefaultFallback: "/settings",
	},
	"/settings/device-types/editor": {
		Path: "/settings/device-types/editor", Title: "自定义设备类型配置", ActiveNav: "settings",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionManageDeviceTemplates}, DefaultFallback: "/settings/device-types",
	},
	"/settings/mqtt": {
		Path: "/settings/mqtt", Title: "MQTT 北向配置", ActiveNav: "settings",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionManageMqtt}, DefaultFallback: "/settings",
	},
	"/settings/modbus-server": {
		Path: "/settings/modbus-server", Title: "Modbus 北向服务", ActiveNav: "settings",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions:  []string{permissionManageModbusServer, permissionManageModbusMappings},
		DefaultFallback: "/settings",
	},
	"/operations": {
		Path: "/operations", Title: "运维管理", ActiveNav: "operations",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionExportDiagnostics}, DefaultFallback: defaultAuthenticatedPath,
	},
	"/operations/users": {
		Path: "/operations/users", Title: "用户与权限", ActiveNav: "operations",
		AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
		AnyPermissions: []string{permissionManageUsers}, DefaultFallback: "/operations",
	},
	"/upgrade-wait": {
		Path: "/upgrade-wait", Title: "应用升级", ActiveNav: "operations",
		AllowLoginRedirect: false, AllowSecondaryPageReturn: false,
		DefaultFallback: "/login",
	},
}

// pageRouteForPath 将请求路径映射为页面路由标识。
func pageRouteForPath(path string) (pageRouteMetadata, bool) {
	if isSafeCommunicationTracePath(path) {
		return pageRouteMetadata{
			Path: path, Title: "通道通讯报文", ActiveNav: "collection",
			AllowLoginRedirect: true, AllowSecondaryPageReturn: true,
			AnyPermissions: []string{permissionManageCollection}, DefaultFallback: "/collection",
		}, true
	}
	metadata, ok := pageRoutes[path]
	return metadata, ok
}

// pageRouteAllowedForRole 判断指定角色能否访问页面路由。
func pageRouteAllowedForRole(metadata pageRouteMetadata, role string) bool {
	return len(metadata.AnyPermissions) == 0 || hasAnyPermission(role, metadata.AnyPermissions...)
}
