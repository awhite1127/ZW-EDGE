package service

// 本文件组装系统概览页面及其局部刷新数据，并允许资源、存储、MQTT 等子状态独立降级。

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"edge-web/internal/ipc"
	"edge-web/internal/model"
)

// LoadOverview 汇总系统配置、运行状态、事件与采集对象，生成概览页数据。
func (s *ConsoleService) LoadOverview(ctx context.Context) model.OverviewPageData {
	// 概览页尽量展示可用信息。每个后端调用单独记录状态，最终用 hasAnyData 判断是否整体不可达。
	result := model.OverviewPageData{
		Settings:          defaultSystemSettings(),
		SettingsState:     model.SectionState{},
		SystemStatusState: model.SectionState{},
		ConfigState:       model.SectionState{},
		RecentErrorState:  model.SectionState{},
		EventsState:       model.SectionState{},
		ActiveAlarmsState: model.SectionState{},
	}
	// 首屏通过原子快照一次性返回页面数据，避免设置、状态、事件和采集拓扑扇出查询。
	snapshot, snapshotErr := s.backend.GetOverviewPageSnapshot(ctx)
	if snapshotErr == nil {
		if len(snapshot.RecentEvents) > 5 {
			snapshot.RecentEvents = snapshot.RecentEvents[:5]
		}
		result.Settings = snapshot.SystemSettings
		result.SettingsState.Available = true
		result.ConfigSummary = snapshot.ConfigSummary
		result.ConfigState.Available = true
		result.RuntimeSnapshot = snapshot.SystemOverviewSnapshot
		result.RuntimeSnapshotAvailable = true
		result.SystemStatus = snapshot.SystemOverviewSnapshot.SystemStatus
		result.SystemStatusState.Available = true
		result.RecentError = snapshot.SystemOverviewSnapshot.CurrentError
		result.RecentErrorState.Available = true
		result.RecentEvents = snapshot.RecentEvents
		result.EventsState.Available = true
		result.ActiveAlarms = snapshot.ActiveAlarms
		result.ActiveAlarmsState.Available = true
		result.Channels = snapshot.Channels
		result.Masters = snapshot.Masters
		result.Devices = snapshot.Devices
		return finalizeOverviewPage(result, true, true, true, true, nil)
	}
	if !overviewSnapshotAllowsPartialFallback(snapshotErr) {
		// 连接失败、超时或协议方法缺失时继续扇出不会得到更多数据，只会放大故障负载。
		message := snapshotErr.Error()
		result.SettingsState.ErrorMessage = message
		result.SystemStatusState.ErrorMessage = message
		result.ConfigState.ErrorMessage = message
		result.RecentErrorState.ErrorMessage = message
		result.EventsState.ErrorMessage = message
		result.ActiveAlarmsState.ErrorMessage = message
		return finalizeOverviewPage(result, false, false, false, false, nil)
	}

	// 快照不可用时分别请求各区块，使局部故障不影响其余内容展示。
	var hasAnyData bool
	var errs []string
	var channelsReadable bool
	var mastersReadable bool
	var devicesReadable bool

	settings, err := s.backend.GetSystemSettings(ctx)
	if err != nil {
		result.SettingsState.ErrorMessage = err.Error()
	} else {
		result.Settings = settings
		result.SettingsState.Available = true
		hasAnyData = true
	}

	// 聚合快照同时提供系统状态与当前诊断；概览降级路径仍固定使用当前协议，
	// 避免 Web/controller 版本不匹配时再回退两个旧查询。
	overviewSnapshot, overviewSnapshotErr := s.backend.GetSystemOverviewSnapshot(ctx)
	if overviewSnapshotErr == nil {
		result.RuntimeSnapshot = overviewSnapshot
		result.RuntimeSnapshotAvailable = true
		result.SystemStatus = overviewSnapshot.SystemStatus
		result.SystemStatusState.Available = true
		result.RecentError = overviewSnapshot.CurrentError
		result.RecentErrorState.Available = true
		hasAnyData = true
	} else {
		result.SystemStatusState.ErrorMessage = overviewSnapshotErr.Error()
		result.RecentErrorState.ErrorMessage = overviewSnapshotErr.Error()
		errs = append(errs, "系统状态与当前诊断获取失败")
	}

	// 补充配置摘要、近期事件和当前报警。
	configSummary, err := s.backend.GetConfigSummary(ctx)
	if err != nil {
		result.ConfigState.ErrorMessage = err.Error()
		errs = append(errs, "配置摘要获取失败")
	} else {
		result.ConfigSummary = configSummary
		result.ConfigState.Available = true
		hasAnyData = true
	}

	if events, err := s.backend.ListRecentEvents(ctx); err != nil {
		result.EventsState.ErrorMessage = err.Error()
		errs = append(errs, "历史事件获取失败")
	} else {
		if len(events) > 5 {
			events = events[:5]
		}
		result.RecentEvents = events
		result.EventsState.Available = true
		hasAnyData = true
	}
	if alarms, err := s.backend.ListActiveAlarms(ctx); err != nil {
		result.ActiveAlarmsState.ErrorMessage = err.Error()
		errs = append(errs, "当前报警获取失败")
	} else {
		result.ActiveAlarms = alarms
		result.ActiveAlarmsState.Available = true
		hasAnyData = true
	}

	// 加载采集对象清单，用于计算链路完整性和对象状态。
	if channels, err := s.backend.ListChannels(ctx); err == nil {
		result.Channels = channels
		channelsReadable = true
		hasAnyData = true
	}
	if masters, err := s.backend.ListMasters(ctx); err == nil {
		result.Masters = masters
		mastersReadable = true
		hasAnyData = true
	}
	if devices, err := s.backend.ListDevices(ctx); err == nil {
		result.Devices = devices
		devicesReadable = true
		hasAnyData = true
	}
	// 统一生成派生卡片，并根据各区块结果确定页面可达状态。
	return finalizeOverviewPage(
		result,
		channelsReadable,
		mastersReadable,
		devicesReadable,
		hasAnyData,
		errs,
	)
}

