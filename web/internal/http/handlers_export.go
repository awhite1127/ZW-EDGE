package httpserver

// 本文件处理 CSV 下载，并对 GET 导出执行管理员二次鉴权，避免只读角色绕过页面按钮限制。

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"

	"edge-web/internal/model"
	"edge-web/internal/service"
)

const (
	exportRequestTimeout      = 5 * time.Minute
	exportResponseBufferBytes = 64 << 10
)

type downloadResponseWriter struct {
	http.ResponseWriter
	written int64
}

func (w *downloadResponseWriter) Write(payload []byte) (int, error) {
	n, err := w.ResponseWriter.Write(payload)
	w.written += int64(n)
	return n, err
}

// handleHistoryCSVExport 处理历史数据CSV导出请求。
func (s *Server) handleHistoryCSVExport(w http.ResponseWriter, r *http.Request) {
	if !s.requireAdminExport(w, r) {
		return
	}
	if s.console == nil {
		http.Error(w, "导出服务尚未初始化", http.StatusInternalServerError)
		return
	}

	query := parseHistoryExportQuery(r)
	filename := historyExportFilename(query, time.Now())

	ctx, cancel := context.WithTimeout(r.Context(), exportRequestTimeout)
	defer cancel()
	setCSVDownloadHeaders(w, filename)
	started, err := streamBufferedDownload(w, func(output io.Writer) error {
		return s.console.WriteHistoryExportCSV(ctx, output, query)
	})
	if err != nil {
		handleStreamingExportError(w, "历史数据 CSV 导出失败", err, started)
		return
	}
}

// handleEventsCSVExport 处理事件CSV导出请求。
func (s *Server) handleEventsCSVExport(w http.ResponseWriter, r *http.Request) {
	if !s.requireAdminExport(w, r) {
		return
	}
	if s.console == nil {
		http.Error(w, "导出服务尚未初始化", http.StatusInternalServerError)
		return
	}

	query := parseEventExportQuery(r)
	filename := eventsExportFilename(query, time.Now())

	ctx, cancel := context.WithTimeout(r.Context(), exportRequestTimeout)
	defer cancel()
	setCSVDownloadHeaders(w, filename)
	started, err := streamBufferedDownload(w, func(output io.Writer) error {
		return s.console.WriteServiceEventsExportCSV(ctx, output, query)
	})
	if err != nil {
		handleStreamingExportError(w, "历史事件 CSV 导出失败", err, started)
		return
	}
}

// streamBufferedDownload 只保留固定大小的网络缓冲，避免完整导出文件常驻 Go 堆。
// 返回 started=true 表示响应已经提交，此时只能记录中断，不能再追加 HTTP 错误页。
func streamBufferedDownload(
	w http.ResponseWriter,
	write func(io.Writer) error,
) (started bool, err error) {
	_ = http.NewResponseController(w).SetWriteDeadline(time.Now().Add(exportRequestTimeout))
	tracked := &downloadResponseWriter{ResponseWriter: w}
	buffered := bufio.NewWriterSize(tracked, exportResponseBufferBytes)
	writeErr := write(buffered)
	if writeErr != nil {
		if tracked.written == 0 {
			// 尚未越过固定缓冲区时丢弃不完整内容，调用方仍可返回规范错误响应。
			return false, writeErr
		}
		flushErr := buffered.Flush()
		return true, errors.Join(writeErr, flushErr)
	}
	if flushErr := buffered.Flush(); flushErr != nil {
		return tracked.written > 0, flushErr
	}
	return tracked.written > 0, nil
}

// requireAdminExport 校验管理员导出。
func (s *Server) requireAdminExport(w http.ResponseWriter, r *http.Request) bool {
	session, authenticated := s.sessionForRequest(r)
	if !authenticated {
		http.Error(w, "请先登录后再导出数据", http.StatusUnauthorized)
		return false
	}
	if normalizeRole(session.Role) != roleSuperAdmin {
		http.Error(w, "当前账户无权导出数据", http.StatusForbidden)
		return false
	}
	return true
}

// parseHistoryExportQuery 解析历史数据导出查询条件。
func parseHistoryExportQuery(r *http.Request) model.HistoryExportQuery {
	values := r.URL.Query()
	period := strings.TrimSpace(values.Get("sample_period"))
	if period == "" {
		period = strings.TrimSpace(values.Get("period"))
	}
	return model.HistoryExportQuery{
		ChannelID:    strings.TrimSpace(values.Get("channel_id")),
		MasterID:     strings.TrimSpace(values.Get("master_id")),
		DeviceID:     strings.TrimSpace(values.Get("device_id")),
		PointKey:     strings.TrimSpace(values.Get("point_key")),
		SamplePeriod: period,
	}
}

// parseEventExportQuery 解析事件导出查询条件。
func parseEventExportQuery(r *http.Request) model.EventExportQuery {
	values := r.URL.Query()
	return model.EventExportQuery{
		Level:     strings.TrimSpace(values.Get("level")),
		Source:    strings.TrimSpace(values.Get("source")),
		TimeRange: strings.TrimSpace(values.Get("range")),
		Search:    strings.TrimSpace(values.Get("q")),
	}
}

// setCSVDownloadHeaders 设置CSV下载响应头。
func setCSVDownloadHeaders(w http.ResponseWriter, filename string) {
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set(
		"Content-Disposition",
		fmt.Sprintf("attachment; filename=%q; filename*=UTF-8''%s", filename, url.PathEscape(filename)),
	)
}

// handleStreamingExportError 记录流式导出错误；响应未提交时返回标准错误页。
func handleStreamingExportError(w http.ResponseWriter, message string, err error, started bool) {
	log.Printf("%s: %v", message, err)
	if started {
		return
	}
	w.Header().Del("Content-Disposition")
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	http.Error(w, message+"："+err.Error(), http.StatusInternalServerError)
}

// historyExportFilename 生成安全的下载文件名。
func historyExportFilename(query model.HistoryExportQuery, now time.Time) string {
	labels := []string{"全部设备"}
	switch {
	case query.DeviceID != "":
		labels = []string{"设备" + query.DeviceID}
	case query.MasterID != "":
		labels = []string{"主站" + query.MasterID}
	case query.ChannelID != "":
		labels = []string{"通道" + query.ChannelID}
	}
	if query.SamplePeriod != "" {
		labels = append(labels, query.SamplePeriod)
	}
	return service.CSVDownloadFilename("历史数据", labels, now)
}

// eventsExportFilename 生成安全的事件 CSV 导出文件名。
func eventsExportFilename(query model.EventExportQuery, now time.Time) string {
	label := "全部"
	if query.Level != "" || query.Source != "" || query.Search != "" || query.TimeRange != "" {
		label = "当前筛选"
	}
	return service.CSVDownloadFilename("历史事件", []string{label}, now)
}
