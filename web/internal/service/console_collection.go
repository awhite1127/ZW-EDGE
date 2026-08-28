package service

// 本文件组装采集管理页面的通道、主站和设备配置视图，并生成串口候选项与配置诊断提示。

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"unicode/utf8"

	"edge-web/internal/model"
)

func normalizeCommunicationTraceLimit(value int) int {
	if value <= 0 {
		return communicationTraceDefaultLimit
	}
	if value > communicationTraceMaxLimit {
		return communicationTraceMaxLimit
	}
	return value
}

func (s *ConsoleService) ListSerialPorts(ctx context.Context) ([]model.SerialPortInfo, error) {
	return s.backend.ListSerialPorts(ctx)
}

func (s *ConsoleService) ListChannels(ctx context.Context) ([]model.ChannelConfig, error) {
	return s.backend.ListChannels(ctx)
}

func (s *ConsoleService) CreateChannelConfig(
	ctx context.Context,
	request model.ChannelConfigUpdateRequest,
) (model.ChannelConfigUpdateResult, error) {
	return s.backend.CreateChannelConfig(ctx, request)
}

func (s *ConsoleService) UpdateChannelConfig(
	ctx context.Context,
	request model.ChannelConfigUpdateRequest,
) (model.ChannelConfigUpdateResult, error) {
	return s.backend.UpdateChannelConfig(ctx, request)
}

func (s *ConsoleService) DeleteChannelConfig(
	ctx context.Context,
	channelID string,
) (model.ChannelConfigDeleteResult, error) {
	return s.backend.DeleteChannelConfig(ctx, channelID)
}

func (s *ConsoleService) GetChannelCommunicationTraces(
	ctx context.Context,
	channelID string,
	limit int,
) (model.ChannelCommunicationTraces, error) {
	channelID = strings.TrimSpace(channelID)
	if channelID == "" {
		return model.ChannelCommunicationTraces{}, errors.New("缺少通道 ID")
	}

	query := model.CommunicationTraceQuery{
		ChannelID: channelID,
		Limit:     normalizeCommunicationTraceLimit(limit),
	}
	return s.backend.GetChannelCommunicationTraces(ctx, query)
}

func (s *ConsoleService) ClearChannelCommunicationTraces(
	ctx context.Context,
	channelID string,
) (model.CommunicationTraceClearResult, error) {
	channelID = strings.TrimSpace(channelID)
	if channelID == "" {
		return model.CommunicationTraceClearResult{}, errors.New("缺少通道 ID")
	}
	return s.backend.ClearChannelCommunicationTraces(ctx, channelID)
}

func (s *ConsoleService) CreateMasterConfig(
	ctx context.Context,
	request model.MasterConfigUpdateRequest,
) (model.MasterConfigUpdateResult, error) {
	return s.backend.CreateMasterConfig(ctx, request)
}

func (s *ConsoleService) UpdateMasterConfig(
	ctx context.Context,
	request model.MasterConfigUpdateRequest,
) (model.MasterConfigUpdateResult, error) {
	return s.backend.UpdateMasterConfig(ctx, request)
}

func (s *ConsoleService) DeleteMasterConfig(
	ctx context.Context,
	masterID string,
) (model.MasterConfigDeleteResult, error) {
	return s.backend.DeleteMasterConfig(ctx, masterID)
}

// CollectionLoadResult 汇总采集管理首屏的三个区块。
type CollectionLoadResult struct {
	Channels       model.ChannelsLoadResult
	Masters        model.MastersLoadResult
	Devices        model.DevicesLoadResult
	PollingKnown   bool
	PollingRunning bool
	PollingState   string
}

// LoadCollection 对采集管理首屏所需的共享数据只读取一次，
// 避免组合三个独立 API 加载器时重复请求通道、主站、系统状态和设备类型。
func (s *ConsoleService) LoadCollection(ctx context.Context) CollectionLoadResult {
	var wg sync.WaitGroup
	channels := startLoad(ctx, &wg, s.backend.ListChannels)
	masters := startLoad(ctx, &wg, s.backend.ListMasters)
	devices := startLoad(ctx, &wg, s.backend.ListDevices)
	systemStatus := startLoad(ctx, &wg, s.backend.GetSystemStatus)
	serialPorts := startLoad(ctx, &wg, s.backend.ListSerialPorts)
	configSummary := startLoad(ctx, &wg, s.backend.GetConfigSummary)
	wg.Wait()

	return CollectionLoadResult{
		Channels: buildChannelsLoadResult(
			channels.value, systemStatus.value, serialPorts.value,
			channels.err, systemStatus.err, serialPorts.err,
		),
		Masters: buildMastersLoadResult(
			masters.value, channels.value, systemStatus.value, configSummary.value,
			masters.err, channels.err, systemStatus.err, configSummary.err,
		),
		Devices: buildDevicesLoadResult(
			devices.value, masters.value, channels.value, systemStatus.value, configSummary.value,
			devices.err, masters.err, channels.err, systemStatus.err, configSummary.err,
		),
		PollingKnown:   systemStatus.err == nil,
		PollingRunning: systemStatus.err == nil && systemStatus.value.PollingRunning,
		PollingState:   systemStatus.value.PollingState,
	}
}