// 只有事件存储故障和当前运行状态冲突可能局限在快照的单个区块；
// 容量拒绝、协议错误和连接错误继续扇出只会放大故障负载。
func overviewSnapshotAllowsPartialFallback(err error) bool {
	var callErr *ipc.CallError
	if !errors.As(err, &callErr) || callErr == nil {
		return false
	}
	switch callErr.Code {
	case "io_error", "invalid_state":
		return true
	default:
		return false
	}
}

// finalizeOverviewPage 生成报警、链路和诊断卡片，并确定概览页整体可达状态。
func finalizeOverviewPage(
	result model.OverviewPageData,
	channelsReadable bool,
	mastersReadable bool,
	devicesReadable bool,
	hasAnyData bool,
	errs []string,
) model.OverviewPageData {
	prepareOverviewAlarmDisplay(&result)

	collectionLink := buildCollectionLinkOverview(
		result.Channels,
		result.Masters,
		result.Devices,
		result.ConfigSummary.DeviceTemplates,
		channelsReadable && mastersReadable && devicesReadable && result.ConfigState.Available,
	)
	result.CollectionObject = buildCollectionObjectOverview(
		collectionLink,
		result.Channels,
		result.Masters,
		result.Devices,
		result.ConfigSummary.DeviceTemplates,
		result.SystemStatus,
		result.SystemStatusState.Available,
		channelsReadable && mastersReadable && devicesReadable && result.ConfigState.Available,
	)

	result.DiagnosisCard = buildOverviewDiagnosisCard(
		result.RecentError,
		result.RecentErrorState.Available,
		result.SystemStatus,
		result.SystemStatusState.Available,
	)

	if !hasAnyData {
		result.BasePageData.BackendReachable = false
		result.BasePageData.ErrorMessage = "后端不可达，概览数据暂时无法获取"
		return result
	}

	result.BasePageData.BackendReachable = true
	if len(errs) > 0 {
		result.BasePageData.ErrorMessage = strings.Join(errs, "；")
	}
	return result
}

// prepareOverviewAlarmDisplay 整理报警对象名称和前三条报警预览。
func prepareOverviewAlarmDisplay(result *model.OverviewPageData) {
	if result == nil || len(result.ActiveAlarms) == 0 {
		return
	}
	devices := make(map[string]model.DeviceConfig, len(result.Devices))
	for _, device := range result.Devices {
		devices[device.DeviceID] = device
	}
	masters := make(map[string]model.MasterNodeConfig, len(result.Masters))
	for _, master := range result.Masters {
		masters[master.MasterID] = master
	}
	seen := make(map[string]struct{})
	names := make([]string, 0, 3)
	preview := make([]model.ActiveAlarm, 0, 3)
	allDevices := true
	for _, alarm := range result.ActiveAlarms {
		name, key, isDevice := overviewAlarmObjectName(alarm, devices, masters)
		alarm.DeviceName = name
		if _, exists := seen[key]; exists {
			continue
		}
		seen[key] = struct{}{}
		names = append(names, name)
		allDevices = allDevices && isDevice
		if len(preview) < 3 {
			preview = append(preview, alarm)
		}
	}
	result.ActiveAlarmPreview = preview
	result.RemainingAlarmCount = len(names) - len(preview)
	visible := strings.Join(names[:minInt(len(names), 3)], "、")
	if len(names) > 3 {
		unit := "个对象"
		if allDevices {
			unit = "台设备"
		}
		result.AlarmObjectText = fmt.Sprintf("%s 等 %d %s报警", visible, len(names), unit)
	} else if allDevices {
		result.AlarmObjectText = "报警设备：" + visible
	} else {
		result.AlarmObjectText = "报警对象：" + visible
	}
}

