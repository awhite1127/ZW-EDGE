// 运维管理 HTTP 入口：组装用户、权限、数据维护、配置导入导出和诊断导出等管理操作。
// Handler 只负责鉴权后的请求编排与页面反馈，业务数据仍由 ConsoleService 和后端 IPC 提供。
package httpserver

import (
	"net/http"
	"strings"
	"sync"

	"edge-web/internal/model"
)

// 运维首页允许部分区块独立降级，单个后端查询失败不阻止其余管理入口展示。
func (s *Server) handleOperationsPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	var pageData model.OperationsPageData
	var settings model.SystemSettings
	var settingsErr error
	var wait sync.WaitGroup
	wait.Add(2)
	go func() {
		defer wait.Done()
		// 升级模块按需初始化，首屏不再读取版本和完整状态。
		pageData = s.console.LoadOperations(ctx, rolePermissionViews())
	}()
	go func() {
		defer wait.Done()
		settings, settingsErr = s.console.GetSystemSettings(ctx)
	}()
	wait.Wait()
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"运维管理",
			"operations",
			"管理用户权限、数据维护、配置备份和现场诊断。",
			r,
		),
		pageData.BasePageData,
	)
	if settingsErr == nil {
		applySystemDisplayName(&pageData.BasePageData, settings)
	}
	s.renderPage(w, "operations", pageData)
}

// handleOperationsUsersPage 处理运维用户页面请求。
func (s *Server) handleOperationsUsersPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := s.console.LoadOperations(ctx, rolePermissionViews())
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"用户与权限",
			"operations",
			"管理固定角色用户、启用状态、密码重置和权限说明。",
			r,
		),
		pageData.BasePageData,
	)
	s.applySystemDisplayNameFromBackend(ctx, &pageData.BasePageData)
	s.renderPage(w, "operations_users", pageData)
}

// handleCreateWebUserAction 处理Web用户创建请求。
func (s *Server) handleCreateWebUserAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "创建用户请求格式不正确"})
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	role := strings.TrimSpace(r.FormValue("role"))
	if normalizeRole(role) == roleSuperAdmin {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "不允许创建新的超级管理员"})
		return
	}
	s.redirectWithFeedback(w, r, s.console.CreateWebUser(ctx, model.WebUserCreateRequest{
		Username:    strings.TrimSpace(r.FormValue("username")),
		DisplayName: strings.TrimSpace(r.FormValue("display_name")),
		Role:        role,
		Password:    r.FormValue("password"),
		Enabled:     formCheckbox(r, "enabled"),
	}))
}

// handleUpdateWebUserAction 处理Web用户更新请求。
func (s *Server) handleUpdateWebUserAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "更新用户请求格式不正确"})
		return
	}
	session, _ := s.sessionForRequest(r)
	username := strings.TrimSpace(r.FormValue("username"))
	role := strings.TrimSpace(r.FormValue("role"))
	enabled := formCheckbox(r, "enabled")
	if username == session.Username && !enabled {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "当前用户不能禁用自己"})
		return
	}
	if username == session.Username && normalizeRole(role) != roleSuperAdmin && normalizeRole(session.Role) == roleSuperAdmin {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "不能将当前超级管理员降级"})
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	users, err := s.console.ListWebUsers(ctx)
	if err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "无法确认超级管理员数量，已取消操作"})
		return
	}
	previousRole, previousEnabled := currentUserRoleAndEnabled(users, username)
	if normalizeRole(role) == roleSuperAdmin && normalizeRole(previousRole) != roleSuperAdmin {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "不允许将普通用户提升为超级管理员"})
		return
	}
	if wouldRemoveLastSuperAdmin(users, username, role, enabled) {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "不能禁用或降级最后一个超级管理员"})
		return
	}
	feedback := s.console.UpdateWebUser(ctx, model.WebUserUpdateRequest{
		Username:    username,
		DisplayName: strings.TrimSpace(r.FormValue("display_name")),
		Role:        role,
		Enabled:     enabled,
	})
	if feedback.Success && (normalizeRole(previousRole) != normalizeRole(role) || previousEnabled != enabled) {
		s.invalidateSessionsForUsername(username, "")
	}
	s.redirectWithFeedback(w, r, feedback)
}

