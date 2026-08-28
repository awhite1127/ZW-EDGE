package httpserver

// 本文件提供页面使用的 JSON API，并将后端错误统一转换为稳定的 HTTP 状态和中文消息。

import (
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"

	"edge-web/internal/model"
)

// handleGetSystemStatus 处理系统状态查询请求。
func (s *Server) handleGetSystemStatus(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetSystemStatus(ctx)
	writeResult(w, result, err)
}

// handleGetSystemOverviewSnapshot 处理系统概览快照查询请求。
func (s *Server) handleGetSystemOverviewSnapshot(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetSystemOverviewSnapshot(ctx)
	writeResult(w, result, err)
}

// handleGetPollingSummary 处理轮询摘要查询请求。
func (s *Server) handleGetPollingSummary(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetPollingSummary(ctx)
	writeResult(w, result, err)
}

// handleGetRecentError 处理最近错误查询请求。
func (s *Server) handleGetRecentError(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetRecentError(ctx)
	writeResult(w, result, err)
}

// handleDataMaintenanceSummary 处理数据维护摘要请求。
func (s *Server) handleDataMaintenanceSummary(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetDataMaintenanceSummary(ctx)
	writeResult(w, result, err)
}

// handleSettingsRuntimeStatus 处理设置运行态状态请求。
func (s *Server) handleSettingsRuntimeStatus(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result := s.console.LoadSettingsRuntimeStatus(ctx)
	writeResult(w, result, nil)
}

// handleListSerialPorts 处理串口端口列表查询请求。
func (s *Server) handleListSerialPorts(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.ListSerialPorts(ctx)
	writeResult(w, result, err)
}

// handleListChannels 处理通道列表查询请求。
func (s *Server) handleListChannels(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result := s.console.LoadChannels(ctx)
	if !result.PrimaryDataAvailable {
		writeError(w, http.StatusBadGateway, "backend_error", loadResultErrorMessage(result.WarningMessage))
		return
	}
	writeResult(w, result.Rows, nil)
}

// handleUpdateChannelConfig 处理通道配置更新请求。
func (s *Server) handleUpdateChannelConfig(w http.ResponseWriter, r *http.Request) {
	channelID := r.PathValue("id")
	if channelID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少通道 ID")
		return
	}

	var request model.ChannelConfigUpdateRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	// 路径参数是被更新对象的唯一来源，避免客户端请求体误带其他通道 ID。
	request.ChannelID = channelID

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.UpdateChannelConfig(ctx, request)
	writeResult(w, result, err)
}

// handleCreateChannelConfig 处理通道配置创建请求。
func (s *Server) handleCreateChannelConfig(w http.ResponseWriter, r *http.Request) {
	var request model.ChannelConfigUpdateRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.CreateChannelConfig(ctx, request)
	writeResult(w, result, err)
}

// handleDeleteChannelConfig 处理通道配置删除请求。
func (s *Server) handleDeleteChannelConfig(w http.ResponseWriter, r *http.Request) {
	channelID := r.PathValue("id")
	if channelID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少通道 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.DeleteChannelConfig(ctx, channelID)
	writeResult(w, result, err)
}

// communicationTraceLimitFromRequest 从 HTTP 请求中解析通讯报文数量上限。
func communicationTraceLimitFromRequest(r *http.Request) int {
	raw := r.URL.Query().Get("limit")
	if raw == "" {
		return 0
	}
	limit, err := strconv.Atoi(raw)
	if err != nil {
		return 0
	}
	return limit
}

// handleGetChannelCommunicationTraces 处理通道通讯报文查询请求。
func (s *Server) handleGetChannelCommunicationTraces(w http.ResponseWriter, r *http.Request) {
	channelID := strings.TrimSpace(r.PathValue("id"))
	if channelID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少通道 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.GetChannelCommunicationTraces(
		ctx,
		channelID,
		communicationTraceLimitFromRequest(r),
	)
	writeResult(w, result, err)
}

// handleClearChannelCommunicationTraces 处理通道通讯报文清空请求。
func (s *Server) handleClearChannelCommunicationTraces(w http.ResponseWriter, r *http.Request) {
	channelID := strings.TrimSpace(r.PathValue("id"))
	if channelID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少通道 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.ClearChannelCommunicationTraces(ctx, channelID)
	writeResult(w, result, err)
}