func overviewAlarmObjectName(
	alarm model.ActiveAlarm,
	devices map[string]model.DeviceConfig,
	masters map[string]model.MasterNodeConfig,
) (string, string, bool) {
	if device, ok := devices[alarm.DeviceID]; ok {
		name := strings.TrimSpace(device.DeviceName)
		if name == "" {
			name = strings.TrimSpace(device.SystemName)
		}
		if name == "" {
			name = device.DeviceID
		}
		return name, "device:" + device.DeviceID, true
	}
	if alarm.DeviceID != "" {
		name := strings.TrimSpace(alarm.DeviceName)
		if name == "" {
			name = alarm.DeviceID
		}
		return name, "device:" + alarm.DeviceID, true
	}
	if master, ok := masters[alarm.MasterID]; ok {
		name := strings.TrimSpace(master.MasterName)
		if name == "" {
			name = master.MasterID
		}
		return name, "master:" + master.MasterID, false
	}
	name := strings.TrimSpace(alarm.DeviceName)
	if name == "" {
		name = strings.TrimSpace(alarm.MasterID)
	}
	if name == "" {
		name = "未识别告警对象"
	}
	return name, "object:" + name, false
}

// buildCollectionLinkOverview 检查通道、主站、设备类型和设备数量是否形成完整采集链路。
func buildCollectionLinkOverview(
	channels []model.ChannelConfig,
	masters []model.MasterNodeConfig,
	devices []model.DeviceConfig,
	deviceTemplates []model.DeviceTemplateDefinition,
	configReadable bool,
) model.CollectionLinkOverview {
	overview := model.CollectionLinkOverview{
		Status:      "empty",
		StatusText:  "采集链路待配置",
		StatusClass: "status-neutral",
		Summary:     "配置数据暂不可读，无法判断采集链路。",
	}
	if !configReadable {
		overview.Status = "blocked"
		overview.StatusText = "采集链路未闭合"
		overview.StatusClass = "status-warn"
		overview.BlockingText = "配置数据暂不可读，请检查配置服务状态。"
		return overview
	}

	// 建立启用通道与主站下设备数量索引。
	enabledChannelIDs := make(map[string]struct{})
	for _, channel := range channels {
		channelID := strings.TrimSpace(channel.ChannelID)
		if channelID != "" && channel.Enabled {
			enabledChannelIDs[channelID] = struct{}{}
		}
	}

	deviceCountByMaster := make(map[string]int)
	for _, device := range devices {
		masterID := strings.TrimSpace(device.MasterID)
		if masterID != "" {
			deviceCountByMaster[masterID]++
		}
	}

	// 按主站累计链路闭合情况及各类阻断原因。
	var enabledMasters int
	var closedMasters int
	var missingChannelMasters int
	var missingTemplateMasters int
	var invalidTemplateMasters int
	var invalidDeviceCountMasters int
	var noDeviceMasters int
	var deviceCountMismatchMasters int
	var expectedDeviceTotal int

	for _, master := range masters {
		if !master.Enabled {
			continue
		}
		enabledMasters++

		_, hasValidChannel := enabledChannelIDs[strings.TrimSpace(master.ChannelID)]
		if !hasValidChannel {
			missingChannelMasters++
		}

		templateID := strings.TrimSpace(master.DeviceTemplate)
		definition, hasTemplate := findSelectedDeviceTemplate(deviceTemplates, templateID)
		templateComplete := hasTemplate && deviceTemplateComplete(definition)
		if templateID == "" {
			missingTemplateMasters++
		} else if !templateComplete {
			invalidTemplateMasters++
		}

		deviceCountConfigured := master.DeviceCount > 0
		expectedDeviceCount := master.DeviceCount
		if deviceCountConfigured {
			expectedDeviceTotal += expectedDeviceCount
		} else {
			invalidDeviceCountMasters++
		}

		actualDeviceCount := deviceCountByMaster[master.MasterID]
		deviceCountMatched := deviceCountConfigured && actualDeviceCount == expectedDeviceCount
		if deviceCountConfigured && !deviceCountMatched {
			if actualDeviceCount > 0 {
				deviceCountMismatchMasters++
			} else {
				noDeviceMasters++
			}
		}

		if hasValidChannel && templateComplete && deviceCountMatched {
			closedMasters++
		}
	}

	// 根据闭合比例和阻断原因生成页面状态及提示。
	switch {
	case len(masters) == 0:
		overview.Status = "empty"
		overview.StatusText = "采集链路待配置"
		overview.StatusClass = "status-neutral"
		overview.Summary = "当前没有可采集主站，采集链路尚未形成。"
		overview.BlockingText = "当前没有配置主站，无法形成采集链路。"
	case enabledMasters == 0:
		overview.Status = "empty"
		overview.StatusText = "采集链路待配置"
		overview.StatusClass = "status-neutral"
		overview.Summary = "当前没有启用主站，采集链路尚未形成。"
		overview.BlockingText = "当前没有可采集对象，无法形成采集链路。"
	case len(enabledChannelIDs) == 0:
		overview.Status = "blocked"
		overview.StatusText = "采集链路未闭合"
		overview.StatusClass = "status-warn"
		overview.Summary = "当前没有启用通道，主站无法绑定有效采集入口。"
		if len(channels) == 0 {
			overview.BlockingText = "当前没有配置通道，无法形成采集链路。"
		} else {
			overview.BlockingText = "当前没有启用通道，无法形成采集链路。"
		}
	case closedMasters == enabledMasters:
		overview.Status = "complete"
		overview.StatusText = "采集链路已闭合"
		overview.StatusClass = "status-ok"
		overview.Summary = "通道、主站、设备类型与显式设备数量关系完整。"
		overview.BlockingText = ""
	case closedMasters > 0:
		overview.Status = "partial"
		overview.StatusText = "采集链路部分未闭合"
		overview.StatusClass = "status-warn"
		overview.Summary = fmt.Sprintf("%d 个主站已闭合，%d 个主站待处理。", closedMasters, enabledMasters-closedMasters)
		overview.BlockingText = collectionLinkBlockingText(missingChannelMasters, missingTemplateMasters, invalidTemplateMasters, invalidDeviceCountMasters, deviceCountMismatchMasters, noDeviceMasters)
	default:
		overview.Status = "blocked"
		overview.StatusText = "采集链路未闭合"
		overview.StatusClass = "status-warn"
		overview.Summary = "配置链路存在阻断，暂未形成完整采集链路。"
		overview.BlockingText = collectionLinkBlockingText(missingChannelMasters, missingTemplateMasters, invalidTemplateMasters, invalidDeviceCountMasters, deviceCountMismatchMasters, noDeviceMasters)
	}
	if expectedDeviceTotal > 0 && len(devices) == 0 && overview.BlockingText == "" {
		overview.BlockingText = "当前未生成可采集设备。"
	}
	return overview
}