// wouldRemoveLastSuperAdmin 判断是否会移除最后超级管理员。
func wouldRemoveLastSuperAdmin(users []model.WebUser, target string, nextRole string, nextEnabled bool) bool {
	activeSuperAdmins := 0
	for _, user := range users {
		role := normalizeRole(user.Role)
		enabled := user.Enabled
		if user.Username == target {
			role = normalizeRole(nextRole)
			enabled = nextEnabled
		}
		if role == roleSuperAdmin && enabled {
			activeSuperAdmins++
		}
	}
	return activeSuperAdmins == 0
}

// currentUserRoleAndEnabled 获取当前用户角色与启用状态。
func currentUserRoleAndEnabled(users []model.WebUser, username string) (string, bool) {
	for _, user := range users {
		if user.Username == username {
			return user.Role, user.Enabled
		}
	}
	return "", false
}

// handleResetWebUserPasswordAction 处理重置Web用户密码请求。
func (s *Server) handleResetWebUserPasswordAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "重置密码请求格式不正确"})
		return
	}
	password := r.FormValue("new_password")
	if message := validatePasswordChangeForm("not-empty", password, r.FormValue("confirm_password"), "当前密码", "新密码", false); message != "" {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: message})
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback := s.console.ResetWebUserPassword(ctx, model.WebUserPasswordResetRequest{
		Username:    strings.TrimSpace(r.FormValue("username")),
		NewPassword: password,
	})
	if feedback.Success {
		s.invalidateSessionsForUsername(strings.TrimSpace(r.FormValue("username")), "")
	}
	s.redirectWithFeedback(w, r, feedback)
}

// rolePermissionViews 构造各角色的权限展示列表。
func rolePermissionViews() []model.RolePermissionView {
	permissions := []model.RolePermissionItem{
		{Key: permissionManageUsers, Name: "用户管理"},
		{Key: permissionManageSystemSettings, Name: "系统设置"},
		{Key: permissionManageCollection, Name: "采集配置"},
		{Key: permissionManageDeviceTemplates, Name: "设备类型"},
		{Key: permissionManageDeviceNames, Name: "设备命名"},
		{Key: permissionManageAlarmRules, Name: "告警规则"},
		{Key: permissionExecuteDeviceCommands, Name: "设备操作"},
		{Key: permissionAckAlarm, Name: "确认告警"},
		{Key: permissionManageDataMaintenance, Name: "数据维护"},
		{Key: permissionImportExportConfig, Name: "配置备份恢复"},
		{Key: permissionExportDiagnostics, Name: "导出诊断包"},
		{Key: permissionFactoryReset, Name: "恢复出厂"},
		{Key: permissionManageModbusServer, Name: "Modbus 服务设置"},
		{Key: permissionManageModbusMappings, Name: "Modbus 寄存器映射"},
		{Key: permissionManageApplicationUpdate, Name: "应用升级"},
	}
	roles := []struct {
		key, name, description string
	}{
		{roleSuperAdmin, "超级管理员", "拥有全部配置、运维和用户管理权限。"},
		{roleEngineer, "工程师", "维护采集、设备类型、告警规则、Modbus 寄存器映射和现场诊断。"},
		{roleOperator, "操作员", "查看运行数据、确认告警和导出诊断包。"},
		{roleViewer, "只读用户", "只能查看概览、实时、历史和事件。"},
	}
	result := make([]model.RolePermissionView, 0, len(roles))
	for _, role := range roles {
		items := make([]model.RolePermissionItem, 0, len(permissions))
		for _, permission := range permissions {
			permission.Allowed = hasPermission(role.key, permission.Key)
			items = append(items, permission)
		}
		result = append(result, model.RolePermissionView{
			Role:        role.key,
			Name:        role.name,
			Description: role.description,
			Permissions: items,
		})
	}
	return result
}
