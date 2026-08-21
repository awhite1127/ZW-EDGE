package httpserver

// 本文件加载页面模板并注册只读格式化函数；模板函数不得执行 IPC、文件写入或状态变更。

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"html/template"
	"io/fs"
	"net/url"
	"path/filepath"
	"sort"
	"strings"
	"time"

	webassets "edge-web"
	"edge-web/internal/model"
)

// staticAssetVersion 在进程初始化时只计算一次。相同的内嵌 CSS/JS 得到相同版本，
// 任一资源变化都会自动改变 URL，从而安全保留 immutable 长缓存。
var staticAssetVersion = embeddedStaticAssetFingerprint()

func embeddedStaticAssetFingerprint() string {
	return staticAssetFingerprint(webassets.StaticFS)
}

func staticAssetFingerprint(assets fs.FS) string {
	var names []string
	_ = fs.WalkDir(assets, "static", func(path string, entry fs.DirEntry, err error) error {
		if err == nil && !entry.IsDir() {
			ext := strings.ToLower(filepath.Ext(path))
			if ext == ".css" || ext == ".js" {
				names = append(names, path)
			}
		}
		return nil
	})
	sort.Strings(names)
	hash := sha256.New()
	for _, name := range names {
		content, err := fs.ReadFile(assets, name)
		if err != nil {
			panic("read embedded static asset: " + err.Error())
		}
		hash.Write([]byte(name))
		hash.Write([]byte{0})
		hash.Write(content)
		hash.Write([]byte{0})
	}
	return fmt.Sprintf("%x", hash.Sum(nil)[:8])
}

// loadPageTemplates 为每个页面加载公共布局、局部模板和页面模板。
func loadPageTemplates(templateDir string) (map[string]*template.Template, error) {
	// 页面键与入口模板文件一一对应。
	pageFiles := map[string]string{
		"overview":                "overview.html",
		"collection":              "collection.html",
		"communication_traces":    "communication_traces.html",
		"history":                 "history.html",
		"device_history":          "device_history.html",
		"realtime":                "realtime.html",
		"events":                  "events.html",
		"settings":                "settings.html",
		"settings_device_types":   "settings_device_types.html",
		"device_template_editor":  "device_template_editor.html",
		"settings_mqtt":           "settings_mqtt.html",
		"settings_modbus_server":  "settings_modbus_server.html",
		"operations":              "operations.html",
		"operations_users":        "operations_users.html",
		"application_update_wait": "application_update_wait.html",
		"login":                   "login.html",
	}

	// 集中注册模板可调用的只读格式化函数。
	funcMap := template.FuncMap{
		"formatTimestamp":          formatTimestamp,
		"formatDiagnosisTime":      formatDiagnosisTime,
		"boolText":                 boolText,
		"yesNoClass":               yesNoClass,
		"defaultText":              defaultText,
		"diagnosisSuggestionText":  diagnosisSuggestionText,
		"diagnosisLastSuccessTime": diagnosisLastSuccessTime,
		"diagnosisLastErrorTime":   diagnosisLastErrorTime,
		"userErrorText":            userVisibleErrorText,
		"channelErrorText":         channelErrorText,
		"boolStateText":            boolStateText,
		"channelStateText":         channelStateText,
		"channelStateClass":        channelStateClass,
		"channelDiagnosticText":    channelDiagnosticText,
		"channelDiagnosticClass":   channelDiagnosticClass,
		"masterDiagnosticText":     masterDiagnosticText,
		"masterDiagnosticClass":    masterDiagnosticClass,
		"deviceDiagnosticText":     deviceDiagnosticText,
		"deviceDiagnosticClass":    deviceDiagnosticClass,
		"eventLevelText":           eventLevelText,
		"eventLevelClass":          eventLevelClass,
		"eventTypeText":            eventTypeText,
		"eventTargetText":          eventTargetText,
		"eventSummaryText":         eventSummaryText,
		"eventRepeatText":          eventRepeatText,
		"alarmLevelText":           alarmLevelText,
		"alarmDirectionText":       alarmDirectionText,
		"roleLabel":                roleText,
		"mqttStateText":            mqttStateText,
		"mqttStateClass":           mqttStateClass,
		"formatAlarmValue":         formatAlarmValue,
		"registerAreaText":         registerAreaText,
		"functionCodeText":         functionCodeText,
		"displayProductVersion":    displayProductVersion,
		"jsonAttr":                 jsonAttr,
		"urlQuery":                 url.QueryEscape,
		"urlPath":                  url.PathEscape,
		"hasPrefix":                strings.HasPrefix,
		"staticAssetVersion":       func() string { return staticAssetVersion },
	}

	// 每个页面独立解析模板集合，避免不同页面的同名定义相互覆盖。
	templates := make(map[string]*template.Template, len(pageFiles))
	useEmbeddedTemplates := strings.TrimSpace(templateDir) == ""
	layoutFile := filepath.Join(templateDir, "layout.html")
	partialFiles := []string{
		filepath.Join(templateDir, "partials", "channels_panel.html"),
		filepath.Join(templateDir, "partials", "masters_panel.html"),
		filepath.Join(templateDir, "partials", "devices_panel.html"),
	}

	for page, file := range pageFiles {
		var (
			tpl *template.Template
			err error
		)
		if useEmbeddedTemplates {
			tpl, err = template.New("layout").Funcs(funcMap).ParseFS(
				webassets.TemplateFS,
				"templates/layout.html",
				"templates/"+file,
				"templates/partials/channels_panel.html",
				"templates/partials/masters_panel.html",
				"templates/partials/devices_panel.html",
			)
		} else {
			files := append([]string{
				layoutFile,
				filepath.Join(templateDir, file),
			}, partialFiles...)
			tpl, err = template.New("layout").Funcs(funcMap).ParseFiles(files...)
		}
		if err != nil {
			return nil, err
		}
		templates[page] = tpl
	}

	return templates, nil
}

