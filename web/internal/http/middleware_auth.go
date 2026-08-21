package httpserver

// 本文件实现登录、CSRF 和只读角色的统一前置授权；handler 内的管理员二次校验用于保护 GET 导出等特例。

import (
	"crypto/subtle"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const maxAuthenticatedRequestBodyBytes int64 = 512 << 10

// authMiddleware 校验会话身份并执行页面访问控制。
func (s *Server) authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if isPublicPath(r) {
			next.ServeHTTP(w, r)
			return
		}

		session, authenticated := s.sessionForRequest(r)
		if !authenticated {
			if r.Method == http.MethodGet && r.URL.Path == "/operations" {
				if cookie, err := r.Cookie(applicationUpdateWatchCookieName); err == nil && cookie.Value != "" {
					http.Redirect(w, r, "/upgrade-wait", http.StatusSeeOther)
					return
				}
			}
			logSecurityRejection(r, "session_not_found_or_expired")
			if wantsJSON(r) || strings.HasPrefix(r.URL.Path, "/api/") {
				writeError(w, http.StatusUnauthorized, "unauthorized", "登录状态不存在或已失效，请重新登录")
				return
			}

			target := "/login?redirect=" + url.QueryEscape(safeRedirectPath(r.URL.RequestURI()))
			http.Redirect(w, r, target, http.StatusSeeOther)
			return
		}

		if csrfRequired(r) {
			// 除两个有独立流式上限的大文件入口外，先给所有已认证写请求加统一硬上限。
			// CSRF 校验会提前 ParseForm；若在这里不限制，处理器稍后设置的上限已经来不及，
			// 异常表单可在鉴权阶段占用大量内存或 multipart 临时文件。
			if authenticatedRequestBodyLimit(r) > 0 {
				r.Body = http.MaxBytesReader(w, r.Body, maxAuthenticatedRequestBodyBytes)
			}
			csrfResult := validateCSRFToken(r, session.CSRFToken)
			if !csrfResult.valid {
				logSecurityRejection(r, csrfResult.reason)
				if wantsJSON(r) || strings.HasPrefix(r.URL.Path, "/api/") {
					writeError(w, http.StatusForbidden, csrfResult.code, csrfResult.message)
					return
				}
				http.Error(w, csrfResult.message, http.StatusForbidden)
				return
			}
		}

		if metadata, ok := pageRouteForPath(r.URL.Path); r.Method == http.MethodGet && ok && !pageRouteAllowedForRole(metadata, session.Role) {
			const message = "当前账户无权访问该页面"
			logSecurityRejection(r, "permission_denied")
			if wantsJSON(r) || strings.HasPrefix(r.URL.Path, "/api/") {
				writeError(w, http.StatusForbidden, "forbidden", message)
				return
			}
			http.Error(w, message, http.StatusForbidden)
			return
		}
		if permissions := requestAnyPermissions(r); len(permissions) > 0 && !hasAnyPermission(session.Role, permissions...) {
			const message = "当前账户无权执行此操作"
			logSecurityRejection(r, "permission_denied")
			if wantsJSON(r) || strings.HasPrefix(r.URL.Path, "/api/") {
				writeError(w, http.StatusForbidden, "forbidden", message)
				return
			}
			http.Error(w, message, http.StatusForbidden)
			return
		}
		if permission := requestPermission(r); permission != "" && !hasPermission(session.Role, permission) {
			const message = "当前账户无权执行此操作"
			logSecurityRejection(r, "permission_denied")
			if wantsJSON(r) || strings.HasPrefix(r.URL.Path, "/api/") {
				writeError(w, http.StatusForbidden, "forbidden", message)
				return
			}
			http.Error(w, message, http.StatusForbidden)
			return
		}

		next.ServeHTTP(w, withRequestSession(r, session))
	})
}

// authenticatedRequestBodyLimit 判断写请求是否使用通用请求体上限。
// 配置导入和应用升级在各自处理器中按产品允许的文件大小流式限流。
func authenticatedRequestBodyLimit(r *http.Request) int64 {
	if r == nil {
		return 0
	}
	switch r.URL.Path {
	case "/settings/config/import", "/api/application-update/upload":
		// 只有处理器实际支持的 multipart 请求才交由其更大的流式上限处理；
		// 伪装成普通表单的同路径请求仍使用通用上限，避免在 CSRF ParseForm 时绕过。
		if strings.HasPrefix(strings.ToLower(strings.TrimSpace(r.Header.Get("Content-Type"))), "multipart/form-data") {
			return 0
		}
	}
	return maxAuthenticatedRequestBodyBytes
}

// csrfRequired 判断当前请求是否必须校验 CSRF 令牌。
func csrfRequired(r *http.Request) bool {
	switch r.Method {
	case http.MethodGet, http.MethodHead, http.MethodOptions:
		return false
	default:
		return !isPublicPath(r)
	}
}

