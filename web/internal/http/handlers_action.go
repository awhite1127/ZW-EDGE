package httpserver

// 本文件处理会改变状态的表单动作；请求校验、CSRF 和角色限制在进入后端 IPC 前完成。

import (
	"context"
	"log"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"edge-web/internal/model"
)

func (s *Server) handleLoginAction(w http.ResponseWriter, r *http.Request) {
	// 登录请求只接受小体积表单，避免认证接口被超大 body 拖慢。
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if err := r.ParseForm(); err != nil {
		s.redirectLoginWithError(w, r, "登录请求格式不正确")
		return
	}

	clientKey := loginAttemptKey(r.RemoteAddr)
	if s.loginAttempts.isLocked(clientKey) {
		log.Printf("登录被临时锁定拦截：client=%s", clientKey)
		s.redirectLoginWithError(w, r, "登录失败次数过多，请稍后再试")
		return
	}

	username := strings.TrimSpace(r.FormValue("username"))
	password := r.FormValue("password")
	if len(password) > maxWebPasswordBytes {
		// 超长密码无需传给后端哈希验证，直接计入失败，降低异常输入成本。
		s.rejectFailedLogin(w, r, clientKey, username)
		return
	}
	ctx, cancel := s.withConsoleTimeout(r.Context())
	defer cancel()

	loginResult, err := s.verifyLogin(ctx, model.WebLoginRequest{
		Username: username,
		Password: password,
	})
	if err != nil {
		log.Printf("登录验证服务失败：client=%s err=%s", clientKey, truncateLogText(err.Error()))
		s.redirectLoginWithError(w, r, "登录服务暂时不可用，请稍后再试")
		return
	}
	if !loginResult.Success {
		s.rejectFailedLogin(w, r, clientKey, username)
		return
	}
	if !validLoginIdentity(loginResult.Username, loginResult.Role) {
		log.Printf("登录验证返回未知身份：client=%s username=%s role=%s", clientKey, loginResult.Username, loginResult.Role)
		s.redirectLoginWithError(w, r, "登录服务暂时不可用，请稍后再试")
		return
	}
	loginResult.Role = normalizeRole(loginResult.Role)

	token, err := newSessionToken()
	if err != nil {
		s.redirectLoginWithError(w, r, "创建登录会话失败")
		return
	}
	csrfToken, err := newSessionToken()
	if err != nil {
		s.redirectLoginWithError(w, r, "创建页面安全令牌失败")
		return
	}

	s.loginAttempts.clear(clientKey)

	now := time.Now()
	expiresAt := sessionExpiration(loginResult.Role, now)
	s.sessionMu.Lock()
	s.pruneSessionsForInsertLocked(now)
	s.sessions[token] = sessionState{
		Username:                  loginResult.Username,
		Role:                      loginResult.Role,
		CSRFToken:                 csrfToken,
		ExpiresAt:                 expiresAt,
		LastSeenAt:                now,
		PasswordChangeRecommended: loginResult.PasswordChangeRecommended,
	}
	s.sessionMu.Unlock()

	sessionCookie := &http.Cookie{
		Name:     sessionCookieName,
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		Secure:   shouldUseSecureCookie(r),
		SameSite: http.SameSiteLaxMode,
	}
	if !expiresAt.IsZero() {
		sessionCookie.Expires = expiresAt
	}
	http.SetCookie(w, sessionCookie)
	http.Redirect(w, r, safeRedirectPathForRole(r.FormValue("redirect"), loginResult.Role), http.StatusSeeOther)
}

