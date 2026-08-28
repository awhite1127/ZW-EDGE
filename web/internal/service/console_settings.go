package service

// 本文件负责系统、网络、时间、MQTT 和模板设置页面；各能力独立取数和降级，
// 避免单个运行态接口不可用时阻断整个设置页面。

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"time"

	"edge-web/internal/model"
)

func (s *ConsoleService) GetConfigSummary(ctx context.Context) (model.ConfigSummary, error) {
	summary, err := s.backend.GetConfigSummary(ctx)
	for index := range summary.DeviceTemplates {
		summary.DeviceTemplates[index] = normalizeDeviceTemplateDefinition(summary.DeviceTemplates[index])
	}
	return summary, err
}

func (s *ConsoleService) GetEditableDeviceTemplate(ctx context.Context, templateID string) (model.DeviceTemplateDefinition, bool, error) {
	view, err := s.backend.GetDeviceTemplateManagement(ctx)
	if err != nil {
		return model.DeviceTemplateDefinition{}, false, err
	}
	view = displayDeviceTemplateManagement(view)
	definition, found := editableDeviceTemplate(view, templateID)
	return definition, found, nil
}

func editableDeviceTemplate(view model.DeviceTemplateManagementView, templateID string) (model.DeviceTemplateDefinition, bool) {
	for _, item := range view.Templates {
		if item.TemplateID == strings.TrimSpace(templateID) && !item.Builtin && item.Editable {
			return item.EditorDefinition, true
		}
	}
	return model.DeviceTemplateDefinition{}, false
}

func (s *ConsoleService) UpdateDeviceTemplateRealtimeDisplay(
	ctx context.Context,
	request model.DeviceTemplateRealtimeDisplayRequest,
) (model.DeviceTemplateRealtimeDisplayResult, error) {
	return s.backend.UpdateDeviceTemplateRealtimeDisplay(ctx, request)
}

func (s *ConsoleService) UpdateDeviceTemplateHistoryEnabled(
	ctx context.Context,
	request model.DeviceTemplateHistoryEnabledRequest,
) (model.DeviceTemplateHistoryEnabledResult, error) {
	return s.backend.UpdateDeviceTemplateHistoryEnabled(ctx, request)
}

func (s *ConsoleService) CreateDeviceTemplate(ctx context.Context, request model.DeviceTemplateDefinition) (model.DeviceTemplateMutationResult, error) {
	request.WriteCommands = []model.DeviceTemplateWriteCommand{}
	result, err := s.backend.CreateDeviceTemplate(ctx, normalizeDeviceTemplateDefinition(request))
	result.TemplateManagement = normalizeDeviceTemplateManagement(result.TemplateManagement)
	return result, err
}

func (s *ConsoleService) UpdateDeviceTemplate(ctx context.Context, request model.DeviceTemplateDefinition) (model.DeviceTemplateMutationResult, error) {
	request.WriteCommands = []model.DeviceTemplateWriteCommand{}
	result, err := s.backend.UpdateDeviceTemplate(ctx, normalizeDeviceTemplateDefinition(request))
	result.TemplateManagement = normalizeDeviceTemplateManagement(result.TemplateManagement)
	return result, err
}

func (s *ConsoleService) DeleteDeviceTemplate(ctx context.Context, templateID string) (model.DeviceTemplateMutationResult, error) {
	return s.backend.DeleteDeviceTemplate(ctx, templateID)
}

func (s *ConsoleService) GetSystemSettings(ctx context.Context) (model.SystemSettings, error) {
	return s.backend.GetSystemSettings(ctx)
}

const systemDisplayNameCacheTTL = 5 * time.Second

// GetSystemDisplayName 缓存页面公共标题，完整设置查询始终保持实时读取。
// generation 防止并发中的旧 IPC 响应覆盖刚刚写入或失效的新值。
func (s *ConsoleService) GetSystemDisplayName(ctx context.Context) (string, error) {
	now := time.Now()
	s.systemDisplayNameMu.Lock()
	if now.Before(s.systemDisplayNameExpiresAt) {
		value := s.systemDisplayName
		s.systemDisplayNameMu.Unlock()
		return value, nil
	}
	generation := s.systemDisplayNameGeneration
	s.systemDisplayNameMu.Unlock()

	settings, err := s.backend.GetSystemSettings(ctx)
	if err != nil {
		return "", err
	}
	value := strings.TrimSpace(settings.DisplayName)

	s.systemDisplayNameMu.Lock()
	if generation == s.systemDisplayNameGeneration {
		s.systemDisplayName = value
		s.systemDisplayNameExpiresAt = time.Now().Add(systemDisplayNameCacheTTL)
	} else {
		// 设置在本次 IPC 期间已经更新或失效，旧响应既不回写也不返回。
		value = s.systemDisplayName
	}
	s.systemDisplayNameMu.Unlock()
	return value, nil
}

