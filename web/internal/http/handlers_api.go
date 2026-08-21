package httpserver

// 本文件提供页面使用的 JSON API，并将后端错误统一转换为稳定的 HTTP 状态和中文消息。

import (
	"log"
	"math"
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
	if message := validateDeviceTemplateReadModelRequest(&request); message != "" {
		writeError(w, http.StatusBadRequest, "invalid_request", message)
		return request, false
	}
	return request, true
}

type deviceTemplateReadRange struct {
	key          string
	functionCode int
	start        uint64
	end          uint64
}

type deviceTemplateFieldRange struct {
	start    uint64
	end      uint64
	parserID string
	bitIndex int
}

// deviceTemplateEnumRange 返回设备类型枚举字段允许的数值范围。
func deviceTemplateEnumRange(field model.DeviceTemplateField) (int64, int64, bool) {
	switch strings.ToLower(strings.TrimSpace(field.ParserID)) {
	case "bit_uint16":
		return 0, 1, true
	case "scaled_high_uint8", "scaled_low_uint8":
		return 0, 255, true
	}
	switch strings.ToLower(strings.TrimSpace(field.DataType)) {
	case "uint16":
		return 0, 65535, true
	case "int16":
		return -32768, 32767, true
	case "uint32":
		return 0, 4294967295, true
	case "int32":
		return -2147483648, 2147483647, true
	default:
		return 0, 0, false
	}
}

// isSafeDeviceTemplateIdentifier 判断设备类型标识是否仅含安全字符。
func isSafeDeviceTemplateIdentifier(value string) bool {
	if value == "" {
		return false
	}
	for _, character := range value {
		if (character >= 'a' && character <= 'z') ||
			(character >= '0' && character <= '9') || character == '_' || character == '-' {
			continue
		}
		return false
	}
	return true
}

