package httpserver

// 本文件统一页面渲染、基础视图合并和模板错误处理，确保身份与运行状态在各页面口径一致。

import (
	"context"
	"encoding/json"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"

	"edge-web/internal/model"
)

func logSlowRealtimeViewBuild(elapsed time.Duration, err error) {
	if elapsed < 200*time.Millisecond {
		return
	}
	if err != nil {
		log.Printf("/api/view/realtime 构建较慢：耗时=%s，错误=%s", elapsed.Round(time.Millisecond), truncateLogText(err.Error()))
		return
	}
	log.Printf("/api/view/realtime 构建较慢：耗时=%s", elapsed.Round(time.Millisecond))
}

func truncateLogText(value string) string {
	const maxLength = 160
	value = strings.TrimSpace(value)
	if len(value) <= maxLength {
		return value
	}
	return value[:maxLength] + "..."
}

func (s *Server) basePageData(title string, activeNav string, subtitle string, r *http.Request) model.BasePageData {
	if metadata, ok := pageRouteForPath(r.URL.Path); ok {
		title = metadata.Title
		activeNav = metadata.ActiveNav
	}
	session, authenticated := s.sessionForRequest(r)
	role := normalizeRole(session.Role)
	permissions := permissionsForRole(role)
	csrfToken := session.CSRFToken
	if authenticated && csrfToken == "" {
		// 正常登录路径创建会话时已经生成令牌；这里只保留旧会话状态的兼容兜底。
		csrfToken = s.csrfTokenForRequest(r)
	}
	return model.BasePageData{
		Title:                      title,
		Subtitle:                   subtitle,
		ActiveNav:                  activeNav,
		SystemDisplayName:          defaultSystemDisplayName,
		BackendReachable:           true,
		SocketPath:                 s.socketPath,
		Authenticated:              authenticated,
		CurrentUsername:            session.Username,
		CurrentRole:                role,
		CurrentRoleText:            roleText(role),
		CSRFToken:                  csrfToken,
		IsAdmin:                    authenticated && role == roleSuperAdmin,
		CanModify:                  authenticated && permissions[permissionManageCollection],
		Permissions:                permissions,
		CanViewOverview:            authenticated && permissions[permissionViewOverview],
		CanViewRealtime:            authenticated && permissions[permissionViewRealtime],
		CanViewHistory:             authenticated && permissions[permissionViewHistory],
		CanViewEvents:              authenticated && permissions[permissionViewEvents],
		CanManageCollection:        authenticated && permissions[permissionManageCollection],
		CanManageSystemSettings:    authenticated && permissions[permissionManageSystemSettings],
		CanManageNetwork:           authenticated && permissions[permissionManageNetwork],
		CanManageMqtt:              authenticated && permissions[permissionManageMqtt],
		CanManageModbusServer:      authenticated && permissions[permissionManageModbusServer],
		CanManageModbusMappings:    authenticated && permissions[permissionManageModbusMappings],
		CanManageOperations:        authenticated && (permissions[permissionManageUsers] || permissions[permissionManageDataMaintenance] || permissions[permissionImportExportConfig] || permissions[permissionExportDiagnostics] || permissions[permissionFactoryReset] || permissions[permissionManageApplicationUpdate]),
		CanManageApplicationUpdate: authenticated && permissions[permissionManageApplicationUpdate],
		CanManageUsers:             authenticated && permissions[permissionManageUsers],
		CanManageAlarmRules:        authenticated && permissions[permissionManageAlarmRules],
		CanAckAlarm:                authenticated && permissions[permissionAckAlarm],
		CanManageDataMaintenance:   authenticated && permissions[permissionManageDataMaintenance],
		CanImportExportConfig:      authenticated && permissions[permissionImportExportConfig],
		CanExportDiagnostics:       authenticated && permissions[permissionExportDiagnostics],
		CanFactoryReset:            authenticated && permissions[permissionFactoryReset],
		CanManageDeviceTemplates:   authenticated && permissions[permissionManageDeviceTemplates],
		CanManageDeviceNames:       authenticated && permissions[permissionManageDeviceNames],
		CanExecuteDeviceCommands:   authenticated && permissions[permissionExecuteDeviceCommands],
		CurrentPath:                safeRedirectPath(r.URL.RequestURI()),
		PasswordChangeRecommended:  authenticated && session.PasswordChangeRecommended,
		Flash:                      readFlash(r),
	}
}