func (s *Server) handleFirstBootAdminEntryAction(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if err := r.ParseForm(); err != nil {
		s.redirectLoginWithError(w, r, "首次部署请求格式不正确")
		return
	}

	// 会话令牌先生成，但在后端原子消费一次性入口成功前不创建任何登录态。
	token, err := newSessionToken()
	if err != nil {
		s.redirectLoginWithError(w, r, "创建登录会话失败")
		return
	}
	csrfToken, err := newSessionToken()
	if err != nil {
		s.redirectLoginWithError(w, r, "创建页面安全令牌失败")
		return
	}
	if s.consumeFirstBootAdminEntry == nil {
		s.redirectLoginWithError(w, r, "首次部署入口暂时不可用")
		return
	}

	ctx, cancel := s.withConsoleTimeout(r.Context())
	result, consumeErr := s.consumeFirstBootAdminEntry(ctx)
	cancel()
	if consumeErr != nil {
		log.Printf("消费首次部署入口失败：client=%s err=%s", loginAttemptKey(r.RemoteAddr), truncateLogText(consumeErr.Error()))
		s.redirectLoginWithError(w, r, "首次部署入口暂时不可用，请稍后再试")
		return
	}
	if !result.Consumed {
		s.redirectLoginWithError(w, r, "首次部署入口已关闭，请使用账号密码登录")
		return
	}

	clientKey := loginAttemptKey(r.RemoteAddr)
	s.loginAttempts.clear(clientKey)
	now := time.Now()
	expiresAt := sessionExpiration(roleSuperAdmin, now)
	s.sessionMu.Lock()
	s.pruneSessionsForInsertLocked(now)
	s.sessions[token] = sessionState{
		Username:                  "admin",
		Role:                      roleSuperAdmin,
		CSRFToken:                 csrfToken,
		ExpiresAt:                 expiresAt,
		LastSeenAt:                now,
		PasswordChangeRecommended: true,
	}
	s.sessionMu.Unlock()

	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookieName,
		Value:    token,
		Path:     "/",
		Expires:  expiresAt,
		HttpOnly: true,
		Secure:   shouldUseSecureCookie(r),
		SameSite: http.SameSiteLaxMode,
	})
	http.Redirect(w, r, safeRedirectPathForRole(r.FormValue("redirect"), roleSuperAdmin), http.StatusSeeOther)
}

// rejectFailedLogin 记录登录失败并返回统一错误。
func (s *Server) rejectFailedLogin(
	w http.ResponseWriter,
	r *http.Request,
	clientKey string,
	username string,
) {
	failures, locked := s.loginAttempts.recordFailure(clientKey)
	log.Printf("登录失败：client=%s attempts=%d username_empty=%t", clientKey, failures, username == "")
	if locked {
		log.Printf("登录失败次数过多，进入临时锁定：client=%s attempts=%d duration=%s", clientKey, failures, loginLockDuration)
		s.redirectLoginWithError(w, r, "登录失败次数过多，请稍后再试")
		return
	}
	s.redirectLoginWithError(w, r, "账号或密码错误")
}

func (s *Server) handleLogoutAction(w http.ResponseWriter, r *http.Request) {
	if cookie, err := r.Cookie(sessionCookieName); err == nil {
		s.sessionMu.Lock()
		delete(s.sessions, cookie.Value)
		s.sessionMu.Unlock()
	}

	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookieName,
		Value:    "",
		Path:     "/",
		Expires:  time.Unix(0, 0),
		MaxAge:   -1,
		HttpOnly: true,
		Secure:   shouldUseSecureCookie(r),
		SameSite: http.SameSiteLaxMode,
	})
	http.Redirect(w, r, "/login?flash_type=success&flash_message="+url.QueryEscape("已退出登录"), http.StatusSeeOther)
}

func (s *Server) handleClearEventsAction(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	s.redirectWithFeedback(w, r, s.console.ClearRecentEvents(ctx))
}

func (s *Server) handleCleanupExpiredDataAction(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	s.redirectWithFeedback(w, r, s.console.CleanupExpiredData(ctx))
}