// displayProductVersion 只在页面展示层增加 V；文件、IPC 和 API 始终保留纯版本号。
func displayProductVersion(value string) string {
	value = strings.TrimSpace(value)
	if value == "" {
		return ""
	}
	value = strings.TrimPrefix(strings.TrimPrefix(value, "V"), "v")
	return "V" + value
}

// jsonAttr 将数据安全序列化为 HTML 属性值。
func jsonAttr(value interface{}) string {
	payload, err := json.Marshal(value)
	if err != nil {
		return "{}"
	}
	return string(payload)
}

// formatTimestamp 将毫秒时间戳格式化为页面时间文本。
func formatTimestamp(ms uint64) string {
	if ms == 0 {
		return "-"
	}
	if ms < 1_000_000_000_000 {
		return fmt.Sprintf("%d ms", ms)
	}
	return time.UnixMilli(int64(ms)).Local().Format("2006-01-02 15:04:05")
}

// formatDiagnosisTime 格式化诊断时间。
func formatDiagnosisTime(ms uint64) string {
	if ms == 0 {
		return "暂无记录"
	}
	return formatTimestamp(ms)
}

// boolText 将布尔值转换为“是”或“否”。
func boolText(value bool) string {
	if value {
		return "是"
	}
	return "否"
}

// boolStateText 将布尔值转换为启用状态文本。
func boolStateText(value bool) string {
	if value {
		return "在线"
	}
	return "离线"
}

// mqttStateText 返回 MQTT 连接状态的中文说明。
func mqttStateText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "disabled":
		return "未启用"
	case "disconnected", "unavailable":
		return "未连接"
	case "connecting":
		return "连接中"
	case "connected":
		return "已连接"
	case "error":
		return "异常"
	default:
		return defaultText(value)
	}
}

// mqttStateClass 返回当前状态对应的 CSS 样式类名。
func mqttStateClass(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "connected":
		return "status-ok"
	case "connecting":
		return "status-warn"
	case "error":
		return "status-bad"
	default:
		return "status-neutral"
	}
}

// registerAreaText 返回 Modbus 寄存器区域的中文名称。
func registerAreaText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "holding_register":
		return "保持寄存器"
	default:
		return defaultText(value)
	}
}

// functionCodeText 返回 Modbus 功能码的中文说明。
func functionCodeText(code uint32) string {
	if code == 0 {
		return "-"
	}
	return fmt.Sprintf("FC%02X", code)
}

