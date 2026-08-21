package service

// 本文件提供历史数据与历史事件 CSV 的统一分页导出和中文格式化，输出始终带 UTF-8 BOM。

import (
	"context"
	"encoding/csv"
	"fmt"
	"io"
	"math"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	"edge-web/internal/model"
)

const exportCSVPageSize = 500

var utf8BOM = []byte{0xEF, 0xBB, 0xBF}

var historyCSVHeader = []string{
	"采样粒度（sample_period）",
	"历史时间",
	"实际采集时间",
	"设备名称",
	"历史值",
	"单位",
	"数据质量",
	"是否有效",
	"通道名称",
	"通道ID",
	"主站名称",
	"主站ID",
	"设备ID",
	"数据项名称",
	"说明",
}

var eventCSVHeader = []string{
	"最近发生时间",
	"首次发生时间",
	"级别",
	"来源",
	"对象ID",
	"事件摘要",
	"事件详情",
	"发生次数",
	"诊断级别",
	"诊断对象ID",
	"诊断对象名称",
	"诊断状态",
	"错误码",
	"诊断消息",
	"处理建议",
	"连续失败次数",
}

type exportNameMaps struct {
	channelNames map[string]string
	masterNames  map[string]string
	deviceNames  map[string]string
}

// WriteHistoryExportCSV 将历史数据按后端分页结果流式写成 Excel 易读的 CSV。
func (s *ConsoleService) WriteHistoryExportCSV(
	ctx context.Context,
	w io.Writer,
	query model.HistoryExportQuery,
) error {
	nameMaps := exportNameMaps{}
	if snapshot, err := s.backend.GetRealtimeViewSnapshot(ctx); err == nil {
		nameMaps = buildExportNameMaps(snapshot)
	}

	query.Limit = exportCSVPageSize
	query.Offset = 0
	records, err := s.backend.ExportHistoryRecords(ctx, query)
	if err != nil {
		return err
	}

	if _, err := w.Write(utf8BOM); err != nil {
		return err
	}
	writer := csv.NewWriter(w)
	if err := writer.Write(historyCSVHeader); err != nil {
		return err
	}

	for {
		for _, record := range records {
			if err := writer.Write(historyCSVRow(record, nameMaps)); err != nil {
				return err
			}
		}
		writer.Flush()
		if err := writer.Error(); err != nil {
			return err
		}
		if len(records) < exportCSVPageSize {
			break
		}
		query.Offset += len(records)
		records, err = s.backend.ExportHistoryRecords(ctx, query)
		if err != nil {
			return err
		}
	}
	return nil
}

// WriteServiceEventsExportCSV 将历史事件按后端分页结果流式写成 Excel 易读的 CSV。
func (s *ConsoleService) WriteServiceEventsExportCSV(
	ctx context.Context,
	w io.Writer,
	query model.EventExportQuery,
) error {
	query.Limit = exportCSVPageSize
	query.Offset = 0
	events, err := s.backend.ExportServiceEvents(ctx, query)
	if err != nil {
		return err
	}
	// 一次导出固定使用同一份设备名称快照。历史事件页数较多时，避免每翻一页
	// 都额外拉取完整实时视图，减少 IPC、JSON 解码和短期内存分配。
	var currentDevices []model.DeviceConfig
	if snapshot, snapshotErr := s.backend.GetRealtimeViewSnapshot(ctx); snapshotErr == nil {
		currentDevices = snapshot.Devices
		events = applyCurrentDeviceNamesToEvents(events, currentDevices)
	}

	if _, err := w.Write(utf8BOM); err != nil {
		return err
	}
	writer := csv.NewWriter(w)
	if err := writer.Write(eventCSVHeader); err != nil {
		return err
	}

	for {
		for _, event := range events {
			if err := writer.Write(eventCSVRow(event)); err != nil {
				return err
			}
		}
		writer.Flush()
		if err := writer.Error(); err != nil {
			return err
		}
		if len(events) < exportCSVPageSize {
			break
		}
		query.Offset += len(events)
		events, err = s.backend.ExportServiceEvents(ctx, query)
		if err != nil {
			return err
		}
		if currentDevices != nil {
			events = applyCurrentDeviceNamesToEvents(events, currentDevices)
		}
	}
	return nil
}

// writeHistoryCSVDocument 将历史记录写为带表头的 CSV 文档。
func writeHistoryCSVDocument(w io.Writer, records []model.HistoryRecord, names exportNameMaps) error {
	if _, err := w.Write(utf8BOM); err != nil {
		return err
	}
	writer := csv.NewWriter(w)
	if err := writer.Write(historyCSVHeader); err != nil {
		return err
	}
	for _, record := range records {
		if err := writer.Write(historyCSVRow(record, names)); err != nil {
			return err
		}
	}
	writer.Flush()
	return writer.Error()
}