func (s *Server) handleSaveAlarmRuleAction(w http.ResponseWriter, r *http.Request) {
	if err := parseActionForm(r); err != nil {
		s.respondAlarmFeedback(w, r, model.ActionFeedback{Success: false, Message: "告警规则请求格式不正确"}, http.StatusBadRequest)
		return
	}
	request, feedback := parseAlarmRuleForm(r)
	if !feedback.Success {
		s.respondAlarmFeedback(w, r, feedback, http.StatusBadRequest)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback = s.console.SaveMasterAlarmRule(ctx, request)
	s.respondAlarmFeedback(w, r, feedback, alarmRuleFeedbackStatus(feedback))
}

func (s *Server) handleDisableAlarmRuleAction(w http.ResponseWriter, r *http.Request) {
	if err := parseActionForm(r); err != nil {
		s.respondAlarmFeedback(w, r, model.ActionFeedback{Success: false, Message: "告警规则请求格式不正确"}, http.StatusBadRequest)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback := s.console.DisableMasterAlarmRule(ctx, r.FormValue("master_id"), r.FormValue("point_key"))
	s.respondAlarmFeedback(w, r, feedback, alarmRuleFeedbackStatus(feedback))
}

func (s *Server) handleDeleteAlarmRuleAction(w http.ResponseWriter, r *http.Request) {
	if err := parseActionForm(r); err != nil {
		s.respondAlarmFeedback(w, r, model.ActionFeedback{Success: false, Message: "告警规则请求格式不正确"}, http.StatusBadRequest)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback := s.console.DeleteMasterAlarmRule(ctx, r.FormValue("master_id"), r.FormValue("point_key"))
	s.respondAlarmFeedback(w, r, feedback, alarmRuleFeedbackStatus(feedback))
}

func (s *Server) handleAcknowledgeActiveAlarmAction(w http.ResponseWriter, r *http.Request) {
	if err := parseActionForm(r); err != nil {
		s.respondAlarmFeedback(w, r, model.ActionFeedback{Success: false, Message: "告警确认请求格式不正确"}, http.StatusBadRequest)
		return
	}
	activeSinceMS, err := strconv.ParseUint(strings.TrimSpace(r.FormValue("active_since_ms")), 10, 64)
	if err != nil || activeSinceMS == 0 {
		s.respondAlarmFeedback(w, r, model.ActionFeedback{Success: false, Message: "当前告警标识无效，请刷新后重试"}, http.StatusBadRequest)
		return
	}
	session, ok := s.sessionForRequest(r)
	if !ok || strings.TrimSpace(session.Username) == "" {
		s.respondAlarmFeedback(w, r, model.ActionFeedback{Success: false, Message: "登录状态已失效，请重新登录"}, http.StatusUnauthorized)
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback := s.console.AcknowledgeActiveAlarm(
		ctx, r.FormValue("device_id"), r.FormValue("point_key"), session.Username, activeSinceMS)
	status := http.StatusOK
	if !feedback.Success {
		status = http.StatusBadRequest
	}
	s.respondAlarmFeedback(w, r, feedback, status)
}

func parseAlarmRuleForm(r *http.Request) (model.MasterAlarmRuleRequest, model.ActionFeedback) {
	// 表单校验在 HTTP 层先给出字段级提示；更完整的业务边界仍由 ConsoleService 复核。
	masterID := strings.TrimSpace(r.FormValue("master_id"))
	if masterID == "" {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: "请选择主站"}
	}
	pointKey := strings.TrimSpace(r.FormValue("point_key"))
	if pointKey == "" {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: "请选择数据项"}
	}
	parseFloat := func(name string, required bool) (float64, bool, string) {
		raw := strings.TrimSpace(r.FormValue(name))
		if raw == "" {
			if required {
				return 0, false, "请填写" + alarmRuleFieldLabel(name)
			}
			return 0, true, ""
		}
		value, err := strconv.ParseFloat(raw, 64)
		if err != nil {
			return 0, false, alarmRuleFieldLabel(name) + "必须是有效数字"
		}
		return value, true, ""
	}
	parseCount := func(name string) (uint32, bool, string) {
		raw := strings.TrimSpace(r.FormValue(name))
		if raw == "" {
			return 1, true, ""
		}
		value, err := strconv.ParseUint(raw, 10, 32)
		if err != nil {
			return 0, false, alarmRuleFieldLabel(name) + "必须是 1～100 的整数"
		}
		return uint32(value), true, ""
	}

	highEnabled := formCheckboxOn(r, "high_enabled")
	lowEnabled := formCheckboxOn(r, "low_enabled")
	high, ok, message := parseFloat("high_threshold", highEnabled)
	if !ok {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: message}
	}
	low, ok, message := parseFloat("low_threshold", lowEnabled)
	if !ok {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: message}
	}
	hysteresis, ok, message := parseFloat("hysteresis", false)
	if !ok {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: message}
	}
	triggerCount, ok, message := parseCount("trigger_count")
	if !ok {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: message}
	}
	recoveryCount, ok, message := parseCount("recovery_count")
	if !ok {
		return model.MasterAlarmRuleRequest{}, model.ActionFeedback{Success: false, Message: message}
	}

	return model.MasterAlarmRuleRequest{
		MasterID:      masterID,
		PointKey:      pointKey,
		Enabled:       formCheckboxOn(r, "enabled"),
		HighEnabled:   highEnabled,
		HighThreshold: high,
		LowEnabled:    lowEnabled,
		LowThreshold:  low,
		Level:         strings.TrimSpace(r.FormValue("level")),
		Hysteresis:    hysteresis,
		TriggerCount:  triggerCount,
		RecoveryCount: recoveryCount,
	}, model.ActionFeedback{Success: true}
}