// buildCollectionObjectOverview 聚合主站与设备的配置、运行状态和页面摘要。
func buildCollectionObjectOverview(
	link model.CollectionLinkOverview,
	channels []model.ChannelConfig,
	masters []model.MasterNodeConfig,
	devices []model.DeviceConfig,
	deviceTemplates []model.DeviceTemplateDefinition,
	systemStatus model.SystemStatus,
	statusReadable bool,
	configReadable bool,
) model.CollectionObjectOverview {
	overview := model.CollectionObjectOverview{
		Status:      link.Status,
		StatusText:  collectionObjectStatusText(link),
		StatusClass: collectionObjectStatusClass(link),
		ConfigHint:  collectionObjectConfigHint(link, configReadable),
	}

	// 建立运行状态和配置归属索引，避免聚合阶段重复遍历。
	deviceStatusByID := make(map[string]model.DeviceStatus, len(systemStatus.DeviceStatusList))
	for _, status := range systemStatus.DeviceStatusList {
		deviceStatusByID[status.DeviceID] = status
	}
	masterStatusByID := make(map[string]model.MasterNodeStatus, len(systemStatus.MasterStatusList))
	for _, status := range systemStatus.MasterStatusList {
		masterStatusByID[status.MasterID] = status
	}
	enabledChannelIDs := make(map[string]bool, len(channels))
	for _, channel := range channels {
		if strings.TrimSpace(channel.ChannelID) != "" && channel.Enabled {
			enabledChannelIDs[channel.ChannelID] = true
		}
	}
	devicesByMaster := make(map[string][]model.DeviceConfig)
	for _, device := range devices {
		devicesByMaster[device.MasterID] = append(devicesByMaster[device.MasterID], device)
	}

	// 汇总设备与主站状态，并仅保留前五个主站作为页面预览。
	masterTotal := len(masters)
	deviceTotal := len(devices)
	var normalMasters int
	var errorMasters int
	var normalDevices int
	var errorDevices int
	summaries := make([]model.CollectionMasterSummary, 0, minInt(len(masters), 5))

	for _, device := range devices {
		status, hasStatus := deviceStatusByID[device.DeviceID]
		switch classifyCollectionDevice(device, status, hasStatus, statusReadable) {
		case collectionObjectNormal:
			normalDevices++
		case collectionObjectIssue:
			errorDevices++
		}
	}

	for index, master := range masters {
		status, hasStatus := masterStatusByID[master.MasterID]
		masterDevices := devicesByMaster[master.MasterID]
		normalInMaster, errorInMaster := collectionDeviceCounts(masterDevices, deviceStatusByID, statusReadable)
		configComplete := collectionMasterConfigComplete(master, enabledChannelIDs, deviceTemplates, len(masterDevices))
		masterState := classifyCollectionMaster(master, status, hasStatus, statusReadable, configComplete)
		switch masterState {
		case collectionObjectNormal:
			normalMasters++
		case collectionObjectIssue:
			errorMasters++
		}

		if index < 5 {
			summaries = append(summaries, model.CollectionMasterSummary{
				Name:          collectionMasterName(master),
				AddressText:   fmt.Sprintf("主站地址 %d", master.TargetAddress),
				TemplateName:  collectionMasterTemplateName(master, deviceTemplates),
				DeviceCount:   len(masterDevices),
				NormalDevices: normalInMaster,
				ErrorDevices:  errorInMaster,
				StatusText:    collectionObjectStateText(masterState),
				StatusState:   collectionObjectStateClass(masterState),
				RegisterRange: collectionMasterRegisterRange(master),
			})
		}
	}
	if len(masters) > 5 {
		overview.MoreMasterCount = len(masters) - 5
	}

	// 生成统计指标和无数据时的引导文案。
	overview.Metrics = []model.CollectionObjectMetric{
		{Label: "主站总数", Value: fmt.Sprintf("%d 个", masterTotal), State: "neutral"},
		{Label: "正常主站", Value: fmt.Sprintf("%d 个", normalMasters), State: "ok"},
		{Label: "异常主站", Value: fmt.Sprintf("%d 个", errorMasters), State: collectionMetricIssueState(errorMasters)},
		{Label: "设备总数", Value: fmt.Sprintf("%d 台", deviceTotal), State: "neutral"},
		{Label: "正常设备", Value: fmt.Sprintf("%d 台", normalDevices), State: "ok"},
		{Label: "异常设备", Value: fmt.Sprintf("%d 台", errorDevices), State: collectionMetricIssueState(errorDevices)},
	}
	overview.MasterSummaries = summaries

	switch {
	case !configReadable:
		overview.EmptyText = "配置数据暂不可读，无法生成完整采集对象概览。"
	case len(masters) == 0:
		overview.EmptyText = "当前暂无主站配置，请先在采集管理的主站配置中创建主站，并绑定通道与设备类型。"
	case len(devices) == 0:
		overview.EmptyText = "当前暂无设备，请检查主站设备类型绑定和显式设备数量配置。"
	}
	return overview
}