func applySystemDisplayName(base *model.BasePageData, settings model.SystemSettings) {
	if base == nil {
		return
	}
	displayName := strings.TrimSpace(settings.DisplayName)
	if displayName == "" {
		displayName = defaultSystemDisplayName
	}
	base.SystemDisplayName = displayName
}

func (s *Server) applySystemDisplayNameFromBackend(ctx context.Context, base *model.BasePageData) {
	displayName, err := s.console.GetSystemDisplayName(ctx)
	if err != nil {
		return
	}
	applySystemDisplayName(base, model.SystemSettings{DisplayName: displayName})
}

func (s *Server) applyPollingState(ctx context.Context, base *model.BasePageData) {
	if base == nil {
		return
	}
	summary, err := s.console.GetPollingSummary(ctx)
	if err != nil {
		return
	}
	base.PollingKnown = true
	base.PollingRunning = summary.PollingRunning
	base.PollingState = summary.PollingState
}

func mergeBasePageData(base model.BasePageData, current model.BasePageData) model.BasePageData {
	base.BackendReachable = current.BackendReachable
	base.ErrorMessage = current.ErrorMessage
	return base
}

func (s *Server) redirectWithFeedback(w http.ResponseWriter, r *http.Request, feedback model.ActionFeedback) {
	feedback.Message = userVisibleErrorMessage(feedback.Message)
	target := r.FormValue("redirect")
	if target == "" {
		target = r.Referer()
	}
	session, authenticated := s.sessionForRequest(r)
	if authenticated {
		target = safeRedirectPathForRole(target, session.Role)
	} else {
		target = safeRedirectPath(target)
	}

	u, err := url.Parse(target)
	if err != nil {
		log.Printf("解析回跳目标失败: %v", err)
		http.Redirect(w, r, "/overview", http.StatusSeeOther)
		return
	}

	query := u.Query()
	if feedback.Type == "warning" || feedback.Type == "info" {
		query.Set("flash_type", feedback.Type)
	} else if feedback.Success {
		query.Set("flash_type", "success")
	} else {
		query.Set("flash_type", "error")
	}
	query.Set("flash_message", feedback.Message)
	u.RawQuery = query.Encode()
	http.Redirect(w, r, u.String(), http.StatusSeeOther)
}

func (s *Server) renderPage(w http.ResponseWriter, page string, data interface{}) {
	tpl, ok := s.templates[page]
	if !ok {
		log.Printf("页面模板不存在: %s", page)
		http.Error(w, "页面模板不存在", http.StatusInternalServerError)
		return
	}

	// 页面包含会话身份、权限和 CSRF 令牌，禁止浏览器或中间代理缓存。
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tpl.ExecuteTemplate(w, "layout", localizeUserFacingData(data)); err != nil {
		log.Printf("页面渲染失败: %v", err)
		http.Error(w, "页面渲染失败", http.StatusInternalServerError)
	}
}

func writeResult(w http.ResponseWriter, data interface{}, err error) {
	if err != nil {
		writeError(w, http.StatusBadGateway, "backend_error", err.Error())
		return
	}

	writeSuccess(w, localizeUserFacingData(data))
}

func writeSuccess(w http.ResponseWriter, data interface{}) {
	writeJSON(w, http.StatusOK, model.APIResponse{Success: true, Data: data})
}

func writeError(w http.ResponseWriter, status int, code string, message string) {
	response := model.APIResponse{
		Success: false,
		Error: &model.APIError{
			Code:    code,
			Message: userVisibleErrorMessage(message),
		},
	}
	writeJSON(w, status, response)
}

func writeJSON(w http.ResponseWriter, status int, value interface{}) {
	// API 响应均为实时状态或当前用户操作结果，不应被共享缓存复用。
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(value); err != nil {
		log.Printf("写入 HTTP JSON 响应失败: %v", err)
	}
}

func loadResultErrorMessage(warning string) string {
	if warning != "" {
		return warning
	}
	return "主数据加载失败"
}
