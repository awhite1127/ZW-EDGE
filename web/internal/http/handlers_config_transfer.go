package httpserver

// 本文件处理配置包导入导出，限制上传体积并保持版本、字段校验和敏感信息边界。

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"path/filepath"
	"strings"
	"time"

	"edge-web/internal/model"
)

const maxConfigImportBytes int64 = 2 << 20

func (s *Server) handleSettingsConfigExport(w http.ResponseWriter, r *http.Request) {
	if s.console == nil {
		http.Error(w, "配置导出服务尚未初始化", http.StatusInternalServerError)
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	bundle, err := s.console.ExportSystemConfig(ctx)
	if err != nil {
		log.Printf("系统配置导出失败: %v", err)
		http.Error(w, "系统配置导出失败："+err.Error(), http.StatusInternalServerError)
		return
	}
	normalizeConfigExportBundleForJSON(&bundle)

	content, err := json.MarshalIndent(bundle, "", "  ")
	if err != nil {
		log.Printf("系统配置 JSON 编码失败: %v", err)
		http.Error(w, "系统配置 JSON 编码失败", http.StatusInternalServerError)
		return
	}

	setJSONDownloadHeaders(w, configExportFilename(time.Now()))
	_, _ = w.Write(content)
}

// version 4 要求这些集合字段即使没有数据也以 JSON 空数组出现，不能写成 null 或被省略。
// 这保证 Web 下载后的文件能够不经修改直接通过同版本导入校验。
func normalizeConfigExportBundleForJSON(bundle *model.ConfigExportBundle) {
	if bundle == nil {
		return
	}
	if bundle.CustomDeviceTypes == nil {
		bundle.CustomDeviceTypes = []model.DeviceTemplateDefinition{}
	}
	for index := range bundle.CustomDeviceTypes {
		bundle.CustomDeviceTypes[index].WriteCommands = []model.DeviceTemplateWriteCommand{}
	}
	if bundle.Channels == nil {
		bundle.Channels = []model.ChannelConfig{}
	}
	if bundle.Masters == nil {
		bundle.Masters = []model.MasterNodeConfig{}
	}
	if bundle.AlarmRules == nil {
		bundle.AlarmRules = []model.AlarmRule{}
	}
	if bundle.ModbusRegisterMappings == nil {
		bundle.ModbusRegisterMappings = []model.ModbusRegisterMapping{}
	}
}

func (s *Server) handleSettingsConfigImport(w http.ResponseWriter, r *http.Request) {
	if s.console == nil {
		s.respondConfigImportError(w, r, http.StatusServiceUnavailable, "config_import_unavailable", "配置导入服务尚未初始化")
		return
	}

	// 限制请求体和表单大小，避免上传内容占用过多内存。
	r.Body = http.MaxBytesReader(w, r.Body, maxConfigImportBytes+64*1024)
	if err := r.ParseMultipartForm(maxConfigImportBytes); err != nil {
		var maxBytesError *http.MaxBytesError
		if errors.As(err, &maxBytesError) {
			s.respondConfigImportError(w, r, http.StatusRequestEntityTooLarge, "config_file_too_large", "配置文件不能超过 2 MB")
			return
		}
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_upload_invalid", "配置文件上传请求格式不正确或文件过大")
		return
	}

	// 获取上传文件并校验扩展名、声明大小和实际内容大小。
	file, header, err := r.FormFile("config_file")
	if err != nil {
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_file_required", "请选择要导入的 JSON 配置文件")
		return
	}
	defer file.Close()

	if header != nil && header.Size > maxConfigImportBytes {
		s.respondConfigImportError(w, r, http.StatusRequestEntityTooLarge, "config_file_too_large", "配置文件不能超过 2 MB")
		return
	}
	if header == nil || !strings.EqualFold(filepath.Ext(header.Filename), ".json") {
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_file_type_invalid", "仅支持 JSON 配置文件")
		return
	}

	content, err := io.ReadAll(io.LimitReader(file, maxConfigImportBytes+1))
	if err != nil {
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_file_read_failed", "读取配置文件失败")
		return
	}
	if int64(len(content)) > maxConfigImportBytes {
		s.respondConfigImportError(w, r, http.StatusRequestEntityTooLarge, "config_file_too_large", "配置文件不能超过 2 MB")
		return
	}
	if len(strings.TrimSpace(string(content))) == 0 {
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_file_empty", "配置文件不能为空")
		return
	}

	// 严格解析单个 JSON 对象，并核对导出格式和版本。
	var bundle model.ConfigExportBundle
	decoder := json.NewDecoder(bytes.NewReader(content))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&bundle); err != nil {
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_json_invalid", "配置文件 JSON 格式不正确")
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		s.respondConfigImportError(w, r, http.StatusBadRequest, "config_json_invalid", "配置文件只能包含一个 JSON 对象")
		return
	}
	if bundle.Format != "edge-controller-config" {
		s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_format_mismatch", "配置文件格式不匹配")
		return
	}
	if bundle.Version != 4 {
		s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_version_unsupported", "配置文件版本不受支持，仅支持当前 version 4")
		return
	}
	// 检查当前版本必需字段，拒绝旧版或不完整的配置包。
	if bundle.TimeSettings == nil {
		s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_content_invalid", "配置文件必须包含 time_settings")
		return
	}
	if message := validateCurrentConfigRequiredFields(content); message != "" {
		s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_content_invalid", message)
		return
	}
	{
		var root map[string]json.RawMessage
		_ = json.Unmarshal(content, &root)
		if raw, ok := root["custom_device_types"]; !ok || string(raw) == "null" {
			s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_content_invalid", "version 4 配置文件必须包含 custom_device_types 数组")
			return
		}
		if _, ok := root["modbus_server_settings"]; !ok {
			s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_content_invalid", "version 4 配置文件必须包含 modbus_server_settings")
			return
		}
		if raw, ok := root["modbus_register_mappings"]; !ok || string(raw) == "null" {
			s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_content_invalid", "version 4 配置文件必须包含 modbus_register_mappings 数组")
			return
		}
	}

	// 通过服务层执行导入，并按客户端类型返回 JSON 或页面反馈。
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	feedback := s.console.ImportSystemConfig(ctx, bundle)
	if !feedback.Success {
		s.respondConfigImportError(w, r, http.StatusUnprocessableEntity, "config_import_failed", feedback.Message)
		return
	}
	s.respondConfigImportSuccess(w, r, feedback.Message)
}