// diagnosisHasIssue 判断诊断状态是否包含有效问题。
func diagnosisHasIssue(diagnosis model.DiagnosisStatus) bool {
	code := strings.TrimSpace(diagnosis.ErrorCode)
	if code == "" || strings.EqualFold(code, "NONE") {
		return false
	}
	status := strings.ToLower(strings.TrimSpace(diagnosis.Status))
	return status != "normal"
}

// diagnosisSuggestionText 返回诊断建议，无建议时使用默认提示。
func diagnosisSuggestionText(diagnosis model.DiagnosisStatus) string {
	if !diagnosisHasIssue(diagnosis) {
		return ""
	}
	return strings.TrimSpace(diagnosis.Suggestion)
}

// diagnosisLastSuccessTime 格式化诊断最近成功时间。
func diagnosisLastSuccessTime(diagnosis model.DiagnosisStatus, fallback uint64) uint64 {
	if diagnosis.LastSuccessTimeMS > 0 {
		return diagnosis.LastSuccessTimeMS
	}
	return fallback
}

// diagnosisLastErrorTime 格式化诊断最近失败时间。
func diagnosisLastErrorTime(diagnosis model.DiagnosisStatus, fallback uint64) uint64 {
	if diagnosis.LastErrorTimeMS > 0 {
		return diagnosis.LastErrorTimeMS
	}
	return fallback
}

// channelStateText 根据配置和运行状态返回通道状态说明。
func channelStateText(hasStatus bool, status model.ChannelStatus) string {
	if !hasStatus {
		return "-"
	}
	if diagnosisHasIssue(status.Diagnosis) {
		return "异常"
	}
	switch status.Status {
	case "online":
		return "正常"
	case "fault":
		return "异常"
	case "disabled":
		return "已禁用"
	case "closed":
		if status.Enabled {
			return "未连接"
		}
		return "已禁用"
	default:
		if status.Opened {
			return "正常"
		}
		if !status.Enabled {
			return "已禁用"
		}
		if status.LastErrorMessage != "" {
			return "异常"
		}
		return "未连接"
	}
}

// channelStateClass 返回当前状态对应的 CSS 样式类名。
func channelStateClass(hasStatus bool, status model.ChannelStatus) string {
	if !hasStatus {
		return "status-neutral"
	}
	if diagnosisHasIssue(status.Diagnosis) {
		return "status-warn"
	}
	switch status.Status {
	case "online":
		return "status-ok"
	case "fault":
		return "status-warn"
	case "disabled":
		return "status-neutral"
	case "closed":
		if status.Enabled {
			return "status-warn"
		}
		return "status-neutral"
	default:
		if status.Opened {
			return "status-ok"
		}
		if status.LastErrorMessage != "" {
			return "status-warn"
		}
		return "status-neutral"
	}
}

// 列表诊断把启用、运行和质量状态收口成一个前端展示结论，不改变后端状态语义。
func channelDiagnosticText(row model.ChannelRow) string {
	if !row.Config.Enabled {
		return "未启用"
	}
	if !row.HasStatus {
		return "暂无运行状态"
	}
	if message := strings.TrimSpace(row.Status.LastErrorMessage); message != "" {
		return channelErrorText(message)
	}
	if diagnosisHasIssue(row.Status.Diagnosis) {
		if message := strings.TrimSpace(row.Status.Diagnosis.Message); message != "" {
			return userVisibleErrorText(message)
		}
		return "通道运行异常"
	}
	if row.Status.Opened && (row.Status.Status == "" || row.Status.Status == "online" || row.Status.Status == "opened") {
		return "无"
	}
	if row.Status.Status == "closed" || !row.Status.Opened {
		return "通道未打开"
	}
	return "通道运行异常"
}

// channelDiagnosticClass 返回当前状态对应的 CSS 样式类名。
func channelDiagnosticClass(row model.ChannelRow) string {
	if channelDiagnosticText(row) == "无" {
		return "table-message table-message-neutral"
	}
	if !row.Config.Enabled {
		return "status-neutral"
	}
	return "table-message table-message-bad"
}