// handleListMasters 处理主站列表查询请求。
func (s *Server) handleListMasters(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result := s.console.LoadMasters(ctx)
	if !result.PrimaryDataAvailable {
		writeError(w, http.StatusBadGateway, "backend_error", loadResultErrorMessage(result.WarningMessage))
		return
	}
	writeResult(w, result.Rows, nil)
}

// handleCreateMasterConfig 处理主站配置创建请求。
func (s *Server) handleCreateMasterConfig(w http.ResponseWriter, r *http.Request) {
	var request model.MasterConfigUpdateRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.CreateMasterConfig(ctx, request)
	writeResult(w, result, err)
}

// handleUpdateMasterConfig 处理主站配置更新请求。
func (s *Server) handleUpdateMasterConfig(w http.ResponseWriter, r *http.Request) {
	masterID := r.PathValue("id")
	if masterID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少主站 ID")
		return
	}

	var request model.MasterConfigUpdateRequest
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}
	// 主控保存后会触发后端停采、写库、重建拓扑和按需恢复轮询，Web 侧只负责转发配置。
	request.MasterID = masterID

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.UpdateMasterConfig(ctx, request)
	writeResult(w, result, err)
}

// handleDeleteMasterConfig 处理主站配置删除请求。
func (s *Server) handleDeleteMasterConfig(w http.ResponseWriter, r *http.Request) {
	masterID := r.PathValue("id")
	if masterID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少主站 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.DeleteMasterConfig(ctx, masterID)
	writeResult(w, result, err)
}

// handleListDevices 处理设备列表查询请求。
func (s *Server) handleListDevices(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result := s.console.LoadDevices(ctx)
	if !result.PrimaryDataAvailable {
		writeError(w, http.StatusBadGateway, "backend_error", loadResultErrorMessage(result.WarningMessage))
		return
	}
	writeResult(w, result.Rows, nil)
}

// handleGetDeviceDetail 处理设备详情查询请求。
func (s *Server) handleGetDeviceDetail(w http.ResponseWriter, r *http.Request) {
	deviceID := strings.TrimSpace(r.PathValue("id"))
	if deviceID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.GetDeviceDetail(ctx, deviceID)
	writeResult(w, result, err)
}

// handleUpdateDeviceDisplayName 处理设备显示名称更新请求。
func (s *Server) handleUpdateDeviceDisplayName(w http.ResponseWriter, r *http.Request) {
	deviceID := strings.TrimSpace(r.PathValue("id"))
	if deviceID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备 ID")
		return
	}
	var body struct {
		DisplayName string `json:"display_name"`
	}
	if !decodeJSONRequest(w, r, &body, maxStandardJSONRequestBodyBytes) {
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateDeviceDisplayName(ctx, model.DeviceDisplayNameUpdateRequest{
		DeviceID: deviceID, DisplayName: body.DisplayName,
	})
	writeResult(w, result, err)
}

// handleUpdateDeviceDisplayNamesBatch 处理设备显示名称批量更新请求。
func (s *Server) handleUpdateDeviceDisplayNamesBatch(w http.ResponseWriter, r *http.Request) {
	var body model.DeviceDisplayNameBatchRequest
	if !decodeJSONRequest(w, r, &body, maxBatchJSONRequestBodyBytes) {
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateDeviceDisplayNamesBatch(ctx, body)
	writeResult(w, result, err)
}

// handleExecuteDeviceCommand 处理设备命令执行请求。
func (s *Server) handleExecuteDeviceCommand(w http.ResponseWriter, r *http.Request) {
	deviceID := strings.TrimSpace(r.PathValue("id"))
	commandKey := strings.TrimSpace(r.PathValue("command_key"))
	if deviceID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备 ID")
		return
	}
	if commandKey == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少命令 key")
		return
	}

	var request struct {
		Values map[string]uint16 `json:"values"`
	}
	if !decodeJSONRequest(w, r, &request, maxStandardJSONRequestBodyBytes) {
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.ExecuteDeviceCommand(ctx, model.DeviceCommandExecuteRequest{
		DeviceID:   deviceID,
		CommandKey: commandKey,
		Values:     request.Values,
	})
	writeResult(w, result, err)
}