// writeEventCSVDocument 将服务事件写为带表头的 CSV 文档。
func writeEventCSVDocument(w io.Writer, events []model.ServiceEvent) error {
	if _, err := w.Write(utf8BOM); err != nil {
		return err
	}
	writer := csv.NewWriter(w)
	if err := writer.Write(eventCSVHeader); err != nil {
		return err
	}
	for _, event := range events {
		if err := writer.Write(eventCSVRow(event)); err != nil {
			return err
		}
	}
	writer.Flush()
	return writer.Error()
}

// buildExportNameMaps 构建页面或接口使用的结果列表。
func buildExportNameMaps(snapshot model.RealtimeViewSnapshot) exportNameMaps {
	names := exportNameMaps{
		channelNames: make(map[string]string, len(snapshot.Channels)),
		masterNames:  make(map[string]string, len(snapshot.Masters)),
		deviceNames:  make(map[string]string, len(snapshot.Devices)),
	}
	for _, channel := range snapshot.Channels {
		names.channelNames[channel.ChannelID] = strings.TrimSpace(channel.ChannelName)
	}
	for _, master := range snapshot.Masters {
		names.masterNames[master.MasterID] = strings.TrimSpace(master.MasterName)
	}
	for _, device := range snapshot.Devices {
		names.deviceNames[device.DeviceID] = strings.TrimSpace(device.DeviceName)
	}
	return names
}

// historyCSVRow 将历史记录转换为 CSV 行。
func historyCSVRow(record model.HistoryRecord, names exportNameMaps) []string {
	return []string{
		csvSafeText(samplePeriodText(record.SamplePeriod)),
		formatCSVHistoryTime(record),
		formatCSVTime(record.TimestampMS, ""),
		csvSafeText(names.deviceNames[record.DeviceID]),
		formatCSVFloat(record.Value, record.Precision),
		csvSafeText(record.Unit),
		csvSafeText(csvQualityText(record.Quality)),
		csvSafeText(boolText(record.Valid)),
		csvSafeText(names.channelNames[record.ChannelID]),
		csvSafeText(record.ChannelID),
		csvSafeText(names.masterNames[record.MasterID]),
		csvSafeText(record.MasterID),
		csvSafeText(record.DeviceID),
		csvSafeText(record.PointName),
		csvSafeText(record.Message),
	}
}

// eventCSVRow 将事件记录转换为 CSV 行。
func eventCSVRow(event model.ServiceEvent) []string {
	diagnosis := event.Diagnosis
	return []string{
		formatCSVTime(event.TimestampMS, ""),
		formatCSVTime(event.FirstTimestampMS, ""),
		csvSafeText(eventLevelLabel(event.Level)),
		csvSafeText(model.EventSourceLabel(event.Source)),
		csvSafeText(event.TargetID),
		csvSafeText(eventSummaryForExport(event)),
		csvSafeText(eventDetailForExport(event)),
		strconv.FormatUint(event.OccurrenceCount, 10),
		csvSafeText(diagnosisLevelLabel(diagnosis.Level)),
		csvSafeText(diagnosis.TargetID),
		csvSafeText(diagnosis.TargetName),
		csvSafeText(diagnosisStatusLabel(diagnosis.Status)),
		csvSafeText(diagnosis.ErrorCode),
		csvSafeText(diagnosis.Message),
		csvSafeText(diagnosis.Suggestion),
		strconv.FormatUint(uint64(diagnosis.ConsecutiveFailures), 10),
	}
}

// csvSafeText 只处理明确的文本列；数值和时间列保持原格式供表格软件识别。
// 风险判断忽略前导空白，但安全前缀加在原文之前，不裁剪或改写原始内容。
func csvSafeText(value string) string {
	effective := strings.TrimLeftFunc(value, func(r rune) bool {
		return unicode.IsSpace(r) || r == '\uFEFF'
	})
	if effective == "" {
		return value
	}
	first, _ := utf8.DecodeRuneInString(effective)
	switch first {
	case '=', '+', '-', '@':
		return "'" + value
	default:
		return value
	}
}

// formatCSVTime 格式化CSV时间。
func formatCSVTime(ms uint64, fallback string) string {
	if ms == 0 {
		return fallback
	}
	return time.UnixMilli(int64(ms)).Local().Format("2006-01-02 15:04:05")
}

// formatCSVHistoryTime 格式化CSV历史数据时间。
func formatCSVHistoryTime(record model.HistoryRecord) string {
	if record.BucketStartMS != 0 {
		return time.UnixMilli(int64(record.BucketStartMS)).Local().Format(historyBucketTimeLayout(record.SamplePeriod))
	}
	if record.TimestampMS != 0 {
		return time.UnixMilli(int64(record.TimestampMS)).Local().Format(historyBucketTimeLayout(record.SamplePeriod))
	}
	return firstNonEmpty(record.BucketText, record.Date)
}

