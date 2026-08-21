package service

// 本文件生成只读诊断 ZIP：事件 CSV、运行态 JSON 和固定白名单日志彼此独立容错，
// 严禁读取任意请求路径，也不导出数据库、证书内容或 MQTT 明文密码。

import (
	"archive/zip"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"edge-web/internal/model"
)

const (
	diagnosticFormat        = "edge-controller-diagnostic"
	diagnosticVersion       = 1
	defaultDiagnosticRoot   = "/opt/edge-controller"
	defaultDiagnosticLogDir = "/opt/edge-controller/log"
	maxDiagnosticLogBytes   = int64(5 * 1024 * 1024)
)

var diagnosticLogFileNames = []string{
	"edge-controller.log",
	"edge-controller.log.1",
	"edge-controller.log.2",
	"edge-controller.log.3",
	"edge-web.log",
	"edge-web.log.1",
	"edge-web.log.2",
	"edge-web.log.3",
	"update.log",
	"update.log.1",
	"update.log.2",
	"update.log.3",
}

var diagnosticProductRoot = defaultDiagnosticRoot

var diagnosticProductFiles = []struct {
	sourcePath  string
	archivePath string
	label       string
}{
	{sourcePath: "VERSION", archivePath: "product/VERSION", label: "安装版本"},
	{sourcePath: "package-info.json", archivePath: "product/package-info.json", label: "发布包信息"},
	{sourcePath: "update/status.json", archivePath: "product/update-status.json", label: "升级状态"},
}

type diagnosticManifest struct {
	Format        string                   `json:"format"`
	Version       int                      `json:"version"`
	GeneratedAt   string                   `json:"generated_at"`
	GeneratedAtMS int64                    `json:"generated_at_ms"`
	LogDirectory  string                   `json:"log_directory"`
	EventFilter   diagnosticEventFilter    `json:"event_filter"`
	Files         []diagnosticManifestFile `json:"files"`
	Warnings      []string                 `json:"warnings"`
	Errors        []string                 `json:"errors"`
}

type diagnosticEventFilter struct {
	Level     string `json:"level"`
	Source    string `json:"source"`
	TimeRange string `json:"time_range"`
	Search    string `json:"search"`
}

type diagnosticManifestFile struct {
	Path              string `json:"path"`
	Category          string `json:"category"`
	SizeBytes         int64  `json:"size_bytes"`
	OriginalSizeBytes int64  `json:"original_size_bytes,omitempty"`
	Truncated         bool   `json:"truncated,omitempty"`
}

type diagnosticArchiveWriter struct {
	zip      *zip.Writer
	now      time.Time
	manifest *diagnosticManifest
}

type diagnosticRuntimeItem struct {
	path  string
	label string
	load  func() (any, error)
}

type diagnosticCountingWriter struct {
	writer io.Writer
	size   int64
}

func (w *diagnosticCountingWriter) Write(payload []byte) (int, error) {
	n, err := w.writer.Write(payload)
	w.size += int64(n)
	return n, err
}

