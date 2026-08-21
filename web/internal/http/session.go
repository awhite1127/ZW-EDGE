package httpserver

// 本文件管理内存会话、Cookie 生命周期和页面基础身份信息；服务重启后会话主动失效。

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"

	"edge-web/internal/model"
)

const defaultAuthenticatedPath = "/realtime"

type requestSessionContextKey struct{}

const (
	roleSuperAdmin  = "super_admin"
	roleLegacyAdmin = "admin"
	roleEngineer    = "engineer"
	roleOperator    = "operator"
	roleViewer      = "viewer"
	roleAdmin       = roleSuperAdmin
)

type sessionState struct {
	// 会话只保存认证与 CSRF 元数据，不缓存后端业务权限或设备运行状态。
	Username                  string
	Role                      string
	CSRFToken                 string
	ExpiresAt                 time.Time
	LastSeenAt                time.Time
	PasswordChangeRecommended bool
}

// sessionExpiration 根据角色计算会话过期时间。
func sessionExpiration(role string, now time.Time) time.Time {
	normalizedRole := normalizeRole(role)
	if normalizedRole == roleOperator || normalizedRole == roleViewer {
		// 操作员和值守只读账号使用浏览器会话 Cookie，服务端另按空闲时间回收。
		return time.Time{}
	}
	return now.Add(sessionDuration)
}

// sessionExpired 判断会话是否已经过期。
func sessionExpired(state sessionState, now time.Time) bool {
	if state.ExpiresAt.IsZero() {
		role := normalizeRole(state.Role)
		if role != roleOperator && role != roleViewer {
			return true
		}
		// 兼容测试夹具或升级前的进程内状态；生产登录入口始终写入 LastSeenAt。
		if state.LastSeenAt.IsZero() {
			return false
		}
		return !now.Before(state.LastSeenAt.Add(browserSessionIdleDuration))
	}
	return !now.Before(state.ExpiresAt)
}

// validLoginIdentity 校验登录响应中的用户名和角色。
func validLoginIdentity(username string, role string) bool {
	role = normalizeRole(role)
	switch role {
	case roleSuperAdmin, roleEngineer, roleOperator, roleViewer:
		return strings.TrimSpace(username) != ""
	default:
		return false
	}
}

// roleText 返回用户角色的中文名称。
func roleText(role string) string {
	switch normalizeRole(role) {
	case roleSuperAdmin:
		return "超级管理员"
	case roleEngineer:
		return "工程师"
	case roleOperator:
		return "操作员"
	case roleViewer:
		return "只读用户"
	default:
		return "未知角色"
	}
}

// normalizeRole 规范化角色。
func normalizeRole(role string) string {
	role = strings.TrimSpace(role)
	if role == roleLegacyAdmin {
		return roleSuperAdmin
	}
	return role
}

// startSessionCleanup 启动会话过期清理协程。
func (s *Server) startSessionCleanup() {
	go func() {
		defer close(s.sessionCleanupDone)
		ticker := time.NewTicker(10 * time.Minute)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				s.cleanupExpiredSessions(time.Now())
			case <-s.sessionCleanupStop:
				return
			}
		}
	}()
}

// stopSessionCleanup 停止会话过期清理协程。
func (s *Server) stopSessionCleanup() {
	s.sessionCleanupOnce.Do(func() {
		close(s.sessionCleanupStop)
		<-s.sessionCleanupDone
	})
}

// cleanupExpiredSessions 清理已过期会话。
func (s *Server) cleanupExpiredSessions(now time.Time) {
	s.sessionMu.Lock()
	defer s.sessionMu.Unlock()

	for token, state := range s.sessions {
		if sessionExpired(state, now) {
			delete(s.sessions, token)
		}
	}
}

// pruneSessionsForInsertLocked 在持锁状态下清理过期会话，并为新会话预留固定容量。
func (s *Server) pruneSessionsForInsertLocked(now time.Time) {
	for token, state := range s.sessions {
		if sessionExpired(state, now) {
			delete(s.sessions, token)
		}
	}
	for len(s.sessions) >= maxServerSessions {
		oldestToken := ""
		var oldestTime time.Time
		for token, state := range s.sessions {
			activityTime := state.LastSeenAt
			if activityTime.IsZero() {
				activityTime = state.ExpiresAt
			}
			if oldestToken == "" || activityTime.Before(oldestTime) {
				oldestToken = token
				oldestTime = activityTime
			}
		}
		if oldestToken == "" {
			break
		}
		delete(s.sessions, oldestToken)
	}
}