// validateDeviceTemplateReadModelRequest 在 Web 边界校验页面提交的权威读取模型。
func validateDeviceTemplateReadModelRequest(request *model.DeviceTemplateDefinition) string {
	// 先校验设备级地址范围和必需的读取区块。
	if request.DeviceAddressStride <= 0 {
		return "设备地址跨度必须大于 0"
	}
	if request.DeviceAddressStride > 65536 {
		return "设备地址跨度不能超过 Modbus 地址空间 65536"
	}
	if len(request.ReadBlocks) == 0 {
		return "设备类型至少需要一个读取区块"
	}

	// 校验读取区块并建立索引，供后续采集点引用检查。
	blocksByKey := make(map[string]model.DeviceTemplateReadBlock, len(request.ReadBlocks))
	ranges := make([]deviceTemplateReadRange, 0, len(request.ReadBlocks))
	for index, block := range request.ReadBlocks {
		label := "读取区块[" + strconv.Itoa(index+1) + "]"
		if strings.TrimSpace(block.BlockKey) == "" {
			return label + " 标识不能为空"
		}
		if _, exists := blocksByKey[block.BlockKey]; exists {
			return "读取区块标识重复：" + block.BlockKey
		}
		if strings.TrimSpace(block.DisplayName) == "" {
			return label + " 显示名称不能为空"
		}
		if block.FunctionCode != 3 && block.FunctionCode != 4 {
			return label + " 功能码仅支持 Modbus FC03 或 FC04"
		}
		if block.StartOffset < 0 || block.StartOffset > 65535 {
			return label + " 起始偏移超出 Modbus 地址空间"
		}
		if block.RegisterCount < 1 || block.RegisterCount > 125 {
			return label + " 寄存器数量必须为 1～125"
		}
		if block.SortOrder < 0 {
			return label + " 顺序不能小于 0"
		}
		end := uint64(block.StartOffset) + uint64(block.RegisterCount)
		if end > 65536 {
			return label + " 超出 Modbus 地址空间"
		}
		if end > uint64(request.DeviceAddressStride) {
			return label + " 超出设备地址跨度"
		}
		blocksByKey[block.BlockKey] = block
		ranges = append(ranges, deviceTemplateReadRange{
			key: block.BlockKey, functionCode: block.FunctionCode,
			start: uint64(block.StartOffset), end: end,
		})
	}

	// 同一功能码下的读取区块不得出现地址重叠。
	for left := 0; left < len(ranges); left++ {
		for right := left + 1; right < len(ranges); right++ {
			first, second := ranges[left], ranges[right]
			if first.functionCode == second.functionCode &&
				first.start < second.end && second.start < first.end {
				return "相同功能码读取区块地址重叠：" + first.key + " 与 " + second.key
			}
		}
	}

	// 规范化实时展示分组，并检查标识和排序的唯一性。
	groupIDs := make(map[string]struct{}, len(request.RealtimeGroups))
	groupOrders := make(map[int]struct{}, len(request.RealtimeGroups))
	for index := range request.RealtimeGroups {
		group := &request.RealtimeGroups[index]
		group.ID = strings.TrimSpace(group.ID)
		group.Name = strings.TrimSpace(group.Name)
		label := "实时展示分组[" + strconv.Itoa(index+1) + "]"
		if !isSafeDeviceTemplateIdentifier(group.ID) {
			return label + " ID 只能使用小写字母、数字、下划线和中划线"
		}
		if _, exists := groupIDs[group.ID]; exists {
			return "实时展示分组 ID 重复：" + group.ID
		}
		groupIDs[group.ID] = struct{}{}
		if group.Name == "" {
			return label + "名称不能为空"
		}
		if len(group.Name) > 100 {
			return label + "名称不能超过 100 个字节"
		}
		if group.Order < 0 {
			return label + "顺序不能小于 0"
		}
		if _, exists := groupOrders[group.Order]; exists {
			return label + "顺序不能重复"
		}
		groupOrders[group.Order] = struct{}{}
	}
	if request.RealtimeGroupingEnabled && len(request.RealtimeGroups) == 0 {
		return "开启实时展示分组后至少需要一个分组"
	}

	// 校验采集点类型、枚举、区块引用及寄存器占用关系。
	fieldRangesByBlock := make(map[string][]deviceTemplateFieldRange)
	for index := range request.Fields {
		field := &request.Fields[index]
		label := "采集点[" + strconv.Itoa(index+1) + "]"
		field.DataType = strings.ToLower(strings.TrimSpace(field.DataType))
		field.ParserID = strings.ToLower(strings.TrimSpace(field.ParserID))
		field.RealtimeGroupID = strings.TrimSpace(field.RealtimeGroupID)
		if field.RealtimeGroupID != "" {
			if _, exists := groupIDs[field.RealtimeGroupID]; !exists {
				return label + " 引用的实时展示分组不存在：" + field.RealtimeGroupID
			}
		}
		if request.RealtimeGroupingEnabled && field.ShowInRealtime && field.RealtimeGroupID == "" {
			return label + " 已启用实时展示，必须选择实时展示分组"
		}
		isBit := field.DataType == "bool" || field.ParserID == "bit_uint16"
		if isBit {
			if field.DataType != "bool" || field.ParserID != "bit_uint16" {
				return label + " bool 字段必须使用 parser_id=bit_uint16"
			}
			if field.RegisterCount != 1 || field.BitIndex < 0 || field.BitIndex > 15 {
				return label + " bool 字段要求 register_count=1 且 bit_index 为 0～15"
			}
			if field.Scale != 1 || field.Offset != 0 || field.Precision != 0 {
				return label + " bool 字段要求 scale=1、offset=0、precision=0"
			}
		} else {
			if field.BitIndex != -1 {
				return label + " 非 bool 字段 bit_index 必须为 -1"
			}
		}
		if math.IsNaN(field.Scale) || math.IsInf(field.Scale, 0) ||
			math.IsNaN(field.Offset) || math.IsInf(field.Offset, 0) {
			return label + " 比例或偏移必须为有限数"
		}
		if len(field.EnumItems) > 32 {
			return label + " 单字段枚举项不能超过 32 条"
		}
		if len(field.EnumItems) > 0 {
			minimum, maximum, supported := deviceTemplateEnumRange(*field)
			if !supported {
				return label + " 当前字段类型不支持枚举显示"
			}
			if field.Scale != 1 || field.Offset != 0 || field.Precision != 0 {
				return label + " 枚举字段要求 scale=1、offset=0、precision=0"
			}
			values := make(map[int64]struct{}, len(field.EnumItems))
			for enumIndex := range field.EnumItems {
				item := &field.EnumItems[enumIndex]
				item.Label = strings.TrimSpace(item.Label)
				if item.Label == "" {
					return label + " 枚举显示文字不能为空"
				}
				if item.Value < minimum || item.Value > maximum {
					return label + " 枚举值超出字段数据类型范围"
				}
				if _, exists := values[item.Value]; exists {
					return label + " 枚举值不能重复"
				}
				values[item.Value] = struct{}{}
			}
		}
		block, exists := blocksByKey[field.ReadBlockKey]
		if !exists {
			key := field.ReadBlockKey
			if strings.TrimSpace(key) == "" {
				key = "<空>"
			}
			return "采集点[" + strconv.Itoa(index+1) + "] 引用的读取区块不存在：" + key
		}
		if field.RegisterCount == 0 ||
			uint64(field.RegisterOffset)+uint64(field.RegisterCount) > uint64(block.RegisterCount) {
			return label + " 超出所属读取区块范围"
		}
		start := uint64(field.RegisterOffset)
		end := start + uint64(field.RegisterCount)
		for _, existing := range fieldRangesByBlock[field.ReadBlockKey] {
			if start >= existing.end || existing.start >= end {
				continue
			}
			sameRegister := start == existing.start && end == existing.end && end == start+1
			distinctBits := sameRegister && field.ParserID == "bit_uint16" &&
				existing.parserID == "bit_uint16" && field.BitIndex != existing.bitIndex
			bytePair := sameRegister && ((field.ParserID == "scaled_high_uint8" && existing.parserID == "scaled_low_uint8") ||
				(field.ParserID == "scaled_low_uint8" && existing.parserID == "scaled_high_uint8"))
			if !distinctBits && !bytePair {
				return label + " 寄存器范围与同一区块内其他字段重叠"
			}
		}
		fieldRangesByBlock[field.ReadBlockKey] = append(fieldRangesByBlock[field.ReadBlockKey], deviceTemplateFieldRange{
			start: start, end: end, parserID: field.ParserID, bitIndex: field.BitIndex,
		})
	}

	return ""
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