// WriteDiagnosticExport 生成面向现场排查的诊断 ZIP。单个数据源失败会写入
// manifest，并为对应 runtime JSON 生成错误占位，不影响其余内容导出。
func (s *ConsoleService) WriteDiagnosticExport(
	ctx context.Context,
	w io.Writer,
	query model.EventExportQuery,
	now time.Time,
) error {
	if s == nil || s.backend == nil {
		return fmt.Errorf("诊断导出服务尚未初始化")
	}
	if now.IsZero() {
		now = time.Now()
	}

	// 初始化文件清单和 ZIP 写入器，后续每项内容均同步登记到清单。
	logDirectory := diagnosticLogDirectory()
	manifest := diagnosticManifest{
		Format:        diagnosticFormat,
		Version:       diagnosticVersion,
		GeneratedAt:   now.Format(time.RFC3339),
		GeneratedAtMS: now.UnixMilli(),
		LogDirectory:  logDirectory,
		EventFilter: diagnosticEventFilter{
			Level:     normalizedDiagnosticFilter(query.Level, "all"),
			Source:    normalizedDiagnosticFilter(query.Source, "all"),
			TimeRange: normalizedDiagnosticFilter(query.TimeRange, "all"),
			Search:    strings.TrimSpace(query.Search),
		},
		Files:    make([]diagnosticManifestFile, 0, 16),
		Warnings: make([]string, 0),
		Errors:   make([]string, 0),
	}

	archive := &diagnosticArchiveWriter{
		zip:      zip.NewWriter(w),
		now:      now,
		manifest: &manifest,
	}

	// 固定产品信息直接读取正式安装目录，不依赖 controller IPC；任一文件缺失只记入清单。
	for _, item := range diagnosticProductFiles {
		source := filepath.Join(diagnosticProductRoot, filepath.FromSlash(item.sourcePath))
		data, originalSize, truncated, readErr := readDiagnosticFileTail(source)
		if readErr != nil {
			if errors.Is(readErr, os.ErrNotExist) {
				manifest.Warnings = append(manifest.Warnings, item.archivePath+"：文件不存在")
			} else {
				manifest.Errors = append(manifest.Errors, item.archivePath+"：读取失败："+readErr.Error())
			}
			continue
		}
		if truncated {
			manifest.Warnings = append(manifest.Warnings,
				fmt.Sprintf("%s：原文件 %d 字节，仅导出末尾 %d 字节", item.archivePath, originalSize, len(data)))
		}
		if err := archive.addFile(item.archivePath, item.label, data, originalSize, truncated); err != nil {
			return err
		}
	}

	// 事件 CSV 直接写入 ZIP 条目，避免历史库增长后同时在内存中保留
	// “完整 CSV + 压缩后 ZIP”两份大对象。
	eventEntry, err := archive.createFile("exports/历史事件.csv")
	if err != nil {
		return err
	}
	eventWriter := &diagnosticCountingWriter{writer: eventEntry}
	if err := s.WriteServiceEventsExportCSV(ctx, eventWriter, query); err != nil {
		manifest.Errors = append(manifest.Errors, "exports/历史事件.csv：历史事件获取未完成："+err.Error())
		if eventWriter.size == 0 {
			if fallbackErr := writeEventCSVDocument(eventWriter, nil); fallbackErr != nil {
				return fmt.Errorf("生成历史事件 CSV 占位文件失败: %w", fallbackErr)
			}
		}
	}
	manifest.Files = append(manifest.Files, diagnosticManifestFile{
		Path: "exports/历史事件.csv", Category: "历史事件", SizeBytes: eventWriter.size,
	})

	// 汇总诊断所需的运行态数据源，并在导出前清除敏感字段。
	runtimeItems := []diagnosticRuntimeItem{
		{
			path: "runtime/系统运行快照.json", label: "系统运行快照",
			load: func() (any, error) {
				value, err := s.backend.GetSystemOverviewSnapshot(ctx)
				return value, err
			},
		},
		{
			path: "runtime/系统状态.json", label: "系统状态",
			load: func() (any, error) {
				value, err := s.backend.GetSystemStatus(ctx)
				return value, err
			},
		},
		{
			path: "runtime/轮询摘要.json", label: "轮询摘要",
			load: func() (any, error) {
				value, err := s.backend.GetPollingSummary(ctx)
				return value, err
			},
		},
		{
			path: "runtime/最近错误.json", label: "最近错误",
			load: func() (any, error) {
				value, err := s.backend.GetRecentError(ctx)
				return value, err
			},
		},
		{
			path: "runtime/配置摘要.json", label: "配置摘要",
			load: func() (any, error) {
				value, err := s.backend.GetConfigSummary(ctx)
				return value, err
			},
		},
		{
			path: "runtime/网络配置.json", label: "网络配置",
			load: func() (any, error) {
				value, err := s.backend.GetNetworkSettings(ctx)
				return value, err
			},
		},
		{
			path: "runtime/网络运行状态.json", label: "网络运行状态",
			load: func() (any, error) {
				value, err := s.backend.GetNetworkRuntimeStatus(ctx)
				return value, err
			},
		},
		{
			path: "runtime/时间配置.json", label: "时间配置",
			load: func() (any, error) {
				value, err := s.getTimeSettings(ctx)
				return value, err
			},
		},
		{
			path: "runtime/时间运行状态.json", label: "时间运行状态",
			load: func() (any, error) {
				value, err := s.getTimeRuntimeStatus(ctx)
				return value, err
			},
		},
		{
			path: "runtime/MQTT配置.json", label: "MQTT 配置",
			load: func() (any, error) {
				value, err := s.backend.GetMqttSettings(ctx)
				value.Password = ""
				return value, err
			},
		},
		{
			path: "runtime/MQTT运行状态.json", label: "MQTT 运行状态",
			load: func() (any, error) {
				value, err := s.backend.GetMqttRuntimeStatus(ctx)
				return value, err
			},
		},
	}

	// 逐项获取并序列化运行态快照，单项失败不会中断其余内容。
	for _, item := range runtimeItems {
		value, loadErr := item.load()
		if loadErr != nil {
			manifest.Errors = append(manifest.Errors, item.path+"：获取失败："+loadErr.Error())
			value = map[string]any{
				"available": false,
				"error":     loadErr.Error(),
			}
		}
		data, marshalErr := marshalDiagnosticJSON(value)
		if marshalErr != nil {
			manifest.Errors = append(manifest.Errors, item.path+"：JSON 编码失败："+marshalErr.Error())
			data, _ = marshalDiagnosticJSON(map[string]any{
				"available": false,
				"error":     "JSON 编码失败：" + marshalErr.Error(),
			})
		}
		if err := archive.addFile(item.path, item.label, data, 0, false); err != nil {
			return err
		}
	}

	// 收集进程日志尾部，控制诊断包大小并记录截断情况。
	for _, fileName := range diagnosticLogFileNames {
		path := filepath.Join(logDirectory, fileName)
		data, originalSize, truncated, err := readDiagnosticFileTail(path)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				manifest.Warnings = append(manifest.Warnings, "logs/"+fileName+"：日志文件不存在")
			} else {
				manifest.Errors = append(manifest.Errors, "logs/"+fileName+"：读取失败："+err.Error())
			}
			continue
		}
		if truncated {
			manifest.Warnings = append(
				manifest.Warnings,
				fmt.Sprintf("logs/%s：原文件 %d 字节，仅导出末尾 %d 字节", fileName, originalSize, len(data)),
			)
		}
		if err := archive.addFile("logs/"+fileName, "进程日志", data, originalSize, truncated); err != nil {
			return err
		}
	}

	// 写入说明和最终文件清单，然后关闭 ZIP 以完成归档。
	readme := diagnosticReadme(manifest, query)
	if err := archive.addFile("README.txt", "使用说明", []byte(readme), 0, false); err != nil {
		return err
	}

	manifest.Files = append(manifest.Files, diagnosticManifestFile{
		Path:     "manifest.json",
		Category: "文件清单",
	})
	manifestJSON, err := marshalDiagnosticJSON(manifest)
	if err != nil {
		return fmt.Errorf("生成诊断包 manifest 失败: %w", err)
	}
	if err := archive.writeFile("manifest.json", manifestJSON); err != nil {
		return err
	}
	if err := archive.zip.Close(); err != nil {
		return fmt.Errorf("完成诊断 ZIP 失败: %w", err)
	}
	return nil
}