// handleReadEM100EventRecord 处理 EM100 事件记录读取请求。
func (s *Server) handleReadEM100EventRecord(w http.ResponseWriter, r *http.Request) {
	deviceID := strings.TrimSpace(r.PathValue("id"))
	if deviceID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.ReadEM100EventRecord(ctx, deviceID)
	writeResult(w, result, err)
}

// handleReadEM100TestRecord 处理 EM100 测试记录读取请求。
func (s *Server) handleReadEM100TestRecord(w http.ResponseWriter, r *http.Request) {
	deviceID := strings.TrimSpace(r.PathValue("id"))
	if deviceID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备 ID")
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result, err := s.console.ReadEM100TestRecord(ctx, deviceID)
	writeResult(w, result, err)
}

// handleListDeviceRealtime 处理设备实时数据列表查询请求。
func (s *Server) handleListDeviceRealtime(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result := s.console.LoadRealtime(ctx)
	if !result.PrimaryDataAvailable {
		writeError(w, http.StatusBadGateway, "backend_error", loadResultErrorMessage(result.WarningMessage))
		return
	}
	writeResult(w, result.Rows, nil)
}

// handleGetConfigSummary 处理配置摘要查询请求。
func (s *Server) handleGetConfigSummary(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.GetConfigSummary(ctx)
	writeResult(w, result, err)
}

// decodeDeviceTemplateRequest 解码并校验设备类型请求。
func decodeDeviceTemplateRequest(w http.ResponseWriter, r *http.Request) (model.DeviceTemplateDefinition, bool) {
	var request model.DeviceTemplateDefinition
	if !decodeJSONRequest(w, r, &request, maxDeviceTemplateBodyBytes) {
		return request, false
	}
	request.ID = strings.TrimSpace(request.ID)
	request.DisplayName = strings.TrimSpace(request.DisplayName)
	request.Description = strings.TrimSpace(request.Description)
	request.Builtin = false
	if len(request.WriteCommands) != 0 {
		writeError(w, http.StatusBadRequest, "invalid_request", "自定义设备类型仅支持 FC03 / FC04 数据采集，不能配置控制操作")
		return request, false
	}
	request.WriteCommands = []model.DeviceTemplateWriteCommand{}
	if request.ID == "" || request.DisplayName == "" || len(request.Fields) == 0 {
		writeError(w, http.StatusBadRequest, "invalid_request", "模板 ID、设备类型名称和数据项不能为空")
		return request, false
	}
	// 完整的 Modbus 地址、字段类型、枚举和重叠规则由 C++ 后端统一校验。
	// Web 边界只保留页面输入的规范化，避免两套校验规则长期漂移。
	for index := range request.RealtimeGroups {
		request.RealtimeGroups[index].ID = strings.TrimSpace(request.RealtimeGroups[index].ID)
		request.RealtimeGroups[index].Name = strings.TrimSpace(request.RealtimeGroups[index].Name)
	}
	for index := range request.Fields {
		field := &request.Fields[index]
		field.DataType = strings.ToLower(strings.TrimSpace(field.DataType))
		field.ParserID = strings.ToLower(strings.TrimSpace(field.ParserID))
		field.RealtimeGroupID = strings.TrimSpace(field.RealtimeGroupID)
		for enumIndex := range field.EnumItems {
			field.EnumItems[enumIndex].Label = strings.TrimSpace(field.EnumItems[enumIndex].Label)
		}
	}
	return request, true
}