func collectionObjectStatusText(link model.CollectionLinkOverview) string {
	switch link.Status {
	case "complete":
		return "配置链路完整"
	case "partial":
		return "配置部分未闭合"
	case "blocked":
		return "配置未闭合"
	default:
		return "对象待配置"
	}
}

func collectionObjectStatusClass(link model.CollectionLinkOverview) string {
	switch link.Status {
	case "complete":
		return "status-ok"
	case "partial", "blocked":
		return "status-warn"
	default:
		return "status-neutral"
	}
}

// collectionObjectConfigHint 生成采集对象的配置提示。
func collectionObjectConfigHint(link model.CollectionLinkOverview, configReadable bool) string {
	if !configReadable {
		return "配置数据暂不可读，无法生成完整采集对象概览。"
	}
	if strings.TrimSpace(link.BlockingText) != "" {
		return "配置链路未闭合：" + strings.TrimSuffix(link.BlockingText, "。") + "。"
	}
	if link.Status == "complete" {
		return "配置链路完整，主站显式设备数量与已生成设备一致。"
	}
	if strings.TrimSpace(link.Summary) != "" {
		return "配置提示：" + strings.TrimSuffix(link.Summary, "。") + "。"
	}
	return "配置提示：当前采集对象关系尚未完整。"
}

func collectionMetricIssueState(count int) string {
	if count > 0 {
		return "bad"
	}
	return "ok"
}

// collectionMasterConfigComplete 判断主站采集配置是否完整。
func collectionMasterConfigComplete(
	master model.MasterNodeConfig,
	enabledChannelIDs map[string]bool,
	deviceTemplates []model.DeviceTemplateDefinition,
	deviceCount int,
) bool {
	if !master.Enabled || !enabledChannelIDs[strings.TrimSpace(master.ChannelID)] {
		return false
	}
	definition, hasTemplate := findSelectedDeviceTemplate(deviceTemplates, strings.TrimSpace(master.DeviceTemplate))
	if !hasTemplate || !deviceTemplateComplete(definition) {
		return false
	}
	if master.DeviceCount <= 0 {
		return false
	}
	return deviceCount == master.DeviceCount
}