// formatCSVFloat 按指定精度格式化 CSV 浮点值。
func formatCSVFloat(value float64, precision uint32) string {
	if math.IsNaN(value) || math.IsInf(value, 0) {
		return ""
	}
	if precision > 12 {
		precision = 12
	}
	return fmt.Sprintf("%.*f", precision, value)
}

// samplePeriodText 返回历史采样周期的中文名称。
func samplePeriodText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "hour":
		return "小时"
	case "day":
		return "天"
	case "raw_10min":
		return "10 分钟基础样本"
	default:
		return strings.TrimSpace(value)
	}
}

// csvQualityText 返回 CSV 中使用的采集质量文本。
func csvQualityText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "good":
		return "良好"
	case "bad":
		return "异常"
	case "stale":
		return "过期"
	default:
		return "暂无数据"
	}
}

// boolText 将布尔值转换为“是”或“否”。
func boolText(value bool) string {
	if value {
		return "是"
	}
	return "否"
}

// eventLevelLabel 返回事件级别的中文标签。
func eventLevelLabel(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "错误"
	case "warning", "warn":
		return "告警"
	case "info":
		return "信息"
	default:
		return strings.TrimSpace(level)
	}
}

// eventSummaryForExport 生成事件导出使用的摘要文本。
func eventSummaryForExport(event model.ServiceEvent) string {
	if summary := strings.TrimSpace(event.Summary); summary != "" {
		return userVisibleAlarmLimitText(summary)
	}
	return userVisibleAlarmLimitText(strings.TrimSpace(event.Diagnosis.Message))
}

// eventDetailForExport 生成事件导出使用的详情文本。
func eventDetailForExport(event model.ServiceEvent) string {
	if detail := strings.TrimSpace(event.Detail); detail != "" {
		return userVisibleAlarmLimitText(detail)
	}
	return userVisibleAlarmLimitText(strings.TrimSpace(event.Diagnosis.Suggestion))
}

// diagnosisLevelLabel 返回诊断级别的中文标签。
func diagnosisLevelLabel(level string) string {
	return eventLevelLabel(level)
}

// diagnosisStatusLabel 返回诊断状态的中文标签。
func diagnosisStatusLabel(status string) string {
	switch strings.ToLower(strings.TrimSpace(status)) {
	case "normal", "ok", "healthy":
		return "正常"
	case "warning", "warn", "degraded":
		return "告警"
	case "error", "failed", "fault", "abnormal":
		return "异常"
	case "offline", "disconnected":
		return "离线"
	case "disabled":
		return "已禁用"
	case "unknown":
		return "未知"
	default:
		return strings.TrimSpace(status)
	}
}

// firstNonEmpty 返回首个非空文本。
func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return strings.TrimSpace(value)
		}
	}
	return ""
}

// CSVDownloadFilename 按统一规则生成下载文件名，保留中文并移除路径分隔符等非法字符。
func CSVDownloadFilename(prefix string, labels []string, now time.Time) string {
	parts := []string{SanitizeCSVFilenamePart(prefix)}
	for _, label := range labels {
		if sanitized := SanitizeCSVFilenamePart(label); sanitized != "" && sanitized != "导出" {
			parts = append(parts, sanitized)
		}
	}
	parts = append(parts, now.Format("20060102_150405"))
	return strings.Join(parts, "_") + ".csv"
}

// SanitizeCSVFilenamePart 移除下载文件名片段中的非法字符。
func SanitizeCSVFilenamePart(value string) string {
	value = strings.TrimSpace(value)
	if value == "" {
		return "导出"
	}

	var builder strings.Builder
	builder.Grow(len(value))
	lastUnderscore := false
	for _, r := range value {
		replace := r == '/' || r == '\\' || r == ':' || r == '*' || r == '?' ||
			r == '"' || r == '<' || r == '>' || r == '|' || unicode.IsControl(r)
		if replace || unicode.IsSpace(r) {
			if !lastUnderscore {
				builder.WriteRune('_')
				lastUnderscore = true
			}
			continue
		}
		builder.WriteRune(r)
		lastUnderscore = false
	}

	sanitized := strings.Trim(builder.String(), "._- ")
	if sanitized == "" || !utf8.ValidString(sanitized) {
		return "导出"
	}
	return truncateRunes(sanitized, 80)
}

// truncateRunes 按字符数截断文本，避免切断 UTF-8 编码。
func truncateRunes(value string, limit int) string {
	if limit <= 0 {
		return ""
	}
	if utf8.RuneCountInString(value) <= limit {
		return value
	}
	runes := []rune(value)
	return string(runes[:limit])
}