// handleCreateDeviceTemplate 处理设备类型创建请求。
func (s *Server) handleCreateDeviceTemplate(w http.ResponseWriter, r *http.Request) {
	request, ok := decodeDeviceTemplateRequest(w, r)
	if !ok {
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.CreateDeviceTemplate(ctx, request)
	if err != nil {
		log.Printf("自定义设备类型创建失败：template_id=%q error=%v", request.ID, err)
	}
	writeResult(w, result, err)
}

// handleUpdateDeviceTemplate 处理设备类型更新请求。
func (s *Server) handleUpdateDeviceTemplate(w http.ResponseWriter, r *http.Request) {
	templateID := strings.TrimSpace(r.PathValue("id"))
	request, ok := decodeDeviceTemplateRequest(w, r)
	if !ok {
		return
	}
	if templateID == "" || request.ID != templateID {
		writeError(w, http.StatusBadRequest, "invalid_request", "路径中的设备类型 ID 与请求体不一致")
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateDeviceTemplate(ctx, request)
	if err != nil {
		log.Printf("自定义设备类型更新失败：template_id=%q error=%v", request.ID, err)
	}
	writeResult(w, result, err)
}

// handleDeleteDeviceTemplate 处理设备类型删除请求。
func (s *Server) handleDeleteDeviceTemplate(w http.ResponseWriter, r *http.Request) {
	templateID := strings.TrimSpace(r.PathValue("id"))
	if templateID == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备类型 ID")
		return
	}
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.DeleteDeviceTemplate(ctx, templateID)
	if err != nil {
		log.Printf("自定义设备类型删除失败：template_id=%q error=%v", templateID, err)
	}
	writeResult(w, result, err)
}

// handleUpdateDeviceTemplateRealtimeDisplay 处理设备类型字段的实时显示设置请求。
func (s *Server) handleUpdateDeviceTemplateRealtimeDisplay(w http.ResponseWriter, r *http.Request) {
	request := model.DeviceTemplateRealtimeDisplayRequest{
		TemplateID: strings.TrimSpace(r.PathValue("id")),
		FieldKey:   strings.TrimSpace(r.PathValue("field_key")),
	}
	if request.TemplateID == "" || request.FieldKey == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备类型或数据项标识")
		return
	}
	var body struct {
		ShowInRealtime bool `json:"show_in_realtime"`
	}
	if !decodeJSONRequest(w, r, &body, maxStandardJSONRequestBodyBytes) {
		return
	}
	request.ShowInRealtime = body.ShowInRealtime
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateDeviceTemplateRealtimeDisplay(ctx, request)
	writeResult(w, result, err)
}

// handleUpdateDeviceTemplateHistoryEnabled 处理设备类型字段的历史记录设置请求。
func (s *Server) handleUpdateDeviceTemplateHistoryEnabled(w http.ResponseWriter, r *http.Request) {
	request := model.DeviceTemplateHistoryEnabledRequest{
		TemplateID: strings.TrimSpace(r.PathValue("id")),
		FieldKey:   strings.TrimSpace(r.PathValue("field_key")),
	}
	if request.TemplateID == "" || request.FieldKey == "" {
		writeError(w, http.StatusBadRequest, "invalid_request", "缺少设备类型或数据项标识")
		return
	}
	var body struct {
		HistoryEnabled bool `json:"history_enabled"`
	}
	if !decodeJSONRequest(w, r, &body, maxStandardJSONRequestBodyBytes) {
		return
	}
	request.HistoryEnabled = body.HistoryEnabled
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()
	result, err := s.console.UpdateDeviceTemplateHistoryEnabled(ctx, request)
	writeResult(w, result, err)
}

// handleRealtimeView 处理实时数据视图请求。
func (s *Server) handleRealtimeView(w http.ResponseWriter, r *http.Request) {
	started := time.Now()
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	response, err := s.console.BuildRealtimeResponse(ctx)
	logSlowRealtimeViewBuild(time.Since(started), err)
	writeResult(w, response, err)
}

// handleOverviewDiagnosisView 处理概览诊断视图请求。
func (s *Server) handleOverviewDiagnosisView(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	response := s.console.LoadOverviewDiagnosis(ctx)
	writeResult(w, response, nil)
}

// handleStartPolling 处理轮询启动请求。
func (s *Server) handleStartPolling(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	feedback := s.console.StartPolling(ctx)
	if !feedback.Success {
		writeError(w, http.StatusBadGateway, "backend_error", feedback.Message)
		return
	}
	writeResult(w, feedback, nil)
}

// handleStopPolling 处理轮询停止请求。
func (s *Server) handleStopPolling(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	feedback := s.console.StopPolling(ctx)
	if !feedback.Success {
		writeError(w, http.StatusBadGateway, "backend_error", feedback.Message)
		return
	}
	writeResult(w, feedback, nil)
}