// isAuthenticated 判断请求是否具有有效登录会话。
func (s *Server) isAuthenticated(r *http.Request) bool {
	_, ok := s.sessionForRequest(r)
	return ok
}

// sessionForRequest 读取当前请求对应的会话。
func (s *Server) sessionForRequest(r *http.Request) (sessionState, bool) {
	if r != nil {
		if state, ok := r.Context().Value(requestSessionContextKey{}).(sessionState); ok {
			return state, true
		}
	}
	// 会话只保存在 Web 进程内存中；每次读取顺便淘汰过期或身份异常的 token。
	cookie, err := r.Cookie(sessionCookieName)
	if err != nil || cookie.Value == "" {
		return sessionState{}, false
	}

	now := time.Now()
	s.sessionMu.Lock()
	defer s.sessionMu.Unlock()

	state, ok := s.sessions[cookie.Value]
	if !ok {
		return sessionState{}, false
	}
	if sessionExpired(state, now) {
		delete(s.sessions, cookie.Value)
		return sessionState{}, false
	}
	if !validLoginIdentity(state.Username, state.Role) {
		delete(s.sessions, cookie.Value)
		return sessionState{}, false
	}
	state.LastSeenAt = now
	s.sessions[cookie.Value] = state
	return state, true
}

// withRequestSession 把鉴权阶段已验证的会话绑定到当前请求，
// 后续权限判断和页面数据构造无需再次解析 Cookie、争用会话锁。
func withRequestSession(r *http.Request, state sessionState) *http.Request {
	if r == nil {
		return nil
	}
	return r.WithContext(context.WithValue(r.Context(), requestSessionContextKey{}, state))
}

// completeOwnPasswordChange 完成当前用户改密并撤销其他会话。
func (s *Server) completeOwnPasswordChange(r *http.Request, username string) {
	cookie, err := r.Cookie(sessionCookieName)
	if err != nil || cookie.Value == "" {
		return
	}
	// 当前用户改密后保留本次会话，其他同账号会话全部失效，避免旧密码登录态继续使用。
	s.invalidateSessionsForUsername(username, cookie.Value)

	s.sessionMu.Lock()
	defer s.sessionMu.Unlock()
	state, ok := s.sessions[cookie.Value]
	if ok && state.Username == username {
		state.PasswordChangeRecommended = false
		s.sessions[cookie.Value] = state
	}
}

// csrfTokenForRequest 读取当前请求会话的 CSRF 令牌。
func (s *Server) csrfTokenForRequest(r *http.Request) string {
	cookie, err := r.Cookie(sessionCookieName)
	if err != nil || cookie.Value == "" {
		return ""
	}

	s.sessionMu.Lock()
	defer s.sessionMu.Unlock()

	state, ok := s.sessions[cookie.Value]
	if !ok {
		return ""
	}
	if state.CSRFToken == "" {
		token, err := newSessionToken()
		if err != nil {
			return ""
		}
		state.CSRFToken = token
		s.sessions[cookie.Value] = state
	}
	return state.CSRFToken
}

// invalidateSessionsForUsername 撤销指定用户的全部会话。
func (s *Server) invalidateSessionsForUsername(username string, keepToken string) {
	s.sessionMu.Lock()
	defer s.sessionMu.Unlock()

	now := time.Now()
	for token, state := range s.sessions {
		if sessionExpired(state, now) {
			delete(s.sessions, token)
			continue
		}
		if state.Username != username || token == keepToken {
			continue
		}
		delete(s.sessions, token)
	}
}

// applyFactoryResetToSessions 恢复出厂设置后撤销现有会话。
func (s *Server) applyFactoryResetToSessions() {
	s.sessionMu.Lock()
	defer s.sessionMu.Unlock()

	// 恢复出厂会重置账户密码和首次部署状态，所有旧会话必须立即失效。
	for token := range s.sessions {
		delete(s.sessions, token)
	}
}