func (s *ConsoleService) cacheSystemDisplayName(value string) {
	s.systemDisplayNameMu.Lock()
	defer s.systemDisplayNameMu.Unlock()
	s.systemDisplayNameGeneration++
	s.systemDisplayName = strings.TrimSpace(value)
	s.systemDisplayNameExpiresAt = time.Now().Add(systemDisplayNameCacheTTL)
}

func (s *ConsoleService) invalidateSystemDisplayName() {
	s.systemDisplayNameMu.Lock()
	defer s.systemDisplayNameMu.Unlock()
	s.systemDisplayNameGeneration++
	s.systemDisplayName = ""
	s.systemDisplayNameExpiresAt = time.Time{}
}

const settingsTemplatePageSize = 8

// loadDeviceTemplateSettingsSources 只读取设备类型子页面实际需要的系统标题和模板清单。
func (s *ConsoleService) loadDeviceTemplateSettingsSources(ctx context.Context) (
	model.SystemSettings,
	model.DeviceTemplateManagementView,
	error,
	error,
) {
	var wg sync.WaitGroup
	settings := startLoad(ctx, &wg, s.backend.GetSystemSettings)
	templates := startLoad(ctx, &wg, s.backend.GetDeviceTemplateManagement)
	wg.Wait()
	return settings.value, displayDeviceTemplateManagement(templates.value), settings.err, templates.err
}

func buildDeviceTemplateSettingsPage(
	settings model.SystemSettings,
	templateManagement model.DeviceTemplateManagementView,
	settingsErr error,
	templateErr error,
	requestedTemplatePage int,
) model.SettingsPageData {
	pagedTemplates, templatePagination := paginateDeviceTemplateManagement(templateManagement, requestedTemplatePage)
	pageData := model.SettingsPageData{
		BasePageData:             model.BasePageData{BackendReachable: true},
		Settings:                 settings,
		SettingsState:            model.SectionState{Available: true},
		DeviceTemplateManagement: pagedTemplates,
		DeviceTemplateState:      model.SectionState{Available: true},
		DeviceTemplatePagination: templatePagination,
		SettingsReturnPath:       fmt.Sprintf("/settings/device-types?template_page=%d", templatePagination.Page),
	}
	if settingsErr != nil {
		pageData.Settings = defaultSystemSettings()
		pageData.SettingsState = model.SectionState{ErrorMessage: settingsErr.Error()}
	}
	if templateErr != nil {
		pageData.DeviceTemplateState = model.SectionState{ErrorMessage: templateErr.Error()}
	}
	if settingsErr != nil && templateErr != nil {
		pageData.BasePageData.BackendReachable = false
		pageData.BasePageData.ErrorMessage = "后端不可达，设备类型暂时无法获取"
	}
	return pageData
}

// LoadDeviceTemplateSettings 为设备类型管理页执行最小查询计划。
func (s *ConsoleService) LoadDeviceTemplateSettings(ctx context.Context, requestedTemplatePage int) model.SettingsPageData {
	settings, templates, settingsErr, templateErr := s.loadDeviceTemplateSettingsSources(ctx)
	return buildDeviceTemplateSettingsPage(settings, templates, settingsErr, templateErr, requestedTemplatePage)
}

// LoadDeviceTemplateEditorSettings 复用同一次模板查询完成返回页分页和编辑目标查找。
func (s *ConsoleService) LoadDeviceTemplateEditorSettings(
	ctx context.Context,
	requestedTemplatePage int,
	templateID string,
) (model.SettingsPageData, model.DeviceTemplateDefinition, bool, error) {
	settings, templates, settingsErr, templateErr := s.loadDeviceTemplateSettingsSources(ctx)
	pageData := buildDeviceTemplateSettingsPage(settings, templates, settingsErr, templateErr, requestedTemplatePage)
	if strings.TrimSpace(templateID) == "" {
		return pageData, model.DeviceTemplateDefinition{}, false, nil
	}
	if templateErr != nil {
		return pageData, model.DeviceTemplateDefinition{}, false, templateErr
	}
	definition, found := editableDeviceTemplate(templates, templateID)
	return pageData, definition, found, nil
}

// LoadMqttSettingsPage 只读取 MQTT 子页面渲染和运行态刷新需要的数据。
func (s *ConsoleService) LoadMqttSettingsPage(ctx context.Context) model.SettingsPageData {
	var wg sync.WaitGroup
	settings := startLoad(ctx, &wg, s.backend.GetSystemSettings)
	mqtt := startLoad(ctx, &wg, s.backend.GetMqttSettings)
	mqttRuntime := startLoad(ctx, &wg, s.backend.GetMqttRuntimeStatus)
	wg.Wait()

	pageData := model.SettingsPageData{
		BasePageData:       model.BasePageData{BackendReachable: true},
		Settings:           settings.value,
		SettingsState:      model.SectionState{Available: true},
		Mqtt:               mqtt.value,
		MqttState:          model.SectionState{Available: true},
		MqttRuntime:        mqttRuntime.value,
		MqttRuntimeState:   model.SectionState{Available: true},
		SettingsReturnPath: "/settings/mqtt",
	}
	if settings.err != nil {
		pageData.Settings = defaultSystemSettings()
		pageData.SettingsState = model.SectionState{ErrorMessage: settings.err.Error()}
	}
	if mqtt.err != nil {
		pageData.Mqtt = defaultMqttSettings()
		pageData.MqttState = model.SectionState{ErrorMessage: mqtt.err.Error()}
	}
	if mqttRuntime.err != nil {
		pageData.MqttRuntime = defaultMqttRuntimeStatus(pageData.Mqtt.Enabled)
		pageData.MqttRuntimeState = model.SectionState{ErrorMessage: mqttRuntime.err.Error()}
	}
	if settings.err != nil && mqtt.err != nil && mqttRuntime.err != nil {
		pageData.BasePageData.BackendReachable = false
		pageData.BasePageData.ErrorMessage = "后端不可达，MQTT 设置暂时无法获取"
	}
	return pageData
}