func parseActionForm(r *http.Request) error {
	contentType := strings.ToLower(strings.TrimSpace(r.Header.Get("Content-Type")))
	if strings.HasPrefix(contentType, "multipart/form-data") {
		return r.ParseMultipartForm(1 << 20)
	}
	return r.ParseForm()
}

func alarmRuleFeedbackStatus(feedback model.ActionFeedback) int {
	if feedback.Success {
		return http.StatusOK
	}
	if isAlarmRuleClientError(userVisibleErrorMessage(feedback.Message)) {
		return http.StatusBadRequest
	}
	return http.StatusBadGateway
}

func isAlarmRuleClientError(message string) bool {
	switch message {
	case "主站不存在",
		"该主站下没有可应用的设备",
		"主站和数据项不能为空",
		"暂无可停用规则",
		"暂无可删除规则":
		return true
	}
	return strings.HasPrefix(message, "请选择") ||
		strings.HasPrefix(message, "请填写") ||
		strings.Contains(message, "必须") ||
		strings.Contains(message, "不能") ||
		strings.Contains(message, "无效") ||
		strings.Contains(message, "至少需要")
}

func (s *Server) respondAlarmFeedback(w http.ResponseWriter, r *http.Request, feedback model.ActionFeedback, status int) {
	// 告警配置既支持普通表单回跳，也支持弹窗 AJAX 保存，响应格式由请求头决定。
	if !wantsJSON(r) {
		s.redirectWithFeedback(w, r, feedback)
		return
	}
	message := userVisibleErrorMessage(feedback.Message)
	if feedback.Success {
		writeSuccess(w, map[string]string{"message": message})
		return
	}
	if status < 400 {
		status = http.StatusBadGateway
	}
	writeJSON(w, status, model.APIResponse{
		Success: false,
		Error: &model.APIError{
			Code:    "alarm_rule_failed",
			Message: message,
		},
	})
}

func formCheckboxOn(r *http.Request, name string) bool {
	value := strings.ToLower(strings.TrimSpace(r.FormValue(name)))
	return value == "on" || value == "true" || value == "1"
}

func alarmRuleFieldLabel(name string) string {
	switch name {
	case "high_threshold":
		return "上限值"
	case "low_threshold":
		return "下限值"
	case "hysteresis":
		return "回差"
	case "trigger_count":
		return "连续触发次数"
	case "recovery_count":
		return "连续恢复次数"
	default:
		return "参数"
	}
}