func (s *ConsoleService) LoadChannels(ctx context.Context) model.ChannelsLoadResult {
	var wg sync.WaitGroup
	channels := startLoad(ctx, &wg, s.backend.ListChannels)
	systemStatus := startLoad(ctx, &wg, s.backend.GetSystemStatus)
	serialPorts := startLoad(ctx, &wg, s.backend.ListSerialPorts)
	wg.Wait()

	return buildChannelsLoadResult(
		channels.value, systemStatus.value, serialPorts.value,
		channels.err, systemStatus.err, serialPorts.err,
	)
}

// buildChannelsLoadResult 使用已取得的共享快照构造通道区块。
func buildChannelsLoadResult(
	channels []model.ChannelConfig,
	systemStatus model.SystemStatus,
	serialPorts []model.SerialPortInfo,
	configErr error,
	statusErr error,
	serialErr error,
) model.ChannelsLoadResult {
	warning := joinPartialErrors(
		partialError("通道配置获取", configErr),
		partialError("通道状态获取", statusErr),
		partialError("系统串口发现", serialErr),
	)
	reachable := configErr == nil || statusErr == nil || serialErr == nil

	serialState := model.SectionState{Available: serialErr == nil}
	if serialErr != nil {
		serialState.ErrorMessage = serialErr.Error()
	}

	if configErr != nil {
		return model.ChannelsLoadResult{
			SerialPorts:          serialPorts,
			SerialPortsState:     serialState,
			WarningMessage:       warning,
			BackendReachable:     reachable,
			PrimaryDataAvailable: false,
		}
	}

	statusByID := make(map[string]model.ChannelStatus)
	if statusErr == nil {
		for _, status := range systemStatus.ChannelStatusList {
			statusByID[status.ChannelID] = status
		}
	}

	bindings := buildChannelSerialBindings(channels, serialPorts)
	rows := make([]model.ChannelRow, 0, len(channels))
	for _, channel := range channels {
		status, ok := statusByID[channel.ChannelID]
		rows = append(rows, model.ChannelRow{
			Config:        channel,
			Status:        status,
			HasStatus:     ok,
			SerialBinding: bindings[channel.ChannelID],
			EndpointText:  channelEndpointText(channel),
			ParamText:     channelParamText(channel),
			TypeText:      channelTypeText(channel.ChannelType),
		})
	}

	return model.ChannelsLoadResult{
		Rows:                 rows,
		SerialPorts:          serialPorts,
		SerialPortsState:     serialState,
		WarningMessage:       warning,
		BackendReachable:     reachable,
		PrimaryDataAvailable: true,
	}
}

func (s *ConsoleService) LoadMasters(ctx context.Context) model.MastersLoadResult {
	var wg sync.WaitGroup
	masters := startLoad(ctx, &wg, s.backend.ListMasters)
	channels := startLoad(ctx, &wg, s.backend.ListChannels)
	systemStatus := startLoad(ctx, &wg, s.backend.GetSystemStatus)
	configSummary := startLoad(ctx, &wg, s.backend.GetConfigSummary)
	wg.Wait()

	return buildMastersLoadResult(
		masters.value, channels.value, systemStatus.value, configSummary.value,
		masters.err, channels.err, systemStatus.err, configSummary.err,
	)
}