func (s *ConsoleService) LoadSettings(ctx context.Context) model.SettingsPageData {
	// 设置页将多个可选能力放在同一屏，任意一块失败时使用默认值降级渲染，避免整页不可用。
	var wg sync.WaitGroup
	settings := startLoad(ctx, &wg, s.backend.GetSystemSettings)
	timeSettings := startLoad(ctx, &wg, s.backend.GetTimeSettings)
	timeRuntime := startLoad(ctx, &wg, s.backend.GetTimeRuntimeStatus)
	network := startLoad(ctx, &wg, s.backend.GetNetworkSettings)
	networkRuntime := startLoad(ctx, &wg, s.backend.GetNetworkRuntimeStatus)
	mqtt := startLoad(ctx, &wg, s.backend.GetMqttSettings)
	mqttRuntime := startLoad(ctx, &wg, s.backend.GetMqttRuntimeStatus)
	// 首页只展示模板总数，不再为不可见的明细做深拷贝、格式化和分页。
	templates := startLoad(ctx, &wg, s.backend.GetDeviceTemplateManagement)
	wg.Wait()
	templateSummary := templates.value
	templateSummary.Templates = nil

	pageData := model.SettingsPageData{
		BasePageData:             model.BasePageData{BackendReachable: true},
		Settings:                 settings.value,
		SettingsState:            model.SectionState{Available: true},
		TimeSettings:             timeSettings.value,
		TimeSettingsState:        model.SectionState{Available: true},
		TimeRuntime:              timeRuntime.value,
		TimeRuntimeState:         model.SectionState{Available: true},
		Network:                  network.value,
		NetworkState:             model.SectionState{Available: true},
		NetworkRuntime:           networkRuntime.value,
		NetworkRuntimeState:      model.SectionState{Available: true},
		NetworkDNSInput:          strings.Join(network.value.DNSServers, ","),
		Mqtt:                     mqtt.value,
		MqttState:                model.SectionState{Available: true},
		MqttRuntime:              mqttRuntime.value,
		MqttRuntimeState:         model.SectionState{Available: true},
		DeviceTemplateManagement: templateSummary,
		DeviceTemplateState:      model.SectionState{Available: true},
		SettingsReturnPath:       "/settings",
	}
	if settings.err != nil {
		pageData.Settings = defaultSystemSettings()
		pageData.SettingsState = model.SectionState{ErrorMessage: settings.err.Error()}
	}
	if timeSettings.err != nil {
		pageData.TimeSettings = defaultTimeSettings()
		pageData.TimeSettingsState = model.SectionState{ErrorMessage: timeSettings.err.Error()}
	}
	if timeRuntime.err != nil {
		pageData.TimeRuntime = defaultTimeRuntimeStatus(pageData.TimeSettings)
		pageData.TimeRuntimeState = model.SectionState{ErrorMessage: timeRuntime.err.Error()}
	}
	if network.err != nil {
		pageData.Network = defaultNetworkSettings()
		pageData.NetworkState = model.SectionState{ErrorMessage: network.err.Error()}
		pageData.NetworkDNSInput = strings.Join(pageData.Network.DNSServers, ",")
	}
	if networkRuntime.err != nil {
		pageData.NetworkRuntime = defaultNetworkRuntimeStatus(pageData.Network.InterfaceName)
		pageData.NetworkRuntimeState = model.SectionState{ErrorMessage: networkRuntime.err.Error()}
	}
	if mqtt.err != nil {
		pageData.Mqtt = defaultMqttSettings()
		pageData.MqttState = model.SectionState{ErrorMessage: mqtt.err.Error()}
	}
	if mqttRuntime.err != nil {
		pageData.MqttRuntime = defaultMqttRuntimeStatus(pageData.Mqtt.Enabled)
		pageData.MqttRuntimeState = model.SectionState{ErrorMessage: mqttRuntime.err.Error()}
	}
	if templates.err != nil {
		pageData.DeviceTemplateState = model.SectionState{ErrorMessage: templates.err.Error()}
	}
	if settings.err != nil && timeSettings.err != nil && network.err != nil &&
		mqtt.err != nil && templates.err != nil {
		pageData.BasePageData.BackendReachable = false
		pageData.BasePageData.ErrorMessage = "后端不可达，系统设置暂时无法获取"
	}
	return pageData
}