func (s *Server) handleSettingsAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "系统设置请求格式不正确"})
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	feedback := s.console.UpdateSystemSettings(ctx, model.SystemSettingsUpdateRequest{
		DeviceName:   r.FormValue("device_name"),
		SiteLocation: r.FormValue("site_location"),
		DisplayName:  r.FormValue("display_name"),
	})
	s.redirectWithFeedback(w, r, feedback)
}

func (s *Server) handleAccountPasswordAction(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "账户密码请求格式不正确"})
		return
	}
	session, authenticated := s.sessionForRequest(r)
	if !authenticated {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "登录状态已失效，请重新登录"})
		return
	}

	currentPassword := r.FormValue("current_password")
	newPassword := r.FormValue("new_password")
	confirmPassword := r.FormValue("confirm_new_password")
	if message := validatePasswordChangeForm(currentPassword, newPassword, confirmPassword, "当前密码", "新密码", true); message != "" {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: message})
		return
	}

	ctx, cancel := s.withConsoleTimeout(r.Context())
	defer cancel()

	feedback := s.changeAccountPassword(ctx, model.WebPasswordChangeRequest{
		Username:        session.Username,
		CurrentPassword: currentPassword,
		NewPassword:     newPassword,
	})
	if feedback.Success {
		s.completeOwnPasswordChange(r, session.Username)
	}
	s.redirectWithFeedback(w, r, feedback)
}

func (s *Server) handleUserPasswordAction(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "只读用户密码请求格式不正确"})
		return
	}
	session, authenticated := s.sessionForRequest(r)
	if !authenticated || !hasPermission(session.Role, permissionManageUsers) {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "仅管理员可以设置只读用户密码"})
		return
	}

	adminPassword := r.FormValue("admin_password")
	newPassword := r.FormValue("new_user_password")
	confirmPassword := r.FormValue("confirm_user_password")
	if message := validatePasswordChangeForm(adminPassword, newPassword, confirmPassword, "管理员当前密码", "user 新密码", false); message != "" {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: message})
		return
	}

	ctx, cancel := s.withConsoleTimeout(r.Context())
	defer cancel()
	feedback := s.setUserPassword(ctx, model.WebViewerPasswordSetRequest{
		AdminPassword:   adminPassword,
		NewUserPassword: newPassword,
	})
	if feedback.Success {
		s.invalidateSessionsForUsername("user", "")
	}
	s.redirectWithFeedback(w, r, feedback)
}

func validatePasswordChangeForm(currentPassword string, newPassword string, confirmPassword string, currentLabel string, newLabel string, compareCurrent bool) string {
	// 这里做前置体验校验；最终密码正确性和持久化仍交给后端认证服务。
	if strings.TrimSpace(currentPassword) == "" {
		return "请输入" + currentLabel
	}
	if len(currentPassword) > maxWebPasswordBytes || len(newPassword) > maxWebPasswordBytes {
		return "密码长度不能超过 256 字节"
	}
	if len(newPassword) < 8 {
		return newLabel + "长度至少 8 位"
	}
	if strings.TrimSpace(newPassword) == "" {
		return newLabel + "不能全为空白字符"
	}
	if compareCurrent && newPassword == currentPassword {
		return newLabel + "不能与当前密码相同"
	}
	if newPassword != confirmPassword {
		return "两次输入的" + newLabel + "不一致"
	}
	return ""
}

func (s *Server) handleNetworkSettingsAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "网络配置请求格式不正确"})
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	feedback := s.console.SaveAndApplyNetworkSettings(ctx, model.NetworkSettingsUpdateRequest{
		Mode:          strings.TrimSpace(r.FormValue("mode")),
		InterfaceName: r.FormValue("interface_name"),
		IPAddress:     r.FormValue("ip_address"),
		Netmask:       r.FormValue("netmask"),
		Gateway:       r.FormValue("gateway"),
		DNSServers:    parseDNSServersInput(r.FormValue("dns_servers")),
	})
	s.redirectWithFeedback(w, r, feedback)
}