// buildMastersLoadResult 使用已取得的共享快照构造主站区块。
func buildMastersLoadResult(
	masters []model.MasterNodeConfig,
	channels []model.ChannelConfig,
	systemStatus model.SystemStatus,
	configSummary model.ConfigSummary,
	configErr error,
	channelErr error,
	statusErr error,
	summaryErr error,
) model.MastersLoadResult {
	warning := joinPartialErrors(
		partialError("主站配置获取", configErr),
		partialError("通道选项获取", channelErr),
		partialError("主站状态获取", statusErr),
		partialError("设备类型获取", summaryErr),
	)
	reachable := configErr == nil || channelErr == nil || statusErr == nil || summaryErr == nil
	if configErr != nil {
		return model.MastersLoadResult{
			WarningMessage:       warning,
			BackendReachable:     reachable,
			PrimaryDataAvailable: false,
		}
	}

	statusByID := make(map[string]model.MasterNodeStatus)
	if statusErr == nil {
		for _, status := range systemStatus.MasterStatusList {
			statusByID[status.MasterID] = status
		}
	}

	channelStatusByID := make(map[string]model.ChannelStatus)
	if statusErr == nil {
		for _, status := range systemStatus.ChannelStatusList {
			channelStatusByID[status.ChannelID] = status
		}
	}

	deviceTemplates := normalizedDeviceTemplates(configSummary.DeviceTemplates)
	rows := make([]model.MasterRow, 0, len(masters))
	for _, master := range masters {
		status, ok := statusByID[master.MasterID]
		rows = append(rows, buildMasterRow(master, status, ok, channels, deviceTemplates))
	}

	return model.MastersLoadResult{
		Rows:                 rows,
		ChannelOptions:       buildMasterChannelOptions(channels, channelStatusByID),
		DeviceTemplates:      deviceTemplates,
		WarningMessage:       warning,
		BackendReachable:     reachable,
		PrimaryDataAvailable: true,
	}
}

func buildMasterRow(
	master model.MasterNodeConfig,
	status model.MasterNodeStatus,
	hasStatus bool,
	channels []model.ChannelConfig,
	deviceTemplates []model.DeviceTemplateDefinition,
) model.MasterRow {
	row := model.MasterRow{
		Config:       master,
		Status:       status,
		HasStatus:    hasStatus,
		ProtocolText: masterProtocolText(master.Protocol),
		TargetLabel:  "绑定通道",
		TargetValue:  defaultString(master.ChannelID, "-"),
	}
	definition, ok := model.FindDeviceTemplateIn(deviceTemplates, master.DeviceTemplate)
	if ok {
		row.DeviceTemplateName = definition.DisplayName
		row.DeviceTemplateNotes = fmt.Sprintf(
			"%s，默认基地址 %d，%d 个读取区块，设备地址跨度 %d",
			definition.Description,
			definition.DefaultStart,
			len(definition.ReadBlocks),
			definition.DeviceAddressStride,
		)
	} else {
		row.DeviceTemplateName = "未知设备类型"
		row.DeviceTemplateNotes = "所属设备类型未识别，请检查主站配置"
	}

	for _, channel := range channels {
		if channel.ChannelID == master.ChannelID {
			row.TargetValue = fmt.Sprintf("%s / %s", defaultString(channel.ChannelName, "未命名通道"), channel.ChannelID)
			return row
		}
	}

	if master.ChannelID != "" {
		row.TargetValue = master.ChannelID + "（当前绑定已失效）"
	}
	return row
}

func buildMasterChannelOptions(
	channels []model.ChannelConfig,
	statusByID map[string]model.ChannelStatus,
) []model.MasterChannelOption {
	options := make([]model.MasterChannelOption, 0, len(channels))
	for _, channel := range channels {
		status, hasStatus := statusByID[channel.ChannelID]
		portPath := channelEndpointText(channel)
		option := model.MasterChannelOption{
			ChannelID:   channel.ChannelID,
			ChannelName: defaultString(channel.ChannelName, "未命名通道"),
			ChannelType: channel.ChannelType,
			PortPath:    portPath,
			Enabled:     channel.Enabled,
			HasStatus:   hasStatus,
			IsFault:     hasStatus && (diagnosisHasIssue(status.Diagnosis) || status.LastErrorMessage != ""),
		}
		option.DisplayLabel = fmt.Sprintf("%s / %s / %s：%s", option.ChannelName, option.ChannelID, channelTypeText(channel.ChannelType), portPath)
		if !option.Enabled {
			option.DisplayLabel += " / 已禁用"
		}
		if option.IsFault {
			option.DisplayLabel += " / 异常"
		}
		options = append(options, option)
	}
	return options
}

func (s *ConsoleService) LoadDevices(ctx context.Context) model.DevicesLoadResult {
	// 并行读取设备配置及其关联的主站、通道、状态和设备类型。
	var wg sync.WaitGroup
	devices := startLoad(ctx, &wg, s.backend.ListDevices)
	masters := startLoad(ctx, &wg, s.backend.ListMasters)
	channels := startLoad(ctx, &wg, s.backend.ListChannels)
	systemStatus := startLoad(ctx, &wg, s.backend.GetSystemStatus)
	configSummary := startLoad(ctx, &wg, s.backend.GetConfigSummary)
	wg.Wait()

	return buildDevicesLoadResult(
		devices.value, masters.value, channels.value, systemStatus.value, configSummary.value,
		devices.err, masters.err, channels.err, systemStatus.err, configSummary.err,
	)
}