// addFile 向诊断包写入文件，并同步登记大小和截断信息。
func (a *diagnosticArchiveWriter) addFile(
	path string,
	category string,
	data []byte,
	originalSize int64,
	truncated bool,
) error {
	if err := a.writeFile(path, data); err != nil {
		return err
	}
	file := diagnosticManifestFile{
		Path:      path,
		Category:  category,
		SizeBytes: int64(len(data)),
		Truncated: truncated,
	}
	if truncated || originalSize > int64(len(data)) {
		file.OriginalSizeBytes = originalSize
	}
	a.manifest.Files = append(a.manifest.Files, file)
	return nil
}

func (a *diagnosticArchiveWriter) writeFile(path string, data []byte) error {
	entry, err := a.createFile(path)
	if err != nil {
		return err
	}
	if _, err := entry.Write(data); err != nil {
		return fmt.Errorf("写入诊断包文件 %s 失败: %w", path, err)
	}
	return nil
}

// createFile 创建一个新的 ZIP 条目；调用方可将大内容直接流式写入该条目。
func (a *diagnosticArchiveWriter) createFile(path string) (io.Writer, error) {
	header := &zip.FileHeader{Name: path, Method: zip.Deflate}
	header.SetModTime(a.now)
	header.SetMode(0o644)
	entry, err := a.zip.CreateHeader(header)
	if err != nil {
		return nil, fmt.Errorf("创建诊断包文件 %s 失败: %w", path, err)
	}
	return entry, nil
}