func (s *Server) handleTimeSettingsSaveApplyAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "日期与时间请求格式不正确"})
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback := s.console.SaveAndApplyTimeSettings(ctx, model.TimeSettingsUpdateRequest{
		Timezone:     strings.TrimSpace(r.FormValue("timezone")),
		NTPEnabled:   formCheckbox(r, "ntp_enabled"),
		NTPPrimary:   strings.TrimSpace(r.FormValue("ntp_primary")),
		NTPSecondary: strings.TrimSpace(r.FormValue("ntp_secondary")),
	})
	s.redirectWithFeedback(w, r, feedback)
}

func (s *Server) handleTimeSyncNowAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "立即同步请求格式不正确"})
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 85*time.Second)
	defer cancel()
	s.redirectWithFeedback(w, r, s.console.SyncTimeNow(ctx))
}

func (s *Server) handleManualTimeAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "手动校时请求格式不正确"})
		return
	}
	epochMS, err := strconv.ParseUint(strings.TrimSpace(r.FormValue("epoch_ms")), 10, 64)
	if err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "手动时间参数无效"})
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	source := strings.TrimSpace(r.FormValue("source"))
	if source == "" {
		source = "manual"
	}
	s.redirectWithFeedback(w, r, s.console.SetManualSystemTime(ctx, epochMS, source))
}

func (s *Server) handleNetworkSettingsSaveApplyAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.respondNetworkFeedback(w, r, model.ActionFeedback{Success: false, Message: "网络配置请求格式不正确"}, http.StatusBadRequest)
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	feedback := s.console.SaveAndApplyNetworkSettings(ctx, model.NetworkSettingsUpdateRequest{
		Mode:          strings.TrimSpace(r.FormValue("mode")),
		InterfaceName: r.FormValue("interface_name"),
		IPAddress:     r.FormValue("ip_address"),
		Netmask:       r.FormValue("netmask"),
		Gateway:       r.FormValue("gateway"),
		DNSServers:    parseDNSServersInput(r.FormValue("dns_servers")),
	})
	status := http.StatusOK
	if !feedback.Success {
		status = http.StatusBadGateway
	}
	s.respondNetworkFeedback(w, r, feedback, status)
}

func (s *Server) respondNetworkFeedback(w http.ResponseWriter, r *http.Request, feedback model.ActionFeedback, status int) {
	if !wantsJSON(r) {
		s.redirectWithFeedback(w, r, feedback)
		return
	}

	message := userVisibleErrorMessage(feedback.Message)
	if feedback.Success {
		writeSuccess(w, map[string]string{"message": message})
		return
	}
	if status < 400 {
		status = http.StatusBadGateway
	}
	writeJSON(w, status, model.APIResponse{
		Success: false,
		Error: &model.APIError{
			Code:    "network_save_apply_failed",
			Message: message,
		},
	})
}

func wantsJSON(r *http.Request) bool {
	return strings.Contains(r.Header.Get("Accept"), "application/json") ||
		strings.EqualFold(r.Header.Get("X-Requested-With"), "XMLHttpRequest")
}

func parseDNSServersInput(value string) []string {
	parts := strings.Split(value, ",")
	result := make([]string, 0, len(parts))
	for _, part := range parts {
		normalized := strings.TrimSpace(part)
		if normalized != "" {
			result = append(result, normalized)
		}
	}
	return result
}

func (s *Server) handleMqttSettingsAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "MQTT 配置请求格式不正确"})
		return
	}

	request, parseMessage := parseMqttSettingsForm(r)
	if parseMessage != "" {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: parseMessage})
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	feedback := s.console.UpdateMqttSettings(ctx, request)
	s.redirectWithFeedback(w, r, feedback)
}