// classifyCollectionMaster 综合配置完整性和运行状态判定主站状态。
func classifyCollectionMaster(
	master model.MasterNodeConfig,
	status model.MasterNodeStatus,
	hasStatus bool,
	statusReadable bool,
	configComplete bool,
) collectionObjectState {
	if !master.Enabled {
		return collectionObjectDisabled
	}
	if statusReadable && hasStatus {
		if collectionMasterRuntimeHasIssue(status) {
			return collectionObjectIssue
		}
		if configComplete && status.Online && status.LastCollectSuccess {
			return collectionObjectNormal
		}
	}
	if !configComplete {
		return collectionObjectConfigIncomplete
	}
	return collectionObjectUnknown
}

// classifyCollectionDevice 综合启用状态和运行状态判定设备状态。
func classifyCollectionDevice(
	device model.DeviceConfig,
	status model.DeviceStatus,
	hasStatus bool,
	statusReadable bool,
) collectionObjectState {
	if !device.Enabled {
		return collectionObjectDisabled
	}
	if !statusReadable || !hasStatus {
		return collectionObjectUnknown
	}
	if collectionDiagnosisIsError(status.Diagnosis) {
		return collectionObjectIssue
	}
	quality := strings.ToLower(strings.TrimSpace(status.CommunicationQuality))
	if !status.Online ||
		!status.LastCollectSuccess ||
		status.LastFailureTimeMS > 0 && status.LastFailureTimeMS >= status.LastSuccessTimeMS ||
		strings.Contains(quality, "bad") ||
		strings.TrimSpace(status.LastErrorMessage) != "" {
		return collectionObjectIssue
	}
	return collectionObjectNormal
}

// collectionDiagnosisIsError 判断采集诊断是否属于错误级别。
func collectionDiagnosisIsError(diagnosis model.DiagnosisStatus) bool {
	if !diagnosisHasIssue(diagnosis) {
		return false
	}
	status := strings.ToLower(strings.TrimSpace(diagnosis.Status))
	if status == "error" || status == "critical" || status == "offline" || status == "failed" {
		return true
	}
	code := strings.ToLower(strings.TrimSpace(diagnosis.ErrorCode))
	return strings.Contains(code, "error") || strings.Contains(code, "failed") || strings.Contains(code, "timeout")
}

// collectionMasterRuntimeHasIssue 判断主站运行态是否存在问题。
func collectionMasterRuntimeHasIssue(status model.MasterNodeStatus) bool {
	if collectionDiagnosisIsError(status.Diagnosis) {
		return true
	}
	quality := strings.ToLower(strings.TrimSpace(status.CommunicationQuality))
	return !status.Online ||
		!status.LastCollectSuccess ||
		status.LastFailureTimeMS > 0 && status.LastFailureTimeMS >= status.LastSuccessTimeMS ||
		strings.Contains(quality, "bad") ||
		strings.TrimSpace(status.LastErrorMessage) != ""
}

// collectionDeviceCounts 统计符合条件的对象数量。
func collectionDeviceCounts(
	devices []model.DeviceConfig,
	statusByID map[string]model.DeviceStatus,
	statusReadable bool,
) (int, int) {
	var normalCount int
	var errorCount int
	for _, device := range devices {
		status, hasStatus := statusByID[device.DeviceID]
		switch classifyCollectionDevice(device, status, hasStatus, statusReadable) {
		case collectionObjectNormal:
			normalCount++
		case collectionObjectIssue:
			errorCount++
		}
	}
	return normalCount, errorCount
}

func collectionMasterName(master model.MasterNodeConfig) string {
	if strings.TrimSpace(master.MasterName) != "" {
		return master.MasterName
	}
	if master.TargetAddress > 0 {
		return fmt.Sprintf("主站地址 %d", master.TargetAddress)
	}
	return defaultString(master.MasterID, "未命名主站")
}

func collectionMasterTemplateName(master model.MasterNodeConfig, deviceTemplates []model.DeviceTemplateDefinition) string {
	templateID := strings.TrimSpace(master.DeviceTemplate)
	if templateID == "" {
		return "未选择设备类型"
	}
	if definition, ok := findSelectedDeviceTemplate(deviceTemplates, templateID); ok && deviceTemplateComplete(definition) {
		return defaultString(definition.DisplayName, definition.ID)
	}
	return "设备类型不可用"
}

func collectionMasterRegisterRange(master model.MasterNodeConfig) string {
	return fmt.Sprintf("基地址 %d", master.BlockStartRegister)
}

func collectionObjectStateText(state collectionObjectState) string {
	switch state {
	case collectionObjectDisabled:
		return "已禁用"
	case collectionObjectConfigIncomplete:
		return "配置不完整"
	case collectionObjectIssue:
		return "异常"
	case collectionObjectNormal:
		return "正常"
	default:
		return "状态未知"
	}
}