func (s *ConsoleService) LoadSettingsRuntimeStatus(ctx context.Context) model.SettingsRuntimeStatusResponse {
	var wg sync.WaitGroup
	networkRuntime := startLoad(ctx, &wg, s.backend.GetNetworkRuntimeStatus)
	mqttRuntime := startLoad(ctx, &wg, s.backend.GetMqttRuntimeStatus)
	timeRuntime := startLoad(ctx, &wg, s.backend.GetTimeRuntimeStatus)
	wg.Wait()
	result := model.SettingsRuntimeStatusResponse{
		BackendReachable:        true,
		NetworkRuntime:          networkRuntime.value,
		NetworkRuntimeAvailable: true,
		MqttRuntime:             mqttRuntime.value,
		MqttRuntimeAvailable:    true,
		TimeRuntime:             timeRuntime.value,
		TimeRuntimeAvailable:    true,
	}
	if networkRuntime.err != nil {
		result.NetworkRuntime = defaultNetworkRuntimeStatus("")
		result.NetworkRuntimeAvailable = false
		result.NetworkRuntimeError = networkRuntime.err.Error()
	}
	if mqttRuntime.err != nil {
		result.MqttRuntime = defaultMqttRuntimeStatus(false)
		result.MqttRuntimeAvailable = false
		result.MqttRuntimeError = mqttRuntime.err.Error()
	}
	if timeRuntime.err != nil {
		result.TimeRuntime = defaultTimeRuntimeStatus(defaultTimeSettings())
		result.TimeRuntimeAvailable = false
		result.TimeRuntimeError = timeRuntime.err.Error()
	}
	if networkRuntime.err != nil && mqttRuntime.err != nil && timeRuntime.err != nil {
		result.BackendReachable = false
	}
	return result
}

func paginateDeviceTemplateManagement(view model.DeviceTemplateManagementView, requestedPage int) (model.DeviceTemplateManagementView, model.DeviceTemplatePagination) {
	totalItems := len(view.Templates)
	totalPages := 1
	if totalItems > 0 {
		totalPages = (totalItems + settingsTemplatePageSize - 1) / settingsTemplatePageSize
	}
	page := requestedPage
	if page < 1 {
		page = 1
	}
	if page > totalPages {
		page = totalPages
	}

	startIndex := (page - 1) * settingsTemplatePageSize
	endIndex := startIndex + settingsTemplatePageSize
	if startIndex > totalItems {
		startIndex = totalItems
	}
	if endIndex > totalItems {
		endIndex = totalItems
	}

	paged := view
	paged.Templates = append([]model.DeviceTemplateManagementItem(nil), view.Templates[startIndex:endIndex]...)
	pages := make([]int, totalPages)
	for index := range pages {
		pages[index] = index + 1
	}
	pagination := model.DeviceTemplatePagination{
		Page:        page,
		TotalPages:  totalPages,
		Pages:       pages,
		HasPrevious: page > 1,
		HasNext:     page < totalPages,
		Previous:    page - 1,
		Next:        page + 1,
		TotalItems:  totalItems,
	}
	if totalItems > 0 {
		pagination.RangeStart = startIndex + 1
		pagination.RangeEnd = endIndex
	}
	return paged, pagination
}

func displayDeviceTemplateManagement(view model.DeviceTemplateManagementView) model.DeviceTemplateManagementView {
	view = normalizeDeviceTemplateManagement(view)
	for index := range view.Templates {
		item := &view.Templates[index]
		item.EditorDefinition = model.DeviceTemplateDefinition{
			ID: item.TemplateID, DisplayName: item.TemplateName, Description: item.Description,
			DefaultStart:            uint16(item.DefaultStartRegister),
			DeviceAddressStride:     item.DeviceAddressStride,
			ReadBlocks:              append([]model.DeviceTemplateReadBlock(nil), item.ReadBlocks...),
			Builtin:                 item.Builtin,
			Fields:                  append([]model.DeviceTemplateField(nil), item.Fields...),
			WriteCommands:           cloneDeviceTemplateWriteCommands(item.WriteCommands),
			RealtimeGroupingEnabled: item.RealtimeGroupingEnabled,
			RealtimeGroups:          append([]model.DeviceTemplateRealtimeGroup(nil), item.RealtimeGroups...),
		}
		item.StartRegisterText = registerAddressDisplayText(item.DefaultStartRegister)
		if item.Builtin {
			item.TemplateKindText = "内置"
			item.TemplateTypeText = builtinTemplateTypeText(*item)
		} else {
			item.TemplateKindText = "自定义"
			item.TemplateTypeText = "自定义 Modbus 设备"
		}
		item.CapabilityText, item.CapabilityClass = templateCapabilityText(*item)
		for fieldIndex := range item.Fields {
			field := &item.Fields[fieldIndex]
			field.DisplayName = displayDataItemName(field.DisplayName, field.Key)
			field.DataTypeText = templateFieldDisplayTypeText(*field)
			field.EnumSummaryText = templateFieldEnumSummaryText(*field)
			registerStart := item.DefaultStartRegister + uint32(field.RegisterOffset)
			for _, block := range item.ReadBlocks {
				if block.BlockKey == field.ReadBlockKey && block.StartOffset >= 0 {
					registerStart += uint32(block.StartOffset)
					break
				}
			}
			registerEnd := registerStart + uint32(maxUint16(field.RegisterCount, 1)) - 1
			field.RegisterAddressText = registerAddressDisplayText(registerStart)
			if registerEnd > registerStart {
				field.RegisterAddressText += " ～ " + registerAddressDisplayText(registerEnd)
			}
			field.SummaryText = boolDisplayText(field.Summary)
			if field.Summary {
				field.AlarmCapabilityText, field.AlarmCapabilityClass = "告警候选", "status-ok"
			} else {
				field.AlarmCapabilityText, field.AlarmCapabilityClass = "非告警候选", "status-neutral"
			}
			if templateFieldWritable(*field, item.WriteCommands) {
				field.WritableText, field.WritableClass = "可写 / 可操作", "status-ok"
			} else {
				field.WritableText, field.WritableClass = "只读", "status-neutral"
			}
		}
	}
	return view
}

