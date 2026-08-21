package service

// 本文件收纳控制台各页面共享的规范化、默认值和集合辅助函数，避免不同页面产生不一致口径。

import (
	"edge-web/internal/model"
	"fmt"
	"strings"
)

// userVisibleAlarmLimitText 只规范化展示文案；后端字段、枚举和配置结构保持 high/low 原样。
func userVisibleAlarmLimitText(text string) string {
	return strings.NewReplacer(
		"高限", "上限",
		"低限", "下限",
	).Replace(text)
}

func normalizePositiveInt(value int, fallback int) int {
	if value <= 0 {
		return fallback
	}
	return value
}

func firstNonEmptyError(errs ...error) string {
	for _, err := range errs {
		if err != nil && strings.TrimSpace(err.Error()) != "" {
			return err.Error()
		}
	}
	return "数据加载失败"
}

func totalPages(total int, pageSize int) int {
	if total <= 0 {
		return 1
	}
	pages := total / pageSize
	if total%pageSize != 0 {
		pages++
	}
	return pages
}

// normalizedDeviceTemplates 复制并规范化 controller 返回的设备类型；空列表保持为空，
// 避免 Web 在配置缺失时注入另一套采集语义。
func normalizedDeviceTemplates(templates []model.DeviceTemplateDefinition) []model.DeviceTemplateDefinition {
	if len(templates) == 0 {
		return nil
	}
	normalized := make([]model.DeviceTemplateDefinition, len(templates))
	for index := range templates {
		normalized[index] = normalizeDeviceTemplateDefinition(templates[index])
	}
	return normalized
}

func isBackendUnavailable(err error) bool {
	if err == nil {
		return false
	}
	message := err.Error()
	return strings.Contains(message, "连接后端 IPC 失败") ||
		strings.Contains(message, "连接后端 IPC 已取消或超时") ||
		strings.Contains(message, "写入 IPC") ||
		strings.Contains(message, "写入后端 IPC 已取消或超时") ||
		strings.Contains(message, "读取 IPC") ||
		strings.Contains(message, "读取后端 IPC 已取消或超时") ||
		strings.Contains(message, "设置 IPC 超时失败") ||
		strings.Contains(message, "后端 IPC 请求等待超时") ||
		strings.Contains(message, "后端 IPC 请求等待已取消或超时")
}

func chooseString(primary string, fallback string) string {
	if primary != "" && primary != "-" {
		return primary
	}
	if fallback != "" {
		return fallback
	}
	return "-"
}

func diagnosisHasIssue(diagnosis model.DiagnosisStatus) bool {
	code := strings.TrimSpace(diagnosis.ErrorCode)
	if code == "" || strings.EqualFold(code, "NONE") {
		return false
	}
	status := strings.ToLower(strings.TrimSpace(diagnosis.Status))
	return status != "normal"
}

func diagnosisMessage(diagnosis model.DiagnosisStatus) string {
	if !diagnosisHasIssue(diagnosis) {
		return ""
	}
	return strings.TrimSpace(diagnosis.Message)
}

func defaultString(value string, fallback string) string {
	if value == "" {
		return fallback
	}
	return value
}

func maxUint64(a uint64, b uint64) uint64 {
	if a > b {
		return a
	}
	return b
}

func minInt(a int, b int) int {
	if a < b {
		return a
	}
	return b
}

func maxInt(a int, b int) int {
	if a > b {
		return a
	}
	return b
}

func partialError(name string, err error) string {
	if err == nil {
		return ""
	}
	return fmt.Sprintf("%s失败：%s", name, err.Error())
}

func joinPartialErrors(messages ...string) string {
	var result []string
	for _, message := range messages {
		if message != "" {
			result = append(result, message)
		}
	}
	return strings.Join(result, "；")
}

func loadPrimaryDataErrorMessage(warning string) string {
	if warning != "" {
		return warning
	}
	return "主数据加载失败"
}

func pollingStateText(running bool) string {
	if running {
		return "运行中"
	}
	return "已停止"
}