func collectionObjectStateClass(state collectionObjectState) string {
	switch state {
	case collectionObjectDisabled, collectionObjectUnknown:
		return "neutral"
	case collectionObjectConfigIncomplete:
		return "warn"
	case collectionObjectIssue:
		return "bad"
	case collectionObjectNormal:
		return "ok"
	default:
		return "neutral"
	}
}

// findSelectedDeviceTemplate 按标识查找已选择的设备类型。
func findSelectedDeviceTemplate(
	deviceTemplates []model.DeviceTemplateDefinition,
	templateID string,
) (model.DeviceTemplateDefinition, bool) {
	if templateID == "" {
		return model.DeviceTemplateDefinition{}, false
	}
	return model.FindDeviceTemplateIn(deviceTemplates, templateID)
}

// deviceTemplateComplete 判断设备类型定义是否完整。
func deviceTemplateComplete(definition model.DeviceTemplateDefinition) bool {
	if strings.TrimSpace(definition.ID) == "" || len(definition.Fields) == 0 {
		return false
	}
	if definition.DeviceAddressStride <= 0 || len(definition.ReadBlocks) == 0 {
		return false
	}
	blockRegisterCounts := make(map[string]uint64, len(definition.ReadBlocks))
	for _, block := range definition.ReadBlocks {
		blockKey := strings.TrimSpace(block.BlockKey)
		if blockKey == "" || block.RegisterCount <= 0 {
			return false
		}
		if _, exists := blockRegisterCounts[blockKey]; exists {
			return false
		}
		blockRegisterCounts[blockKey] = uint64(block.RegisterCount)
	}
	for _, field := range definition.Fields {
		if strings.TrimSpace(field.Key) == "" || field.RegisterCount == 0 {
			return false
		}
		blockKey := strings.TrimSpace(field.ReadBlockKey)
		if blockKey == "" {
			return false
		}
		blockRegisterCount, exists := blockRegisterCounts[blockKey]
		if !exists || uint64(field.RegisterOffset)+uint64(field.RegisterCount) > blockRegisterCount {
			return false
		}
	}
	return true
}

// collectionLinkBlockingText 汇总采集链路未闭合的主要原因。
func collectionLinkBlockingText(
	missingChannelMasters int,
	missingTemplateMasters int,
	invalidTemplateMasters int,
	invalidDeviceCountMasters int,
	deviceCountMismatchMasters int,
	noDeviceMasters int,
) string {
	switch {
	case missingChannelMasters > 0:
		return "存在主站未绑定有效通道。"
	case missingTemplateMasters > 0:
		return "存在主站未选择设备类型。"
	case invalidTemplateMasters > 0:
		return "设备类型缺失，无法生成设备。"
	case invalidDeviceCountMasters > 0:
		return "存在主站未配置有效的显式设备数量。"
	case deviceCountMismatchMasters > 0:
		return "已生成设备数量与主站显式设备数量不一致，请检查配置加载状态。"
	case noDeviceMasters > 0:
		return "当前未生成可采集设备。"
	default:
		return ""
	}
}

// registerRangeText 注册范围文本。
func registerRangeText(start uint16, count uint16) string {
	if count == 0 {
		return "-"
	}
	end := uint32(start) + uint32(count) - 1
	return fmt.Sprintf("%d ~ %d", start, end)
}

func (s *ConsoleService) LoadOverviewDiagnosis(ctx context.Context) model.OverviewDiagnosisResponse {
	response := model.OverviewDiagnosisResponse{
		BackendReachable: true,
	}

	// 原子快照同时包含系统状态和最近错误，高频刷新固定只发起一次 IPC。
	overviewSnapshot, snapshotErr := s.backend.GetSystemOverviewSnapshot(ctx)
	systemStatus := overviewSnapshot.SystemStatus
	recentError := overviewSnapshot.CurrentError
	statusErr := snapshotErr
	recentErr := snapshotErr
	response.Card = buildOverviewDiagnosisCard(
		recentError,
		recentErr == nil,
		systemStatus,
		statusErr == nil,
	)
	if statusErr != nil && recentErr != nil {
		response.BackendReachable = false
		response.ErrorMessage = "后端不可达，系统运行诊断暂时无法刷新"
		response.Card = model.OverviewDiagnosisCard{
			HasIssue:         true,
			StateText:        "后端不可达",
			StateClass:       "status-bad",
			CollectStateText: "未知",
			Note:             response.ErrorMessage,
		}
	}
	return response
}

