package httpserver

// 本文件创建 HTTP 服务并装配模板、会话、控制台服务和超时策略。

import (
	"context"
	"html/template"
	"net/http"
	"sync"
	"time"

	"edge-web/internal/model"
	"edge-web/internal/service"
)

// sessions、sessionMu 和清理信号共同构成会话子系统；网络/IPC 调用不得在持有 sessionMu 时执行。
type Server struct {
	httpServer                   *http.Server
	templates                    map[string]*template.Template
	console                      *service.ConsoleService
	loginAttempts                *loginAttemptStore
	verifyWebLogin               func(context.Context, model.WebLoginRequest) (model.WebLoginResult, error)
	changeWebPassword            func(context.Context, model.WebPasswordChangeRequest) model.ActionFeedback
	setViewerPassword            func(context.Context, model.WebViewerPasswordSetRequest) model.ActionFeedback
	requestFactoryReset          func(context.Context) model.ActionFeedback
	getFirstBootAdminEntryStatus func(context.Context) (model.FirstBootAdminEntryStatus, error)
	consumeFirstBootAdminEntry   func(context.Context) (model.FirstBootAdminEntryResult, error)
	getUpdateCurrentVersion      func(context.Context) (model.UpdateVersion, error)
	getUpdateStatus              func(context.Context) (model.UpdateStatus, error)
	importApplicationUpdate      func(context.Context, string, string) (model.UpdateStatus, error)
	startApplicationUpdate       func(context.Context, string) (model.UpdateStatus, error)
	applicationUpdateUploadDir   string
	applicationUpdateMu          sync.Mutex
	sessions                     map[string]sessionState
	sessionMu                    sync.Mutex
	sessionCleanupStop           chan struct{}
	sessionCleanupDone           chan struct{}
	sessionCleanupOnce           sync.Once
}

const (
	sessionCookieName          = "edge_web_session"
	sessionDuration            = 12 * time.Hour
	browserSessionIdleDuration = 24 * time.Hour
	maxServerSessions          = 256
	maxWebPasswordBytes        = 256
	readHeaderTimeout          = 5 * time.Second
	readTimeout                = 15 * time.Second
	// ntpd -gq 的后端超时为 45 秒，HTTP 写超时需覆盖完整 IPC 往返。
	writeTimeout = 95 * time.Second
	idleTimeout  = 60 * time.Second
	// 后端不可达时的页面标题降级默认值，需要与 C++ backend/src/data/model/system_settings.h 保持同步。
	defaultSystemDisplayName = "珠海知更通讯管理系统"
)

// NewServer 创建并初始化服务。
func NewServer(addr string, templateDir string, staticDir string, backend *service.BackendService) (*Server, error) {
	consoleService := service.NewConsoleService(backend)
	templates, err := loadPageTemplates(templateDir)
	if err != nil {
		return nil, err
	}

	server := &Server{
		templates:                    templates,
		console:                      consoleService,
		loginAttempts:                newLoginAttemptStore(),
		verifyWebLogin:               consoleService.VerifyWebLogin,
		changeWebPassword:            consoleService.ChangeWebPassword,
		setViewerPassword:            consoleService.SetWebViewerPassword,
		requestFactoryReset:          consoleService.RequestFactoryReset,
		getFirstBootAdminEntryStatus: backend.GetFirstBootAdminEntryStatus,
		consumeFirstBootAdminEntry:   backend.ConsumeFirstBootAdminEntry,
		getUpdateCurrentVersion:      consoleService.GetUpdateCurrentVersion,
		getUpdateStatus:              consoleService.GetUpdateStatus,
		importApplicationUpdate:      consoleService.ImportApplicationUpgradePackage,
		startApplicationUpdate:       consoleService.StartUpdateJob,
		applicationUpdateUploadDir:   defaultApplicationUpdateUploadDir,
		sessions:                     make(map[string]sessionState),
		sessionCleanupStop:           make(chan struct{}),
		sessionCleanupDone:           make(chan struct{}),
	}
	server.startSessionCleanup()

	mux := http.NewServeMux()
	server.registerRoutes(mux, staticDir)

	server.httpServer = &http.Server{
		Addr:              addr,
		Handler:           loggingMiddleware(server.authMiddleware(mux)),
		ReadHeaderTimeout: readHeaderTimeout,
		ReadTimeout:       readTimeout,
		WriteTimeout:      writeTimeout,
		IdleTimeout:       idleTimeout,
	}
	return server, nil
}

// withConsoleTimeout 为单次控制台调用创建超时上下文。
func (s *Server) withConsoleTimeout(parent context.Context) (context.Context, context.CancelFunc) {
	if s.console != nil {
		return s.console.WithTimeout(parent)
	}
	return context.WithTimeout(parent, 15*time.Second)
}

// verifyLogin 验证登录。
func (s *Server) verifyLogin(
	ctx context.Context,
	request model.WebLoginRequest,
) (model.WebLoginResult, error) {
	if s.verifyWebLogin != nil {
		return s.verifyWebLogin(ctx, request)
	}
	return s.console.VerifyWebLogin(ctx, request)
}

// changeAccountPassword 修改账户密码。
func (s *Server) changeAccountPassword(
	ctx context.Context,
	request model.WebPasswordChangeRequest,
) model.ActionFeedback {
	if s.changeWebPassword != nil {
		return s.changeWebPassword(ctx, request)
	}
	return s.console.ChangeWebPassword(ctx, request)
}

// setUserPassword 设置用户密码。
func (s *Server) setUserPassword(
	ctx context.Context,
	request model.WebViewerPasswordSetRequest,
) model.ActionFeedback {
	if s.setViewerPassword != nil {
		return s.setViewerPassword(ctx, request)
	}
	return s.console.SetWebViewerPassword(ctx, request)
}

// ListenAndServe 启动 HTTP 服务并监听请求。
func (s *Server) ListenAndServe() error {
	return s.httpServer.ListenAndServe()
}

// Close 关闭 HTTP 服务。
func (s *Server) Close() error {
	s.stopSessionCleanup()
	return s.httpServer.Close()
}

// Shutdown 优雅关闭。
func (s *Server) Shutdown(ctx context.Context) error {
	s.stopSessionCleanup()
	return s.httpServer.Shutdown(ctx)
}