func cloneDeviceTemplateWriteCommands(source []model.DeviceTemplateWriteCommand) []model.DeviceTemplateWriteCommand {
	result := make([]model.DeviceTemplateWriteCommand, len(source))
	for index := range source {
		result[index] = source[index]
		result[index].FixedValues = append([]uint16(nil), source[index].FixedValues...)
		result[index].Warnings = append([]string(nil), source[index].Warnings...)
		result[index].ValueFields = append([]model.DeviceTemplateWriteCommandField(nil), source[index].ValueFields...)
		for fieldIndex := range result[index].ValueFields {
			result[index].ValueFields[fieldIndex].Options = append(
				[]model.DeviceTemplateWriteCommandOption(nil),
				source[index].ValueFields[fieldIndex].Options...,
			)
		}
	}
	return result
}

func templateFieldWritable(field model.DeviceTemplateField, commands []model.DeviceTemplateWriteCommand) bool {
	for _, command := range commands {
		for _, valueField := range command.ValueFields {
			if strings.EqualFold(strings.TrimSpace(valueField.Key), strings.TrimSpace(field.Key)) {
				return true
			}
		}
		if command.HasAbsolute || command.RegisterCount == 0 {
			continue
		}
		fieldStart := uint32(field.RegisterOffset)
		fieldEnd := fieldStart + uint32(maxUint16(field.RegisterCount, 1))
		commandStart := uint32(command.RegisterOffset)
		commandEnd := commandStart + uint32(command.RegisterCount)
		if fieldStart < commandEnd && fieldEnd > commandStart {
			return true
		}
	}
	return false
}

func maxUint16(value uint16, fallback uint16) uint16 {
	if value > fallback {
		return value
	}
	return fallback
}

func builtinTemplateTypeText(item model.DeviceTemplateManagementItem) string {
	name := strings.ToUpper(strings.TrimSpace(item.TemplateID + " " + item.TemplateName + " " + item.Description))
	switch {
	case strings.Contains(name, "SF6"):
		return "SF6 监测"
	case strings.Contains(name, "WT") || strings.Contains(name, "测温") || strings.Contains(name, "温度"):
		return "无线测温"
	case strings.Contains(name, "RD") || strings.Contains(name, "接地") || strings.Contains(name, "电阻"):
		return "接地电阻"
	case strings.Contains(name, "EM100") || strings.Contains(name, "绝缘"):
		return "绝缘监测"
	default:
		return "采集设备类型"
	}
}

func templateCapabilityText(item model.DeviceTemplateManagementItem) (string, string) {
	if len(item.WriteCommands) > 0 {
		return "支持设备操作", "status-ok"
	}
	return "只读采集", "status-neutral"
}

func registerAddressDisplayText(address uint32) string {
	return fmt.Sprintf("%d / 0x%04X", address, address)
}

func templateFieldDataTypeText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "uint16":
		return "无符号 16 位"
	case "int16":
		return "有符号 16 位"
	case "uint32":
		return "无符号 32 位"
	case "int32":
		return "有符号 32 位"
	case "float", "float32":
		return "32 位浮点数"
	case "bool":
		return "状态位（单比特）"
	default:
		return defaultString(value, "-")
	}
}

const (
	defaultTemplateByteOrder = "big_endian"
	defaultTemplateWordOrder = "high_word_first"
)

func normalizeDeviceTemplateDefinition(definition model.DeviceTemplateDefinition) model.DeviceTemplateDefinition {
	// IPC 协议要求所有列表参数都是 JSON 数组。以非 nil 空切片作为复制起点，
	// 避免前端提交的 [] 在规范化后退化为 null。
	definition.ReadBlocks = append([]model.DeviceTemplateReadBlock{}, definition.ReadBlocks...)
	definition.Fields = append([]model.DeviceTemplateField{}, definition.Fields...)
	definition.WriteCommands = cloneDeviceTemplateWriteCommands(definition.WriteCommands)
	definition.RealtimeGroups = append([]model.DeviceTemplateRealtimeGroup{}, definition.RealtimeGroups...)
	applyDeviceTemplateFieldOrderDefaults(definition.Fields)
	return definition
}