type csrfValidationResult struct {
	valid   bool
	code    string
	reason  string
	message string
}

// validateCSRFToken 校验请求携带的 CSRF 令牌。
func validateCSRFToken(r *http.Request, expected string) csrfValidationResult {
	if expected == "" {
		return csrfValidationResult{
			code:    "csrf_session_unavailable",
			reason:  "csrf_session_token_unavailable",
			message: "当前会话的安全令牌不可用，请重新登录",
		}
	}
	candidate := strings.TrimSpace(r.Header.Get("X-CSRF-Token"))
	if candidate == "" {
		// multipart 上传必须通过请求头携带令牌。这里不提前解析上传体，
		// 避免绕过处理器的体积限制，也避免 ParseForm 无法读取 multipart 字段的歧义。
		if !strings.HasPrefix(strings.ToLower(r.Header.Get("Content-Type")), "multipart/form-data") {
			if err := r.ParseForm(); err != nil {
				return csrfValidationResult{
					code:    "csrf_request_invalid",
					reason:  "csrf_form_parse_failed",
					message: "请求格式无法完成安全校验，请刷新页面后重试",
				}
			}
			candidate = strings.TrimSpace(r.FormValue("csrf_token"))
		}
	}
	if candidate == "" {
		return csrfValidationResult{
			code:    "csrf_missing",
			reason:  "csrf_token_missing",
			message: "请求缺少安全令牌，请刷新页面后重试",
		}
	}
	if subtle.ConstantTimeCompare([]byte(candidate), []byte(expected)) != 1 {
		return csrfValidationResult{
			code:    "csrf_mismatch",
			reason:  "csrf_token_mismatch",
			message: "安全令牌已失效，请刷新页面后重试",
		}
	}
	return csrfValidationResult{valid: true}
}

// csrfTokenValid 以常量时间比较 CSRF 令牌。
func csrfTokenValid(r *http.Request, expected string) bool {
	return validateCSRFToken(r, expected).valid
}

// logSecurityRejection 记录安全拒绝。
func logSecurityRejection(r *http.Request, reason string) {
	log.Printf("Web 安全校验拒绝请求: reason=%s method=%s path=%s", reason, r.Method, r.URL.Path)
}

// isPublicPath 判断是否为公开路径。
func isPublicPath(r *http.Request) bool {
	if strings.HasPrefix(r.URL.Path, "/static/") {
		return true
	}
	switch {
	case r.Method == http.MethodGet && r.URL.Path == "/upgrade-wait":
		return true
	case r.Method == http.MethodGet && r.URL.Path == "/api/application-update/watch":
		return true
	case r.Method == http.MethodGet && r.URL.Path == "/login":
		return true
	case r.Method == http.MethodPost && r.URL.Path == "/login":
		return true
	case r.Method == http.MethodPost && r.URL.Path == "/login/initial-setup":
		return true
	default:
		return false
	}
}

type responseStatusWriter struct {
	http.ResponseWriter
	status int
}

func (w *responseStatusWriter) WriteHeader(status int) {
	if w.status != 0 {
		return
	}
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

func (w *responseStatusWriter) Write(payload []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	return w.ResponseWriter.Write(payload)
}

// Unwrap 允许 http.ResponseController 继续访问底层 ResponseWriter 的可选能力。
func (w *responseStatusWriter) Unwrap() http.ResponseWriter {
	return w.ResponseWriter
}

// loggingMiddleware 仅记录失败、慢请求和产生状态变更的请求，
// 避免静态资源与高频状态轮询在正常运行时持续刷写日志。
func loggingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		started := time.Now()
		statusWriter := &responseStatusWriter{ResponseWriter: w}
		next.ServeHTTP(statusWriter, r)

		status := statusWriter.status
		if status == 0 {
			status = http.StatusOK
		}
		elapsed := time.Since(started)
		switch {
		case status >= http.StatusBadRequest:
			log.Printf(
				"HTTP 请求失败: method=%s path=%s status=%d elapsed=%s",
				r.Method,
				r.URL.Path,
				status,
				elapsed.Round(time.Millisecond),
			)
		case elapsed >= time.Second:
			log.Printf(
				"HTTP 请求较慢: method=%s path=%s status=%d elapsed=%s",
				r.Method,
				r.URL.Path,
				status,
				elapsed.Round(time.Millisecond),
			)
		case r.Method != http.MethodGet && r.Method != http.MethodHead && r.Method != http.MethodOptions:
			log.Printf(
				"HTTP 请求完成: method=%s path=%s status=%d elapsed=%s",
				r.Method,
				r.URL.Path,
				status,
				elapsed.Round(time.Millisecond),
			)
		}
	})
}
