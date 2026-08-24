package httpserver

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestAPIGetRoutesHaveExplicitAuthorization(t *testing.T) {
	seen := make(map[string]bool, len(apiGETRoutes))
	for _, spec := range apiGETRoutes {
		if seen[spec.pattern] {
			t.Fatalf("duplicate API GET route %q", spec.pattern)
		}
		seen[spec.pattern] = true
		if spec.handler == nil {
			t.Errorf("%s has no handler", spec.pattern)
		}
		if spec.public && len(spec.anyPermissions) > 0 {
			t.Errorf("public route %s must not require authenticated permissions", spec.pattern)
		}
		if !spec.public && len(spec.anyPermissions) == 0 {
			t.Errorf("protected route %s has no explicit permissions", spec.pattern)
		}
		for _, permission := range spec.anyPermissions {
			if !hasPermission(roleSuperAdmin, permission) {
				t.Errorf("%s references unknown permission %q", spec.pattern, permission)
			}
		}
	}
}

func TestAPIGetPermissionPolicy(t *testing.T) {
	tests := []struct {
		name string
		role string
		path string
		want int
	}{
		{name: "viewer overview", role: roleViewer, path: "/api/system/overview-snapshot", want: http.StatusNoContent},
		{name: "viewer realtime", role: roleViewer, path: "/api/view/realtime", want: http.StatusNoContent},
		{name: "viewer data maintenance", role: roleViewer, path: "/api/data-maintenance", want: http.StatusForbidden},
		{name: "viewer devices", role: roleViewer, path: "/api/devices", want: http.StatusForbidden},
		{name: "viewer device detail", role: roleViewer, path: "/api/devices/device-1/detail", want: http.StatusForbidden},
		{name: "viewer config summary", role: roleViewer, path: "/api/config/summary", want: http.StatusForbidden},
		{name: "engineer device detail", role: roleEngineer, path: "/api/devices/device-1/detail", want: http.StatusNoContent},
		{name: "admin data maintenance", role: roleSuperAdmin, path: "/api/data-maintenance", want: http.StatusNoContent},
		{name: "undeclared api fails closed", role: roleSuperAdmin, path: "/api/not-declared", want: http.StatusForbidden},
		{name: "public update watch", role: "", path: "/api/application-update/watch", want: http.StatusNoContent},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := &Server{sessions: make(map[string]sessionState)}
			request := httptest.NewRequest(http.MethodGet, tt.path, nil)
			if tt.role != "" {
				const token = "test-session"
				server.sessions[token] = sessionState{
					Username:   "tester",
					Role:       tt.role,
					ExpiresAt:  time.Now().Add(time.Hour),
					LastSeenAt: time.Now(),
				}
				request.AddCookie(&http.Cookie{Name: sessionCookieName, Value: token})
			}

			response := httptest.NewRecorder()
			next := http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(http.StatusNoContent)
			})
			server.authMiddleware(next).ServeHTTP(response, request)
			if response.Code != tt.want {
				t.Fatalf("status = %d, want %d; body=%s", response.Code, tt.want, response.Body.String())
			}
		})
	}
}