func normalizeDeviceTemplateManagement(view model.DeviceTemplateManagementView) model.DeviceTemplateManagementView {
	view.Templates = append([]model.DeviceTemplateManagementItem(nil), view.Templates...)
	for index := range view.Templates {
		item := &view.Templates[index]
		item.ReadBlocks = append([]model.DeviceTemplateReadBlock(nil), item.ReadBlocks...)
		item.Fields = append([]model.DeviceTemplateField(nil), item.Fields...)
		item.RealtimeGroups = append([]model.DeviceTemplateRealtimeGroup(nil), item.RealtimeGroups...)
		applyDeviceTemplateFieldOrderDefaults(item.Fields)
	}
	return view
}

func applyDeviceTemplateFieldOrderDefaults(fields []model.DeviceTemplateField) {
	for index := range fields {
		if strings.TrimSpace(fields[index].ByteOrder) == "" {
			fields[index].ByteOrder = defaultTemplateByteOrder
		}
		if strings.TrimSpace(fields[index].WordOrder) == "" {
			fields[index].WordOrder = defaultTemplateWordOrder
		}
		items := make([]model.DeviceTemplateEnumItem, len(fields[index].EnumItems))
		copy(items, fields[index].EnumItems)
		fields[index].EnumItems = items
	}
}

func templateFieldDisplayTypeText(field model.DeviceTemplateField) string {
	if strings.EqualFold(strings.TrimSpace(field.ParserID), "bit_uint16") ||
		strings.EqualFold(strings.TrimSpace(field.DataType), "bool") {
		return fmt.Sprintf("状态位 bit%d", field.BitIndex)
	}
	switch strings.ToLower(strings.TrimSpace(field.ParserID)) {
	case "scaled_high_uint8":
		return "高 8 位"
	case "scaled_low_uint8":
		return "低 8 位"
	}

	text := templateFieldDataTypeText(field.DataType)
	if templateFieldUses32Bits(field.DataType) {
		text += " / " + templateFieldDataOrderText(field)
	}
	return text
}

func templateFieldEnumSummaryText(field model.DeviceTemplateField) string {
	if len(field.EnumItems) == 0 {
		return ""
	}
	if len(field.EnumItems) <= 2 {
		parts := make([]string, 0, len(field.EnumItems))
		for _, item := range field.EnumItems {
			parts = append(parts, fmt.Sprintf("%d=%s", item.Value, strings.TrimSpace(item.Label)))
		}
		return strings.Join(parts, "，")
	}
	return fmt.Sprintf("%d 个枚举项", len(field.EnumItems))
}

func templateFieldUses32Bits(dataType string) bool {
	switch strings.ToLower(strings.TrimSpace(dataType)) {
	case "uint32", "int32", "float32":
		return true
	default:
		return false
	}
}

func templateFieldDataOrderText(field model.DeviceTemplateField) string {
	byteOrder := strings.ToLower(strings.TrimSpace(field.ByteOrder))
	wordOrder := strings.ToLower(strings.TrimSpace(field.WordOrder))
	if byteOrder == "" {
		byteOrder = defaultTemplateByteOrder
	}
	if wordOrder == "" {
		wordOrder = defaultTemplateWordOrder
	}
	switch byteOrder + "/" + wordOrder {
	case "big_endian/high_word_first":
		return "ABCD"
	case "little_endian/high_word_first":
		return "BADC"
	case "big_endian/low_word_first":
		return "CDAB"
	case "little_endian/low_word_first":
		return "DCBA"
	default:
		return "未知排列"
	}
}

func boolDisplayText(value bool) string {
	if value {
		return "是"
	}
	return "否"
}

func (s *ConsoleService) UpdateSystemSettings(
	ctx context.Context,
	request model.SystemSettingsUpdateRequest,
) model.ActionFeedback {
	result, err := s.backend.UpdateSystemSettings(ctx, request)
	if err != nil {
		s.invalidateSystemDisplayName()
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	displayName := result.Settings.DisplayName
	if strings.TrimSpace(displayName) == "" && strings.TrimSpace(request.DisplayName) != "" {
		displayName = request.DisplayName
	}
	s.cacheSystemDisplayName(displayName)
	message := result.Message
	if message == "" {
		message = "系统设置已保存"
	}
	return model.ActionFeedback{Success: true, Message: message}
}

func (s *ConsoleService) UpdateNetworkSettings(
	ctx context.Context,
	request model.NetworkSettingsUpdateRequest,
) model.ActionFeedback {
	result, err := s.backend.UpdateNetworkSettings(ctx, request)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := result.Message
	if message == "" {
		message = "网络配置已保存"
	}
	return model.ActionFeedback{Success: true, Message: message}
}

func (s *ConsoleService) SaveAndApplyTimeSettings(ctx context.Context, request model.TimeSettingsUpdateRequest) model.ActionFeedback {
	result, err := s.backend.SaveAndApplyTimeSettings(ctx, request)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "日期与时间设置已保存并应用"
	}
	if warning := strings.TrimSpace(result.WarningMessage); warning != "" {
		message += "；" + warning
	}
	return model.ActionFeedback{Success: true, Message: message}
}

