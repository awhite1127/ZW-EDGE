package httpserver

// 本文件负责诊断 ZIP 的 HTTP 下载头和管理员校验；包内容及安全白名单由 service 层生成。

import (
	"context"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"time"

	"edge-web/internal/service"
)

// handleDiagnosticExport 处理诊断导出请求。
func (s *Server) handleDiagnosticExport(w http.ResponseWriter, r *http.Request) {
	if !s.requirePermission(w, r, permissionExportDiagnostics) {
		return
	}
	if s.console == nil {
		http.Error(w, "诊断包导出服务尚未初始化", http.StatusInternalServerError)
		return
	}

	now := time.Now()
	ctx, cancel := context.WithTimeout(r.Context(), exportRequestTimeout)
	defer cancel()

	filename := service.DiagnosticExportFilename(now)
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set(
		"Content-Disposition",
		fmt.Sprintf("attachment; filename=%q; filename*=UTF-8''%s", filename, url.PathEscape(filename)),
	)
	started, err := streamBufferedDownload(w, func(output io.Writer) error {
		return s.console.WriteDiagnosticExport(ctx, output, parseEventExportQuery(r), now)
	})
	if err != nil {
		log.Printf("诊断包导出失败: %v", err)
		if !started {
			w.Header().Del("Content-Disposition")
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			http.Error(w, "诊断包导出失败："+err.Error(), http.StatusInternalServerError)
		}
	}
}