// buildOverviewDiagnosisCard 根据最近错误和系统状态构建诊断卡片。
func buildOverviewDiagnosisCard(
	recentError model.ServiceErrorSummary,
	recentErrorAvailable bool,
	systemStatus model.SystemStatus,
	systemStatusAvailable bool,
) model.OverviewDiagnosisCard {
	if recentErrorAvailable && recentError.HasError {
		if card, ok := overviewIssueDiagnosisCard(recentError.Diagnosis, recentError.Message); ok {
			return card
		}
	}
	if systemStatusAvailable {
		if card, ok := overviewIssueDiagnosisCard(systemStatus.Diagnosis, systemStatus.LastPollCycleErrorMessage); ok {
			return card
		}
	}
	if !systemStatusAvailable {
		return model.OverviewDiagnosisCard{
			HasIssue:         false,
			StateText:        "状态未知",
			StateClass:       "status-warn",
			CollectStateText: "状态未知",
			Note:             "系统状态暂时无法获取。",
		}
	}

	card := model.OverviewDiagnosisCard{
		HasIssue:          false,
		StateText:         "运行正常",
		StateClass:        "status-ok",
		CollectStateText:  "轮询状态未知",
		LastSuccessTimeMS: latestOverviewSuccessTime(systemStatus),
		Note:              "当前未检测到采集异常。",
	}
	if systemStatusAvailable {
		card.CollectStateText = pollingStateLabel(systemStatus.PollingState, systemStatus.PollingRunning)
		if !systemStatus.ConfigLoaded {
			card.StateText = "配置待确认"
			card.StateClass = "status-warn"
			card.Note = "配置尚未完成加载，请检查系统配置和后端状态。"
		} else if !systemStatus.ServiceReady || !systemStatus.Running {
			card.StateText = "后端未就绪"
			card.StateClass = "status-warn"
			card.Note = "后端服务尚未进入可工作状态。"
		} else if !systemStatus.PollingRunning {
			card.StateText = "轮询未启动"
			card.StateClass = "status-neutral"
			card.Note = "当前未检测到采集异常，轮询服务尚未启动。"
		}
	}
	return card
}

// overviewIssueDiagnosisCard 将有效诊断转换为概览诊断卡片。
func overviewIssueDiagnosisCard(diagnosis model.DiagnosisStatus, fallbackMessage string) (model.OverviewDiagnosisCard, bool) {
	hasIssue := diagnosisHasIssue(diagnosis)
	message := diagnosisMessage(diagnosis)
	if !hasIssue && strings.TrimSpace(fallbackMessage) == "" {
		return model.OverviewDiagnosisCard{}, false
	}
	if message == "" {
		message = strings.TrimSpace(fallbackMessage)
	}
	if message == "" {
		message = defaultString(strings.TrimSpace(diagnosis.ErrorCode), "未分类异常")
	}
	suggestion := strings.TrimSpace(diagnosis.Suggestion)
	if suggestion == "" {
		suggestion = "请检查相关配置和现场连接状态。"
	}
	return model.OverviewDiagnosisCard{
		HasIssue:            true,
		StateText:           diagnosisStateText(diagnosis),
		StateClass:          diagnosisStateClass(diagnosis),
		CollectStateText:    "采集异常",
		TargetText:          diagnosisTargetText(diagnosis),
		TypeText:            message,
		ErrorCode:           strings.TrimSpace(diagnosis.ErrorCode),
		ConsecutiveFailures: diagnosis.ConsecutiveFailures,
		LastSuccessTimeMS:   diagnosis.LastSuccessTimeMS,
		LastErrorTimeMS:     diagnosis.LastErrorTimeMS,
		Suggestion:          suggestion,
		Note:                message,
	}, true
}

func diagnosisStateText(diagnosis model.DiagnosisStatus) string {
	switch strings.ToLower(strings.TrimSpace(diagnosis.Level)) {
	case "channel":
		return "通道异常"
	case "master":
		return "采集异常"
	case "device":
		return "设备异常"
	case "system":
		return "系统异常"
	default:
		return "采集异常"
	}
}

func diagnosisStateClass(diagnosis model.DiagnosisStatus) string {
	switch strings.ToLower(strings.TrimSpace(diagnosis.Status)) {
	case "warning":
		return "status-warn"
	case "normal", "":
		return "status-ok"
	default:
		return "status-bad"
	}
}

func diagnosisTargetText(diagnosis model.DiagnosisStatus) string {
	if target := strings.TrimSpace(diagnosis.TargetName); target != "" {
		return target
	}
	if target := strings.TrimSpace(diagnosis.TargetID); target != "" {
		return target
	}
	switch strings.ToLower(strings.TrimSpace(diagnosis.Level)) {
	case "channel":
		return "通道"
	case "master":
		return "主站"
	case "device":
		return "设备"
	case "system":
		return "系统"
	default:
		return "采集对象"
	}
}

func latestOverviewSuccessTime(status model.SystemStatus) uint64 {
	var latest uint64
	for _, master := range status.MasterStatusList {
		if master.LastSuccessTimeMS > latest {
			latest = master.LastSuccessTimeMS
		}
	}
	for _, device := range status.DeviceStatusList {
		if device.LastSuccessTimeMS > latest {
			latest = device.LastSuccessTimeMS
		}
	}
	return latest
}