func (s *ConsoleService) SyncTimeNow(ctx context.Context) model.ActionFeedback {
	result, err := s.backend.SyncTimeNow(ctx)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := strings.TrimSpace(result.Message)
	if warning := strings.TrimSpace(result.WarningMessage); warning != "" {
		message += "；" + warning
	}
	return model.ActionFeedback{Success: result.Synchronized, Message: message}
}

func (s *ConsoleService) SetManualSystemTime(ctx context.Context, epochMS uint64, source string) model.ActionFeedback {
	result, err := s.backend.SetManualSystemTime(ctx, model.ManualTimeSetRequest{EpochMS: epochMS, Source: source})
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := strings.TrimSpace(result.Message)
	if warning := strings.TrimSpace(result.WarningMessage); warning != "" {
		message += "；" + warning
	}
	return model.ActionFeedback{Success: result.TimeSet, Message: message}
}

func (s *ConsoleService) UpdateMqttSettings(
	ctx context.Context,
	request model.MqttSettingsUpdateRequest,
) model.ActionFeedback {
	result, err := s.backend.UpdateMqttSettings(ctx, request)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := result.Message
	if message == "" {
		if result.Applied {
			message = "配置已保存并应用"
		} else {
			applyError := strings.TrimSpace(result.ApplyError)
			if applyError == "" {
				applyError = "未返回具体错误"
			}
			message = "配置已保存，但运行应用失败：" + applyError
		}
	}
	feedbackType := "success"
	if !result.Applied {
		feedbackType = "warning"
	}
	return model.ActionFeedback{Success: true, Message: message, Type: feedbackType}
}

func (s *ConsoleService) ExportSystemConfig(ctx context.Context) (model.ConfigExportBundle, error) {
	return s.backend.ExportSystemConfig(ctx)
}

func (s *ConsoleService) ImportSystemConfig(ctx context.Context, bundle model.ConfigExportBundle) model.ActionFeedback {
	result, err := s.backend.ImportSystemConfig(ctx, bundle)
	if err != nil {
		s.invalidateSystemDisplayName()
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "导入系统配置失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "配置导入成功，自定义设备类型与采集运行配置已刷新；导入的网络和时间设置已保存，将在设备重启或通过对应设置页手动应用后生效，请重新确认目标设备网络；MQTT 密码和 TLS 证书文件内容不会随配置文件导入，请人工补齐凭据与证书。"
	}
	if result.Imported {
		s.cacheSystemDisplayName(bundle.SystemSettings.DisplayName)
	} else {
		s.invalidateSystemDisplayName()
	}
	return model.ActionFeedback{Success: result.Imported, Message: message}
}

