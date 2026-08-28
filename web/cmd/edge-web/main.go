package main

// edge-web 仅提供页面、鉴权和 IPC 适配，不直接访问串口或业务数据库。

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	httpserver "edge-web/internal/http"
	"edge-web/internal/ipc"
	"edge-web/internal/service"
)

// version 由正式 release 使用根 VERSION 通过 -ldflags 注入；本地开发构建保持 dev。
var version = "dev"

func main() {
	if err := run(); err != nil {
		// run 已完成 signal/server 等资源的 defer 清理，最后才设置非零退出码。
		log.Printf("edge-web 退出：%v", err)
		os.Exit(1)
	}
}

type lifecycleServer interface {
	ListenAndServe() error
	Shutdown(context.Context) error
	Close() error
}

func run() error {
	listenAddr := envOrDefault("EDGE_WEB_LISTEN", ":8080")
	socketPath := envOrDefault("EDGE_CONTROLLER_IPC_SOCK", "/tmp/edge-controller.sock")
	templateDir := templateDirFromEnv()
	staticDir := envOrDefault("EDGE_WEB_STATIC_DIR", "static")
	ipcTimeout := ipcTimeoutFromEnv()
	log.Printf("Edge Web 软件版本：%s", version)

	// Web 进程不直接访问串口或数据库，所有运行态数据都通过后端 IPC 获取。
	ipcClient := ipc.NewClient(socketPath, ipcTimeout)
	backendService := service.NewBackendServiceWithTimeout(ipcClient, ipcTimeout)

	server, err := httpserver.NewServer(listenAddr, templateDir, staticDir, backendService)
	if err != nil {
		return fmt.Errorf("初始化 Web 服务失败：%w", err)
	}

	log.Printf("Web 服务启动，监听地址：%s", listenAddr)
	log.Printf("后端 IPC 套接字：%s", socketPath)
	log.Printf("后端 IPC 超时时间：%s", ipcTimeout)
	log.Printf("当前路由依赖 Go 1.22+ 的 ServeMux 路由写法")

	// 捕获退出信号后走 http.Server.Shutdown，保证正在处理的页面/API 请求有机会完成。
	signalCtx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return serveUntilDone(signalCtx, server, 5*time.Second)
}

// serveUntilDone 统一收敛监听错误与退出信号；调用方返回后再决定进程退出码，避免绕过 defer。
func serveUntilDone(signalCtx context.Context, server lifecycleServer, shutdownTimeout time.Duration) error {
	serverErrCh := make(chan error, 1)
	go func() {
		serverErrCh <- server.ListenAndServe()
	}()
	defer func() {
		if err := server.Close(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("关闭 Web 服务资源失败：%v", err)
		}
	}()

	select {
	case err := <-serverErrCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("Web 服务运行失败：%w", err)
		}
		return nil
	case <-signalCtx.Done():
		log.Printf("收到退出信号，开始停止 Web 服务")

		shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
		defer cancel()

		if err := server.Shutdown(shutdownCtx); err != nil {
			log.Printf("优雅关闭失败：%v", err)
			if closeErr := server.Close(); closeErr != nil {
				log.Printf("强制关闭失败：%v", closeErr)
			}
			return fmt.Errorf("优雅关闭 Web 服务失败：%w", err)
		}

		if err := <-serverErrCh; err != nil && !errors.Is(err, http.ErrServerClosed) {
			return fmt.Errorf("Web 服务退出时发生错误：%w", err)
		}
		return nil
	}
}

// templateDirFromEnv 在未配置目录时使用内嵌模板；任何显式目录都作为磁盘覆盖路径。
func templateDirFromEnv() string {
	value := strings.TrimSpace(os.Getenv("EDGE_WEB_TEMPLATE_DIR"))
	return value
}

func envOrDefault(key string, fallback string) string {
	value := os.Getenv(key)
	if value == "" {
		return fallback
	}
	return value
}

func ipcTimeoutFromEnv() time.Duration {
	const defaultTimeoutSeconds = 15
	rawValue := os.Getenv("EDGE_WEB_IPC_TIMEOUT_SECONDS")
	if rawValue == "" {
		return defaultTimeoutSeconds * time.Second
	}

	seconds, err := strconv.Atoi(rawValue)
	if err != nil || seconds <= 0 {
		log.Printf("EDGE_WEB_IPC_TIMEOUT_SECONDS 无效，使用默认 %d 秒", defaultTimeoutSeconds)
		return defaultTimeoutSeconds * time.Second
	}
	return time.Duration(seconds) * time.Second
}