func parseMqttSettingsForm(r *http.Request) (model.MqttSettingsUpdateRequest, string) {
	brokerPort, ok := parseUint16Form(r, "broker_port")
	if !ok {
		return model.MqttSettingsUpdateRequest{}, "Broker 端口必须是 1 到 65535 的整数"
	}
	publishInterval, ok := parseUint32Form(r, "publish_interval_seconds")
	if !ok || publishInterval < 1 || publishInterval > 3600 {
		return model.MqttSettingsUpdateRequest{}, "发布周期必须是 1 到 3600 的整数"
	}
	keepAlive, ok := parseUint32Form(r, "keep_alive_seconds")
	if !ok || keepAlive < 10 || keepAlive > 300 {
		return model.MqttSettingsUpdateRequest{}, "Keep Alive 必须是 10 到 300 的整数"
	}
	qos, ok := parseUint32Form(r, "qos")
	if !ok || qos > 1 {
		return model.MqttSettingsUpdateRequest{}, "QoS 只支持 0 或 1"
	}
	return model.MqttSettingsUpdateRequest{
		Enabled:                formCheckbox(r, "enabled"),
		BrokerHost:             r.FormValue("broker_host"),
		BrokerPort:             brokerPort,
		ClientID:               r.FormValue("client_id"),
		NodeID:                 r.FormValue("node_id"),
		Username:               r.FormValue("username"),
		Password:               r.FormValue("password"),
		TopicPrefix:            r.FormValue("topic_prefix"),
		PublishIntervalSeconds: publishInterval,
		QoS:                    int(qos),
		RetainStatus:           formCheckbox(r, "retain_status"),
		KeepAliveSeconds:       keepAlive,
		ClearPassword:          formCheckbox(r, "clear_password"),
		TLSEnabled:             formCheckbox(r, "tls_enabled"),
		TLSCAFile:              r.FormValue("tls_ca_file"),
		TLSClientCertFile:      r.FormValue("tls_client_cert_file"),
		TLSClientKeyFile:       r.FormValue("tls_client_key_file"),
		TLSInsecure:            formCheckbox(r, "tls_insecure"),
	}, ""
}

func parseUint16Form(r *http.Request, name string) (uint16, bool) {
	value, ok := parseUint32Form(r, name)
	if !ok || value == 0 || value > 65535 {
		return 0, false
	}
	return uint16(value), true
}

func parseUint32Form(r *http.Request, name string) (uint32, bool) {
	raw := strings.TrimSpace(r.FormValue(name))
	if raw == "" {
		return 0, false
	}
	value, err := strconv.ParseUint(raw, 10, 32)
	if err != nil {
		return 0, false
	}
	return uint32(value), true
}

func formCheckbox(r *http.Request, name string) bool {
	value := strings.ToLower(strings.TrimSpace(r.FormValue(name)))
	return value == "on" || value == "true" || value == "1" || value == "yes"
}

func (s *Server) handleFactoryResetAction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "恢复出厂数据请求格式不正确"})
		return
	}

	ctx, cancel := s.withConsoleTimeout(r.Context())
	defer cancel()

	if s.requestFactoryReset == nil {
		s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: "恢复出厂数据服务尚未初始化"})
		return
	}
	feedback := s.requestFactoryReset(ctx)
	if feedback.Success {
		s.applyFactoryResetToSessions()
		http.SetCookie(w, &http.Cookie{
			Name:     sessionCookieName,
			Value:    "",
			Path:     "/",
			Expires:  time.Unix(0, 0),
			MaxAge:   -1,
			HttpOnly: true,
			Secure:   shouldUseSecureCookie(r),
			SameSite: http.SameSiteLaxMode,
		})
		target := "/login?flash_type=success&flash_message=" + url.QueryEscape("已恢复出厂设置，请重新进入系统")
		http.Redirect(w, r, target, http.StatusSeeOther)
		return
	}
	s.redirectWithFeedback(w, r, feedback)
}