func (s *ConsoleService) SaveAndApplyNetworkSettings(
	ctx context.Context,
	request model.NetworkSettingsUpdateRequest,
) model.ActionFeedback {
	applyResult, err := s.backend.SaveAndApplyNetworkSettings(ctx, request)
	if err != nil {
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "保存并应用网络配置失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}

	message := strings.TrimSpace(applyResult.Message)
	if message == "" {
		targetIP := strings.TrimSpace(applyResult.CurrentIPAddress)
		if targetIP == "" {
			targetIP = strings.TrimSpace(request.IPAddress)
		}
		if request.Mode == "dhcp" {
			message = "网络配置已切换为 DHCP，当前地址为 http://" + targetIP + "。"
		} else {
			message = "网络配置已保存并应用到系统网口，请使用新的 IP 地址 http://" + targetIP + " 重新访问页面。"
		}
	}
	return model.ActionFeedback{Success: true, Message: message}
}

func (s *ConsoleService) RequestFactoryReset(ctx context.Context) model.ActionFeedback {
	result, err := s.backend.RequestFactoryReset(ctx)
	if err != nil {
		s.invalidateSystemDisplayName()
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "恢复出厂数据失败"
		} else if !strings.Contains(message, "恢复出厂数据失败") {
			message = "恢复出厂数据失败: " + message
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := result.Message
	if message == "" {
		if result.ResetCompleted {
			message = "恢复出厂数据成功"
		} else {
			message = "恢复出厂数据失败"
		}
	}
	if result.ResetCompleted {
		s.cacheSystemDisplayName("")
	} else {
		s.invalidateSystemDisplayName()
	}
	return model.ActionFeedback{Success: result.ResetCompleted, Message: message}
}

func (s *ConsoleService) GetWebAuthStatus(ctx context.Context) (model.WebAuthStatus, error) {
	return s.backend.GetWebAuthStatus(ctx)
}

func (s *ConsoleService) VerifyWebLogin(
	ctx context.Context,
	request model.WebLoginRequest,
) (model.WebLoginResult, error) {
	return s.backend.VerifyWebLogin(ctx, request)
}

func (s *ConsoleService) ChangeWebPassword(
	ctx context.Context,
	request model.WebPasswordChangeRequest,
) model.ActionFeedback {
	result, err := s.backend.ChangeWebPassword(ctx, request)
	if err != nil {
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "修改当前账户密码失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	if !result.Success {
		message := strings.TrimSpace(result.Message)
		if message == "" {
			message = "修改当前账户密码失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "当前账户密码已更新"
	}
	return model.ActionFeedback{Success: true, Message: message}
}

func (s *ConsoleService) SetWebViewerPassword(
	ctx context.Context,
	request model.WebViewerPasswordSetRequest,
) model.ActionFeedback {
	result, err := s.backend.SetWebViewerPassword(ctx, request)
	if err != nil {
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "设置只读用户密码失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "只读用户密码已更新"
	}
	return model.ActionFeedback{Success: result.Success, Message: message}
}

func (s *ConsoleService) ListWebUsers(ctx context.Context) ([]model.WebUser, error) {
	return s.backend.ListWebUsers(ctx)
}

func (s *ConsoleService) CreateWebUser(ctx context.Context, request model.WebUserCreateRequest) model.ActionFeedback {
	result, err := s.backend.CreateWebUser(ctx, request)
	if err != nil {
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "创建用户失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "用户已创建"
	}
	return model.ActionFeedback{Success: result.Success, Message: message}
}

func (s *ConsoleService) UpdateWebUser(ctx context.Context, request model.WebUserUpdateRequest) model.ActionFeedback {
	result, err := s.backend.UpdateWebUser(ctx, request)
	if err != nil {
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "更新用户失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "用户已更新"
	}
	return model.ActionFeedback{Success: result.Success, Message: message}
}

func (s *ConsoleService) ResetWebUserPassword(ctx context.Context, request model.WebUserPasswordResetRequest) model.ActionFeedback {
	result, err := s.backend.ResetWebUserPassword(ctx, request)
	if err != nil {
		message := strings.TrimSpace(err.Error())
		if message == "" {
			message = "重置用户密码失败"
		}
		return model.ActionFeedback{Success: false, Message: message}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "用户密码已重置"
	}
	return model.ActionFeedback{Success: result.Success, Message: message}
}

func defaultSystemSettings() model.SystemSettings {
	// 后端不可达时的页面降级默认值，需要与 C++ backend/src/model/system_settings.h 保持同步。
	return model.SystemSettings{
		DeviceName:   "边缘计算控制器",
		SiteLocation: "未设置",
		DisplayName:  "珠海知更通讯管理系统",
	}
}

func defaultNetworkSettings() model.NetworkSettings {
	return model.NetworkSettings{
		Mode:          "static",
		InterfaceName: "eth0",
		IPAddress:     "192.168.0.172",
		Netmask:       "255.255.254.0",
		Gateway:       "192.168.1.1",
		DNSServers:    []string{"223.5.5.5", "8.8.8.8"},
		ApplyModeText: "目标静态网络配置已保存",
	}
}

func defaultTimeSettings() model.TimeSettings {
	return model.TimeSettings{Timezone: "Asia/Shanghai", NTPEnabled: false, NTPPrimary: "ntp.aliyun.com", NTPSecondary: "pool.ntp.org"}
}

func defaultTimeRuntimeStatus(settings model.TimeSettings) model.TimeRuntimeStatus {
	return model.TimeRuntimeStatus{
		Timezone:            settings.Timezone,
		NTPEnabled:          settings.NTPEnabled,
		NTPProcessState:     "stopped",
		NTPProcessStateText: "NTP 状态不可用",
		SyncState:           "error",
		SyncStateText:       "状态不可用",
	}
}

func defaultNetworkRuntimeStatus(interfaceName string) model.NetworkRuntimeStatus {
	if strings.TrimSpace(interfaceName) == "" {
		interfaceName = "eth0"
	}
	return model.NetworkRuntimeStatus{
		ConfiguredMode: "static",
		InterfaceName:  interfaceName,
		Operstate:      "unknown",
		LinkState:      "unknown",
		LinkStateText:  "未知",
		Message:        "当前网口状态不可用",
	}
}

func defaultMqttSettings() model.MqttSettings {
	return model.MqttSettings{
		Enabled:                false,
		BrokerPort:             1883,
		NodeID:                 "edge-controller",
		TopicPrefix:            "edge-controller",
		PublishIntervalSeconds: 10,
		QoS:                    0,
		RetainStatus:           true,
		KeepAliveSeconds:       60,
		TLSEnabled:             false,
		TLSCAFile:              "",
		TLSClientCertFile:      "",
		TLSClientKeyFile:       "",
		TLSInsecure:            false,
	}
}

func defaultMqttRuntimeStatus(enabled bool) model.MqttRuntimeStatus {
	state := "disabled"
	if enabled {
		state = "disconnected"
	}
	return model.MqttRuntimeStatus{
		Enabled: enabled,
		State:   state,
	}
}