// redirectLoginWithError 重定向登录带错误。
func (s *Server) redirectLoginWithError(w http.ResponseWriter, r *http.Request, message string) {
	target := "/login?redirect=" + url.QueryEscape(safeRedirectPath(r.FormValue("redirect")))
	target += "&flash_type=error&flash_message=" + url.QueryEscape(message)
	http.Redirect(w, r, target, http.StatusSeeOther)
}

// newSessionToken 生成安全的随机会话令牌。
func newSessionToken() (string, error) {
	bytes := make([]byte, 32)
	if _, err := rand.Read(bytes); err != nil {
		return "", err
	}
	return hex.EncodeToString(bytes), nil
}

// readFlash 读取并清除一次性页面提示。
func readFlash(r *http.Request) *model.FlashMessage {
	message := r.URL.Query().Get("flash_message")
	if message == "" {
		return nil
	}

	flashType := r.URL.Query().Get("flash_type")
	switch flashType {
	case "success", "error", "info", "warning":
	default:
		flashType = "info"
	}

	return &model.FlashMessage{
		Type: flashType,
		Text: userVisibleErrorMessage(message),
	}
}

// safeRedirectPath 校验并返回安全的站内跳转路径。
func safeRedirectPath(raw string) string {
	// 只允许站内已知页面作为跳转目标，避免登录 redirect 被用作开放重定向。
	if raw == "" {
		return defaultAuthenticatedPath
	}

	u, err := url.Parse(raw)
	if err != nil {
		return defaultAuthenticatedPath
	}

	if u.Scheme != "" || u.Host != "" {
		return defaultAuthenticatedPath
	}

	if u.Path == "" || u.Path[0] != '/' || strings.HasPrefix(u.Path, "//") || strings.Contains(u.Path, "\\") {
		return defaultAuthenticatedPath
	}

	// 根地址用于板端启动；仅透传虚拟键盘的显式开关，其他参数仍按既有白名单处理。
	if u.Path == "/" {
		if keyboard := u.Query().Get("keyboard"); keyboard == "0" || keyboard == "1" {
			return defaultAuthenticatedPath + "?keyboard=" + keyboard
		}
		return defaultAuthenticatedPath
	}

	metadata, ok := pageRouteForPath(u.Path)
	if !ok || !metadata.AllowLoginRedirect {
		return defaultAuthenticatedPath
	}
	if u.RawQuery != "" {
		return u.Path + "?" + u.RawQuery
	}
	return u.Path
}

// safeRedirectPathForRole 校验并返回角色可访问的重定向路径。
func safeRedirectPathForRole(raw string, role string) string {
	target := safeRedirectPath(raw)
	u, err := url.Parse(target)
	if err != nil {
		return defaultAuthenticatedPath
	}
	metadata, ok := pageRouteForPath(u.Path)
	if !ok {
		return defaultAuthenticatedPath
	}
	if !pageRouteAllowedForRole(metadata, role) {
		fallback := metadata.DefaultFallback
		fallbackMetadata, fallbackOK := pageRouteForPath(fallback)
		if fallbackOK && pageRouteAllowedForRole(fallbackMetadata, role) {
			return fallback
		}
		return defaultAuthenticatedPath
	}
	return target
}

// isSafeCommunicationTracePath 判断是否为安全通讯报文路径。
func isSafeCommunicationTracePath(path string) bool {
	const prefix = "/collection/channels/"
	const suffix = "/communication-traces"
	if !strings.HasPrefix(path, prefix) || !strings.HasSuffix(path, suffix) {
		return false
	}
	channelID := strings.TrimSuffix(strings.TrimPrefix(path, prefix), suffix)
	return channelID != "" && !strings.Contains(channelID, "/")
}

// shouldUseSecureCookie 判断当前请求是否应使用安全 Cookie。
func shouldUseSecureCookie(r *http.Request) bool {
	if strings.TrimSpace(os.Getenv("EDGE_WEB_COOKIE_SECURE")) == "1" {
		return true
	}
	if r != nil && r.TLS != nil {
		return true
	}
	return r != nil && strings.EqualFold(strings.TrimSpace(r.Header.Get("X-Forwarded-Proto")), "https")
}