// 主站列表将启用、在线与诊断状态合并为一个展示结论，避免重复状态列。
func masterDiagnosticText(row model.MasterRow) string {
	if !row.Config.Enabled {
		return "未启用"
	}
	if !row.HasStatus {
		return "暂无运行状态"
	}
	if message := strings.TrimSpace(row.Status.LastErrorMessage); message != "" {
		return userVisibleErrorText(message)
	}
	if masterDiagnosisHasIssue(row.Status.Diagnosis) {
		if message := strings.TrimSpace(row.Status.Diagnosis.Message); message != "" {
			return userVisibleErrorText(message)
		}
		return "主站运行异常"
	}
	if row.Status.Online && row.Status.LastCollectSuccess {
		return "无"
	}
	if !row.Status.Online {
		return "主站通讯异常"
	}
	return "最近采集失败"
}

// masterDiagnosisHasIssue 根据输入完成单一的数据转换并返回结果。
func masterDiagnosisHasIssue(diagnosis model.DiagnosisStatus) bool {
	if diagnosisHasIssue(diagnosis) {
		return true
	}
	switch strings.ToLower(strings.TrimSpace(diagnosis.Level)) {
	case "warning", "warn", "error", "critical", "bad":
		return true
	}
	switch strings.ToLower(strings.TrimSpace(diagnosis.Status)) {
	case "", "normal", "online", "ok", "opened", "running":
		return false
	default:
		return true
	}
}

// masterDiagnosticClass 返回当前状态对应的 CSS 样式类名。
func masterDiagnosticClass(row model.MasterRow) string {
	if masterDiagnosticText(row) == "无" {
		return "table-message table-message-neutral"
	}
	if !row.Config.Enabled || !row.HasStatus {
		return "status-neutral"
	}
	return "table-message table-message-bad"
}

// deviceDiagnosticText 返回设备行中最具体的诊断说明。
func deviceDiagnosticText(row model.DeviceRow) string {
	if !row.Config.Enabled {
		return "未启用"
	}
	if !row.HasStatus {
		return "暂无采集状态"
	}
	if message := strings.TrimSpace(row.Status.LastErrorMessage); message != "" {
		return userVisibleErrorText(message)
	}
	if diagnosisHasIssue(row.Status.Diagnosis) {
		if message := strings.TrimSpace(row.Status.Diagnosis.Message); message != "" {
			return userVisibleErrorText(message)
		}
		return "设备通讯异常"
	}
	if !row.Status.LastCollectSuccess {
		return "最近采集失败"
	}
	if !row.Status.Online {
		return "设备通讯异常"
	}
	if len(row.SummaryPoints) == 0 {
		return "暂无有效数据"
	}
	for _, point := range row.SummaryPoints {
		quality := strings.ToLower(strings.TrimSpace(point.Quality))
		qualityOK := quality == "" || quality == "良好" || quality == "正常" || quality == "good" || quality == "normal" || quality == "ok"
		if !point.Valid || !qualityOK {
			if message := strings.TrimSpace(point.Message); message != "" {
				return userVisibleErrorText(message)
			}
			if name := strings.TrimSpace(point.Name); name != "" {
				return name + "数据异常"
			}
			return "数据质量异常"
		}
	}
	return "无"
}

// deviceDiagnosticClass 返回当前状态对应的 CSS 样式类名。
func deviceDiagnosticClass(row model.DeviceRow) string {
	if deviceDiagnosticText(row) == "无" {
		return "table-message table-message-neutral"
	}
	if !row.Config.Enabled {
		return "status-neutral"
	}
	return "table-message table-message-bad"
}

// eventLevelText 返回事件级别的中文名称。
func eventLevelText(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "错误"
	case "warning", "warn":
		return "告警"
	case "info":
		return "信息"
	default:
		return "事件"
	}
}

// eventLevelClass 返回当前状态对应的 CSS 样式类名。
func eventLevelClass(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "status-bad"
	case "warning", "warn":
		return "status-warn"
	case "info":
		return "status-ok"
	default:
		return "status-neutral"
	}
}

// eventSourceText 返回事件来源的中文名称。
func eventSourceText(source string) string {
	return model.EventSourceLabel(source)
}

// eventTypeText 返回事件类型的中文名称。
func eventTypeText(event model.ServiceEvent) string {
	return eventSourceText(event.Source)
}