// buildDevicesLoadResult 使用已取得的共享快照构建设备区块。
func buildDevicesLoadResult(
	devices []model.DeviceConfig,
	masters []model.MasterNodeConfig,
	channels []model.ChannelConfig,
	systemStatus model.SystemStatus,
	configSummary model.ConfigSummary,
	configErr error,
	masterErr error,
	channelErr error,
	statusErr error,
	summaryErr error,
) model.DevicesLoadResult {
	// 汇总局部读取错误；设备清单失败时无法继续构造主数据。
	warning := joinPartialErrors(
		partialError("设备配置获取", configErr),
		partialError("主站选项获取", masterErr),
		partialError("通道选项获取", channelErr),
		partialError("设备状态获取", statusErr),
		partialError("设备类型获取", summaryErr),
	)
	reachable := configErr == nil || masterErr == nil || channelErr == nil || statusErr == nil || summaryErr == nil
	if configErr != nil {
		return model.DevicesLoadResult{
			WarningMessage:       warning,
			BackendReachable:     reachable,
			PrimaryDataAvailable: false,
		}
	}

	// 为关联数据建立索引，再逐台设备组装页面行。
	statusByID := make(map[string]model.DeviceStatus)
	if statusErr == nil {
		for _, status := range systemStatus.DeviceStatusList {
			statusByID[status.DeviceID] = status
		}
	}

	masterStatusByID := make(map[string]model.MasterNodeStatus)
	if statusErr == nil {
		for _, status := range systemStatus.MasterStatusList {
			masterStatusByID[status.MasterID] = status
		}
	}

	channelByID := make(map[string]model.ChannelConfig)
	if channelErr == nil {
		for _, channel := range channels {
			channelByID[channel.ChannelID] = channel
		}
	}

	rows := make([]model.DeviceRow, 0, len(devices))
	deviceTemplates := normalizedDeviceTemplates(configSummary.DeviceTemplates)
	for _, device := range devices {
		status, ok := statusByID[device.DeviceID]
		rows = append(rows, buildDeviceRow(device, status, ok, masters, channelByID, masterStatusByID, deviceTemplates))
	}

	return model.DevicesLoadResult{
		Rows:                 rows,
		WarningMessage:       warning,
		BackendReachable:     reachable,
		PrimaryDataAvailable: true,
	}
}

func (s *ConsoleService) UpdateDeviceDisplayName(
	ctx context.Context,
	request model.DeviceDisplayNameUpdateRequest,
) (model.DeviceConfig, error) {
	request.DeviceID = strings.TrimSpace(request.DeviceID)
	request.DisplayName = strings.TrimSpace(request.DisplayName)
	if request.DeviceID == "" {
		return model.DeviceConfig{}, errors.New("缺少设备 ID")
	}
	if utf8.RuneCountInString(request.DisplayName) > 40 {
		return model.DeviceConfig{}, errors.New("自定义设备名称不能超过 40 个字符")
	}
	return s.backend.UpdateDeviceDisplayName(ctx, request)
}

func (s *ConsoleService) UpdateDeviceDisplayNamesBatch(
	ctx context.Context,
	request model.DeviceDisplayNameBatchRequest,
) (model.DeviceDisplayNameBatchResult, error) {
	if len(request.Items) == 0 {
		return model.DeviceDisplayNameBatchResult{}, errors.New("批量命名至少需要一个设备")
	}
	for index := range request.Items {
		request.Items[index].DeviceID = strings.TrimSpace(request.Items[index].DeviceID)
		request.Items[index].DisplayName = strings.TrimSpace(request.Items[index].DisplayName)
		if request.Items[index].DeviceID == "" {
			return model.DeviceDisplayNameBatchResult{}, errors.New("批量命名包含缺少设备 ID 的条目")
		}
		if utf8.RuneCountInString(request.Items[index].DisplayName) > 40 {
			return model.DeviceDisplayNameBatchResult{}, errors.New("自定义设备名称不能超过 40 个字符")
		}
	}
	return s.backend.UpdateDeviceDisplayNamesBatch(ctx, request)
}