func (s *Server) respondConfigImportError(w http.ResponseWriter, r *http.Request, status int, code string, message string) {
	if wantsJSON(r) {
		writeError(w, status, code, message)
		return
	}
	s.redirectWithFeedback(w, r, model.ActionFeedback{Success: false, Message: message})
}

func (s *Server) respondConfigImportSuccess(w http.ResponseWriter, r *http.Request, message string) {
	if message == "" {
		message = "系统配置导入成功"
	}
	if wantsJSON(r) {
		writeSuccess(w, map[string]interface{}{
			"message":         message,
			"reload_required": true,
		})
		return
	}
	s.redirectWithFeedback(w, r, model.ActionFeedback{Success: true, Message: message})
}

func validateCurrentConfigRequiredFields(content []byte) string {
	var root map[string]json.RawMessage
	if err := json.Unmarshal(content, &root); err != nil {
		return "配置文件 JSON 格式不正确"
	}

	requiredObjectFields := func(name string, fields []string) string {
		raw, ok := root[name]
		if !ok {
			return "version 4 配置文件必须包含 " + name
		}
		var object map[string]json.RawMessage
		if err := json.Unmarshal(raw, &object); err != nil || object == nil {
			return "version 4 配置文件中 " + name + " 必须是对象"
		}
		for _, field := range fields {
			if _, ok := object[field]; !ok {
				return "version 4 配置文件缺少必填字段 " + name + "." + field
			}
		}
		return ""
	}

	if message := requiredObjectFields("time_settings", []string{
		"timezone", "ntp_enabled", "ntp_primary", "ntp_secondary", "updated_at",
	}); message != "" {
		return message
	}
	if message := requiredObjectFields("network_settings", []string{"mode"}); message != "" {
		return message
	}
	if message := requiredObjectFields("mqtt_settings", []string{
		"tls_enabled", "tls_ca_file", "tls_client_cert_file", "tls_client_key_file", "tls_insecure",
	}); message != "" {
		return message
	}

	rawChannels, ok := root["channels"]
	if !ok {
		return "version 4 配置文件必须包含 channels"
	}
	var channels []map[string]json.RawMessage
	if err := json.Unmarshal(rawChannels, &channels); err != nil || channels == nil {
		return "version 4 配置文件中 channels 必须是数组"
	}
	channelFields := []string{
		"channel_id", "channel_name", "enabled", "channel_type", "device_path", "port_name",
		"tcp_host", "tcp_port", "connect_timeout_ms", "baud_rate", "data_bits", "parity",
		"stop_bits", "response_timeout_ms", "retry_count",
	}
	for index, channel := range channels {
		for _, field := range channelFields {
			if _, ok := channel[field]; !ok {
				return fmt.Sprintf("version 4 配置文件缺少必填字段 channels[%d].%s", index, field)
			}
		}
	}
	rawMasters, ok := root["masters"]
	if !ok {
		return "version 4 配置文件必须包含 masters"
	}
	var masters []map[string]json.RawMessage
	if err := json.Unmarshal(rawMasters, &masters); err != nil || masters == nil {
		return "version 4 配置文件中 masters 必须是数组"
	}
	for index, master := range masters {
		if _, ok := master["device_count"]; !ok {
			return fmt.Sprintf("version 4 配置文件缺少必填字段 masters[%d].device_count", index)
		}
	}
	return ""
}

func setJSONDownloadHeaders(w http.ResponseWriter, filename string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set(
		"Content-Disposition",
		fmt.Sprintf("attachment; filename=%q; filename*=UTF-8''%s", filename, url.PathEscape(filename)),
	)
}

func configExportFilename(now time.Time) string {
	return "edge-controller-config-" + now.Format("20060102-150405") + ".json"
}