func marshalDiagnosticJSON(value any) ([]byte, error) {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(data, '\n'), nil
}

func diagnosticLogDirectory() string {
	if value := strings.TrimSpace(os.Getenv("EDGE_CONTROLLER_LOG_DIR")); value != "" {
		return filepath.Clean(value)
	}
	return defaultDiagnosticLogDir
}

// readDiagnosticFileTail 安全读取固定白名单普通文件尾部，并返回原始大小和截断状态。
func readDiagnosticFileTail(path string) ([]byte, int64, bool, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, 0, false, err
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return nil, 0, false, fmt.Errorf("拒绝读取符号链接")
	}
	if !info.Mode().IsRegular() {
		return nil, 0, false, fmt.Errorf("不是普通日志文件")
	}

	file, err := os.Open(path)
	if err != nil {
		return nil, 0, false, err
	}
	defer file.Close()

	openedInfo, err := file.Stat()
	if err != nil {
		return nil, 0, false, err
	}
	if !openedInfo.Mode().IsRegular() || !os.SameFile(info, openedInfo) {
		return nil, 0, false, fmt.Errorf("日志文件在读取前发生变化")
	}

	originalSize := openedInfo.Size()
	offset := int64(0)
	truncated := originalSize > maxDiagnosticLogBytes
	if truncated {
		offset = originalSize - maxDiagnosticLogBytes
	}
	data, err := io.ReadAll(io.NewSectionReader(file, offset, originalSize-offset))
	if err != nil {
		return nil, originalSize, truncated, err
	}
	return data, originalSize, truncated, nil
}

func normalizedDiagnosticFilter(value string, fallback string) string {
	if value = strings.TrimSpace(value); value != "" {
		return value
	}
	return fallback
}

func diagnosticReadme(manifest diagnosticManifest, query model.EventExportQuery) string {
	filterLines := []string{
		"- 级别：" + diagnosticLevelText(query.Level),
		"- 来源：" + diagnosticSourceText(query.Source),
		"- 时间范围：" + diagnosticTimeRangeText(query.TimeRange),
		"- 搜索词：" + normalizedDiagnosticFilter(query.Search, "无"),
	}
	return strings.Join([]string{
		"边缘计算控制器诊断包",
		"====================",
		"",
		"生成时间：" + manifest.GeneratedAt,
		"格式版本：" + fmt.Sprintf("%d", manifest.Version),
		"",
		"本诊断包用于现场问题回传和故障排查，内容包括：",
		"1. exports/历史事件.csv：沿用历史事件页当前筛选条件，使用 UTF-8 BOM 和中文表头。",
		"2. product/*：不依赖 controller 的安装版本、发布包信息和持久化升级状态。",
		"3. runtime/*.json：系统、轮询、网络、时间和 MQTT 的只读运行快照。",
		"4. logs/*：固定白名单内实际存在的 controller、Web 和升级日志；单文件超过 5MB 时只保留末尾 5MB。",
		"5. manifest.json：文件清单、日志截断提示、缺失文件警告和数据获取错误。",
		"",
		"历史事件筛选：",
		strings.Join(filterLines, "\n"),
		"",
		"安全说明：",
		"- 不包含数据库原文件。",
		"- 不包含证书或私钥文件内容。",
		"- MQTT 密码字段已清空，不导出明文密码。",
		"- 某项运行态获取失败时，对应 JSON 会记录错误占位，详细信息同时写入 manifest errors。",
		"",
		fmt.Sprintf("导出警告：%d 项；导出错误：%d 项。", len(manifest.Warnings), len(manifest.Errors)),
		"",
	}, "\n")
}

func diagnosticLevelText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "error":
		return "错误"
	case "warning", "warn":
		return "告警"
	case "info":
		return "信息"
	default:
		return "全部级别"
	}
}

func diagnosticSourceText(value string) string {
	if value = strings.TrimSpace(value); value != "" {
		return model.EventSourceLabel(value)
	}
	return "全部来源"
}

func diagnosticTimeRangeText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "24h":
		return "最近 24 小时"
	case "3d":
		return "最近 3 天"
	case "7d":
		return "最近 7 天"
	default:
		return "全部"
	}
}

func DiagnosticExportFilename(now time.Time) string {
	return "edge-controller-diagnostic-" + now.Format("20060102-150405") + ".zip"
}