// eventTargetText 返回事件关联对象的展示名称。
func eventTargetText(event model.ServiceEvent) string {
	if diagnosisHasIssue(event.Diagnosis) {
		if target := strings.TrimSpace(event.Diagnosis.TargetName); target != "" {
			return target
		}
		if target := strings.TrimSpace(event.Diagnosis.TargetID); target != "" {
			return target
		}
	}
	switch strings.ToLower(strings.TrimSpace(event.TargetID)) {
	case "data_maintenance":
		return "数据维护"
	case "time_adjustment":
		return "时间校准"
	case "network", "network_config":
		return "网络配置"
	default:
		return defaultText(event.TargetID)
	}
}

// eventSummaryText 返回本地化后的事件摘要。
func eventSummaryText(event model.ServiceEvent) string {
	summary := ""
	if diagnosisHasIssue(event.Diagnosis) && strings.TrimSpace(event.Diagnosis.Message) != "" {
		summary = userVisibleAlarmLimitText(strings.TrimSpace(event.Diagnosis.Message))
	} else {
		summary = userVisibleAlarmLimitText(userVisibleErrorSummaryText(event.Summary))
	}
	detail := eventSummaryDetailText(event)
	if detail == "" || strings.Contains(summary, detail) {
		return summary
	}
	return summary + "；" + detail
}

// eventSummaryDetailText 只把用户判断事件所需的短详情并入摘要，避免重新制造“详情列”。
func eventSummaryDetailText(event model.ServiceEvent) string {
	rawDetail := strings.TrimSpace(event.Detail)
	if rawDetail == "" {
		return ""
	}
	detail := userVisibleErrorDetailText(rawDetail)
	if detail == "操作失败" {
		for _, keyword := range []string{"当前值", "阈值", "高限", "低限", "上限", "下限", "超时", "无应答", "清理", "删除数量"} {
			if strings.Contains(rawDetail, keyword) && containsChinese(rawDetail) {
				detail = rawDetail
				break
			}
		}
	}
	if detail == "" || detail == "-" || detail == "无" || detail == "操作失败" || len([]rune(detail)) > 80 {
		return ""
	}
	detail = userVisibleAlarmLimitText(detail)

	source := strings.ToLower(strings.TrimSpace(event.Source))
	switch source {
	case "data_alarm", "data_maintenance", "time_adjustment", "network", "network_config", "config_apply":
		return detail
	}

	summary := strings.TrimSpace(event.Summary)
	for _, generic := range []string{"操作失败", "执行失败", "处理失败", "操作完成", "执行完成", "处理完成"} {
		if summary == generic {
			return detail
		}
	}
	for _, keyword := range []string{"当前值", "阈值", "高限", "低限", "上限", "下限", "超时", "无应答", "清理", "删除数量"} {
		if strings.Contains(detail, keyword) {
			return detail
		}
	}
	return ""
}

// userVisibleAlarmLimitText 清理报警文本中仅供内部使用的限制说明。
func userVisibleAlarmLimitText(text string) string {
	return strings.NewReplacer(
		"高限", "上限",
		"低限", "下限",
	).Replace(text)
}

// eventRepeatText 格式化事件重复次数。
func eventRepeatText(count uint64) string {
	if count <= 1 {
		return ""
	}
	return fmt.Sprintf("累计 %d 次", count)
}

// alarmLevelText 返回报警级别的中文名称。
func alarmLevelText(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "严重"
	default:
		return "告警"
	}
}

// alarmDirectionText 返回报警越限方向的中文名称。
func alarmDirectionText(direction string) string {
	switch strings.ToLower(strings.TrimSpace(direction)) {
	case "high":
		return "上限"
	case "low":
		return "下限"
	default:
		return "-"
	}
}

// formatAlarmValue 格式化告警值。
func formatAlarmValue(value float64, precision uint32, unit string) string {
	if precision > 12 {
		precision = 12
	}
	text := fmt.Sprintf("%.*f", int(precision), value)
	unit = strings.TrimSpace(unit)
	if unit == "" {
		return text
	}
	return text + " " + unit
}

// yesNoClass 返回当前状态对应的 CSS 样式类名。
func yesNoClass(value bool) string {
	if value {
		return "status-ok"
	}
	return "status-neutral"
}

// defaultText 生成默认文本。
func defaultText(value string) string {
	if value == "" {
		return "-"
	}
	return value
}