func buildDeviceRow(
	device model.DeviceConfig,
	status model.DeviceStatus,
	hasStatus bool,
	masters []model.MasterNodeConfig,
	channelByID map[string]model.ChannelConfig,
	masterStatusByID map[string]model.MasterNodeStatus,
	deviceTemplates []model.DeviceTemplateDefinition,
) model.DeviceRow {
	// 先设置关联失效时的保底展示内容。
	row := model.DeviceRow{
		Config:            device,
		Status:            status,
		HasStatus:         hasStatus,
		MasterLabel:       device.MasterID + "（当前绑定主站已失效）",
		MasterDetailText:  "-",
		MasterInvalid:     true,
		ChannelName:       "未绑定通道",
		ChannelText:       "未绑定通道",
		ChannelDetailText: "-",
		MasterStatusText:  "主站状态未知",
		SlaveAddressText:  "-",
		RealStartText:     "-",
	}
	// 找到绑定主站后补齐设备类型、通道、地址和主站状态。
	for _, master := range masters {
		if master.MasterID == device.MasterID {
			definition, ok := model.FindDeviceTemplateIn(deviceTemplates, master.DeviceTemplate)
			templateID := master.DeviceTemplate
			templateName := "未知设备类型"
			deviceAddressStride := 0
			if ok {
				deviceAddressStride = definition.DeviceAddressStride
				templateID = definition.ID
				templateName = definition.DisplayName
				row.TemplateName = definition.DisplayName
				row.TemplateReadBlockCount = len(definition.ReadBlocks)
				row.TemplateAddressStride = definition.DeviceAddressStride
				row.TemplateFields = definition.Fields
			} else {
				row.TemplateName = "未知设备类型"
			}
			realStart := uint32(master.BlockStartRegister) + uint32(device.RegisterOffset)
			row.RealStartText = fmt.Sprintf("%d", realStart)
			row.MasterLabel = defaultString(master.MasterName, "未命名主站")
			row.MasterDetailText = fmt.Sprintf("地址 %d / %s", master.TargetAddress, defaultString(master.MasterID, "未命名主站 ID"))
			row.MasterInvalid = false
			channel, hasChannel := channelByID[master.ChannelID]
			if hasChannel {
				row.ChannelName = defaultString(channel.ChannelName, "未命名通道")
				row.ChannelText = fmt.Sprintf("%s / %s", defaultString(channel.ChannelName, "未命名通道"), masterProtocolText(master.Protocol))
				row.ChannelDetailText = channelEndpointText(channel)
			} else if master.ChannelID != "" {
				row.ChannelName = "通道已失效"
				row.ChannelText = fmt.Sprintf("%s / 通道已失效", masterProtocolText(master.Protocol))
				row.ChannelDetailText = master.ChannelID
			} else {
				row.ChannelName = "未绑定通道"
				row.ChannelText = fmt.Sprintf("%s / 未绑定通道", masterProtocolText(master.Protocol))
			}
			row.SlaveAddressText = fmt.Sprintf("%d", master.TargetAddress)
			row.SequenceText = deviceSequenceText(master, device, deviceAddressStride)
			row.SummaryPoints = buildDeviceRowSummaryPoints(status, hasStatus, templateID, templateName)
			if masterStatus, ok := masterStatusByID[master.MasterID]; ok {
				if masterStatus.Online {
					row.MasterStatusText = "在线"
				} else {
					row.MasterStatusText = "离线"
				}
				if message := defaultString(diagnosisMessage(masterStatus.Diagnosis), masterStatus.LastErrorMessage); message != "" {
					row.MasterStatusText += " / " + message
				}
			} else if !master.Enabled {
				row.MasterStatusText = "已禁用"
			}
			return row
		}
	}
	if device.MasterID == "" {
		row.MasterLabel = "当前绑定主站已失效"
	}
	return row
}

func buildDeviceRowSummaryPoints(
	status model.DeviceStatus,
	hasStatus bool,
	templateID string,
	templateName string,
) []model.RealtimePointRow {
	if !hasStatus {
		return nil
	}
	points := status.Points
	if len(points) == 0 && status.HasResistance {
		points = []model.PointValue{{
			Key:   "resistance",
			Name:  "接地电阻",
			Value: status.ResistanceValue,
			Unit:  "Ω",
			// 当前扁平摘要由 HasResistance 明确标记为有效；字段质量不能复用设备通讯质量。
			Quality: "good",
			Valid:   true,
			Summary: true,
		}}
	}
	return buildRealtimeSummaryPoints(model.DeviceRealtimeSnapshot{
		TemplateID:           templateID,
		TemplateName:         templateName,
		CommunicationQuality: status.CommunicationQuality,
		Points:               points,
	})
}

func deviceSequenceText(master model.MasterNodeConfig, device model.DeviceConfig, deviceAddressStride int) string {
	if deviceAddressStride <= 0 {
		return "-"
	}
	index := uint32(device.RegisterOffset)/uint32(deviceAddressStride) + 1
	return fmt.Sprintf("%d", index)
}
