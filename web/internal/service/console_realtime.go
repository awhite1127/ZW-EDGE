package service

// 本文件把后端通道、主站、设备和实时值快照整理成页面层级视图；只做展示聚合，
// 不改变采集状态，也不主动打开或重建通信链路。

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"time"
	"unicode/utf16"

	"edge-web/internal/model"
)

// LoadRealtime 优先使用后端原子快照；部分接口失败时保留配置行并标记状态未知，
// 避免短暂 IPC 故障让设备从页面消失。
func (s *ConsoleService) LoadRealtime(ctx context.Context) model.RealtimeLoadResult {
	// 原子快照是实时页的主数据源，获取失败时直接返回不可达状态。
	snapshot, snapshotErr := s.backend.GetRealtimeViewSnapshot(ctx)
	if snapshotErr != nil {
		warning := "实时监控快照获取失败：" + snapshotErr.Error()
		dashboard := buildRealtimeDashboard(nil, nil, nil, model.SystemStatus{}, false, warning, false)
		return model.RealtimeLoadResult{
			WarningMessage:       warning,
			BackendReachable:     false,
			PrimaryDataAvailable: false,
			Dashboard:            dashboard,
		}
	}

	// 展开快照并补充设备类型定义，类型读取失败仅作为局部警告。
	devices := snapshot.Devices
	channels := snapshot.Channels
	masters := snapshot.Masters
	systemStatus := snapshot.SystemStatus
	realtimeList := snapshot.DeviceRealtimeSnapshots
	warning := ""
	reachable := true
	primaryAvailable := true
	deviceTemplates, summaryErr := s.realtimeDeviceTemplates(ctx, snapshot.DeviceTemplateGeneration)
	alarms, alarmsErr := s.backend.ListActiveAlarms(ctx)
	templatesAvailable := summaryErr == nil && len(deviceTemplates) > 0
	if summaryErr != nil {
		warning = joinPartialErrors(warning, partialError("设备类型获取", summaryErr))
	}

	// 为设备状态、实时值及关联配置建立索引。
	statusByID := make(map[string]model.DeviceStatus, len(systemStatus.DeviceStatusList))
	for _, status := range systemStatus.DeviceStatusList {
		statusByID[status.DeviceID] = status
	}

	realtimeByID := make(map[string]model.DeviceRealtimeSnapshot, len(realtimeList))
	for _, item := range realtimeList {
		if validRealtimeSnapshot(item) {
			realtimeByID[item.DeviceID] = item
		}
	}

	masterByID := make(map[string]model.MasterNodeConfig, len(masters))
	for _, master := range masters {
		masterByID[master.MasterID] = master
	}
	channelByID := make(map[string]model.ChannelConfig, len(channels))
	for _, channel := range channels {
		channelByID[channel.ChannelID] = channel
	}

	// 优先按设备配置构造行；配置为空时退回状态清单，避免丢失运行中设备。
	var rows []model.RealtimeRow
	rows = make([]model.RealtimeRow, 0, len(devices))
	for _, device := range devices {
		status, hasStatus := statusByID[device.DeviceID]
		snapshot, hasRealtime := realtimeByID[device.DeviceID]
		hasRealtime = realtimeSnapshotMatchesStatus(snapshot, hasRealtime, status, hasStatus)
		rows = append(rows, buildRealtimeRowFromConfig(device, status, hasStatus, snapshot, hasRealtime, masterByID, channelByID, deviceTemplates, templatesAvailable))
	}
	if len(rows) == 0 && len(systemStatus.DeviceStatusList) > 0 {
		rows = make([]model.RealtimeRow, 0, len(systemStatus.DeviceStatusList))
		for _, status := range systemStatus.DeviceStatusList {
			snapshot, hasRealtime := realtimeByID[status.DeviceID]
			hasRealtime = realtimeSnapshotMatchesStatus(snapshot, hasRealtime, status, true)
			rows = append(rows, buildRealtimeRowFromStatus(status, snapshot, hasRealtime, masterByID, channelByID, deviceTemplates, templatesAvailable))
		}
	}
	// 合并活动告警，并清理后端诊断中不适合直接展示的限制文本。
	if alarmsErr == nil {
		applyRealtimeAlarmDiagnostics(rows, alarms)
	} else {
		warning = joinPartialErrors(warning, partialError("当前告警获取", alarmsErr))
	}
	for index := range rows {
		rows[index].ErrorMessage = userVisibleAlarmLimitText(rows[index].ErrorMessage)
		rows[index].SummaryText = userVisibleAlarmLimitText(rows[index].SummaryText)
		rows[index].Diagnosis.Message = userVisibleAlarmLimitText(rows[index].Diagnosis.Message)
		rows[index].Diagnosis.Suggestion = userVisibleAlarmLimitText(rows[index].Diagnosis.Suggestion)
	}

	// 根据最终行状态补充页面级警告并生成看板摘要。
	if warning == "" && realtimeRowsHaveWarnings(rows) {
		warning = "存在设备暂无有效采集数据或采集失败"
	}

	return model.RealtimeLoadResult{
		Rows:                 rows,
		Dashboard:            buildRealtimeDashboard(rows, channels, masters, systemStatus, reachable, warning, primaryAvailable),
		WarningMessage:       warning,
		BackendReachable:     reachable,
		PrimaryDataAvailable: primaryAvailable,
	}
}

// realtimeDeviceTemplates 复用低频变化的类型定义；后端 registry generation 是唯一失效信号。
func (s *ConsoleService) realtimeDeviceTemplates(ctx context.Context, generation uint64) ([]model.DeviceTemplateDefinition, error) {
	s.realtimeTemplateMu.RLock()
	if generation != 0 && s.realtimeTemplateGeneration == generation && len(s.realtimeTemplates) > 0 {
		templates := s.realtimeTemplates
		s.realtimeTemplateMu.RUnlock()
		return templates, nil
	}
	s.realtimeTemplateMu.RUnlock()

	s.realtimeTemplateMu.Lock()
	defer s.realtimeTemplateMu.Unlock()
	if generation != 0 && s.realtimeTemplateGeneration == generation && len(s.realtimeTemplates) > 0 {
		return s.realtimeTemplates, nil
	}
	summary, err := s.backend.GetConfigSummary(ctx)
	if err != nil {
		return nil, err
	}
	s.realtimeTemplates = summary.DeviceTemplates
	s.realtimeTemplateGeneration = generation
	return s.realtimeTemplates, nil
}

// realtimeRowsHaveWarnings 判断实时数据行中是否存在错误或缺失值。
func realtimeRowsHaveWarnings(rows []model.RealtimeRow) bool {
	for _, row := range rows {
		if row.ErrorMessage != "" || !row.HasRealtime {
			return true
		}
	}
	return false
}

// validRealtimeSnapshot 判断设备实时快照是否包含有效采样。
func validRealtimeSnapshot(snapshot model.DeviceRealtimeSnapshot) bool {
	if snapshot.SampleTimeMS == 0 {
		return false
	}
	if len(snapshot.Points) > 0 {
		return true
	}
	return snapshot.HasResistance &&
		snapshot.Resistance != nil &&
		snapshot.Resistance.SampleTimeMS > 0
}

// realtimeSnapshotMatchesStatus 汇总并返回当前展示状态。
func realtimeSnapshotMatchesStatus(
	snapshot model.DeviceRealtimeSnapshot,
	hasRealtime bool,
	status model.DeviceStatus,
	hasStatus bool,
) bool {
	if !hasRealtime || !validRealtimeSnapshot(snapshot) {
		return false
	}
	if !hasStatus {
		return false
	}
	if status.UpdatedAtMS > 0 && snapshot.SampleTimeMS >= status.UpdatedAtMS {
		return true
	}
	return status.LastCollectSuccess &&
		status.LastSuccessTimeMS > 0 &&
		snapshot.SampleTimeMS >= status.LastSuccessTimeMS
}

// BuildRealtimeResponse 构建实时数据响应。
func (s *ConsoleService) BuildRealtimeResponse(ctx context.Context) (model.RealtimeViewResponse, error) {
	result := s.LoadRealtime(ctx)
	response := model.RealtimeViewResponse{
		Rows:             result.Rows,
		Dashboard:        result.Dashboard,
		RefreshedAt:      time.Now().Format("2006-01-02 15:04:05"),
		BackendReachable: result.BackendReachable,
		ErrorMessage:     result.WarningMessage,
	}

	if !result.PrimaryDataAvailable {
		response.ErrorMessage = loadPrimaryDataErrorMessage(result.WarningMessage)
	}

	return response, nil
}

// buildRealtimeDashboard 构建实时数据看板。
func buildRealtimeDashboard(
	rows []model.RealtimeRow,
	channels []model.ChannelConfig,
	masters []model.MasterNodeConfig,
	systemStatus model.SystemStatus,
	reachable bool,
	warning string,
	primaryAvailable bool,
) model.RealtimeDashboard {
	channelItems := buildRealtimeChannelItems(rows, channels, masters, systemStatus, reachable)
	masterItems := buildRealtimeMasterItems(rows, masters, systemStatus, reachable)
	dashboard := model.RealtimeDashboard{
		SystemCore: model.RealtimeSystemCore{
			BackendStateText: backendStateText(reachable),
			PollingStateText: pollingStateLabel(systemStatus.PollingState, systemStatus.PollingRunning),
			LinkStateText:    realtimeLinkStateText(reachable, systemStatus, warning),
			StateClass:       realtimeStateClass(reachable, systemStatus, warning),
			Diagnosis:        systemStatus.Diagnosis,
			LastErrorText:    realtimeLastErrorText(systemStatus, warning),
		},
		Channels:            channelItems,
		Masters:             masterItems,
		ChannelSummary:      buildRealtimeLayerSummary(channelItems, "通道"),
		MasterSummary:       buildRealtimeLayerSummary(masterItems, "主站"),
		DeviceSummary:       buildRealtimeDeviceSummary(rows, reachable, primaryAvailable),
		EmptyStateText:      realtimeEmptyStateText(rows, reachable, primaryAvailable, systemStatus),
		TableStatusText:     realtimeTableStatusText(reachable, warning),
		AutoRefreshText:     "自动刷新",
		LatestRefreshStatus: "周期 10 秒",
	}
	return dashboard
}

// buildRealtimeLayerSummary 汇总并返回当前展示状态。
func buildRealtimeLayerSummary(items []model.RealtimeLayerItem, label string) model.RealtimeLayerSummary {
	summary := model.RealtimeLayerSummary{TotalCount: len(items)}
	for _, item := range items {
		if item.StateClass == "status-ok" {
			summary.OnlineCount++
		} else {
			summary.AbnormalCount++
		}
	}
	summary.Text = fmt.Sprintf("%s：共 %d 个，在线 %d 个，异常 %d 个", label, summary.TotalCount, summary.OnlineCount, summary.AbnormalCount)
	return summary
}

// applyRealtimeAlarmDiagnostics 将活动告警诊断补充到实时数据行。
func applyRealtimeAlarmDiagnostics(rows []model.RealtimeRow, alarms []model.ActiveAlarm) {
	byDevice := make(map[string][]model.ActiveAlarm, len(alarms))
	for _, alarm := range alarms {
		byDevice[alarm.DeviceID] = append(byDevice[alarm.DeviceID], alarm)
	}
	for index := range rows {
		deviceAlarms := byDevice[rows[index].DeviceID]
		if len(deviceAlarms) == 0 {
			continue
		}
		rows[index].Diagnosis.ErrorCode = "data_alarm"
		if rows[index].ErrorMessage != "" || !rows[index].Online {
			continue
		}
		alarm := deviceAlarms[0]
		direction := "高于上限"
		if strings.EqualFold(alarm.Direction, "low") {
			direction = "低于下限"
		}
		rows[index].ErrorMessage = fmt.Sprintf("%s %s，%s %s", displayDataItemName(alarm.PointName, alarm.PointKey), formatHistoryValue(alarm.CurrentValue, alarm.Precision, alarm.Unit), direction, formatHistoryValue(alarm.ThresholdValue, alarm.Precision, alarm.Unit))
		rows[index].Diagnosis.Level = normalizeAlarmLevel(alarm.Level)
		rows[index].Diagnosis.Status = "warning"
		rows[index].Diagnosis.Message = rows[index].ErrorMessage
		rows[index].Diagnosis.Suggestion = "建议检查接地回路、设备状态或调整告警阈值。"
	}
}

// realtimeLinkStateText 综合后端和轮询状态返回采集链路说明。
func realtimeLinkStateText(reachable bool, status model.SystemStatus, warning string) string {
	if !reachable {
		return "后端不可达"
	}
	if status.PollingState == "config_applying" {
		return "配置应用中"
	}
	if status.PollingState == "polling_rebuilding" {
		return "轮询重建中"
	}
	if status.LastPollCycleHasError || warning != "" {
		return "采集链路存在异常"
	}
	if status.PollingRunning {
		return "采集链路运行中"
	}
	return "轮询未启动"
}

// realtimeStateClass 返回实时链路状态的样式类名。
func realtimeStateClass(reachable bool, status model.SystemStatus, warning string) string {
	if !reachable || status.LastPollCycleHasError {
		return "status-bad"
	}
	if warning != "" || status.PollingState == "config_applying" || status.PollingState == "polling_rebuilding" {
		return "status-warn"
	}
	if status.PollingRunning {
		return "status-ok"
	}
	return "status-neutral"
}

// realtimeLastErrorText 按优先级返回最近的有效诊断或错误信息。
func realtimeLastErrorText(status model.SystemStatus, warning string) string {
	if message := diagnosisMessage(status.Diagnosis); message != "" {
		return message
	}
	if status.LastPollCycleErrorMessage != "" {
		return status.LastPollCycleErrorMessage
	}
	if warning != "" {
		return warning
	}
	return "暂无最近错误"
}

// 通道和主站摘要基于完整配置清单聚合，而不是仅统计当前有实时值的设备。
func buildRealtimeChannelItems(
	rows []model.RealtimeRow,
	channels []model.ChannelConfig,
	masters []model.MasterNodeConfig,
	systemStatus model.SystemStatus,
	reachable bool,
) []model.RealtimeLayerItem {
	// 先按通道统计设备总数和在线数。
	deviceTotalByChannel := make(map[string]int, len(channels))
	deviceOnlineByChannel := make(map[string]int, len(channels))
	for _, row := range rows {
		channelID := row.ChannelID
		if channelID == "" {
			channelID = "unknown"
		}
		deviceTotalByChannel[channelID]++
		if row.HasStatus && row.Online {
			deviceOnlineByChannel[channelID]++
		}
	}

	channelStatusByID := make(map[string]model.ChannelStatus, len(systemStatus.ChannelStatusList))
	for _, status := range systemStatus.ChannelStatusList {
		channelStatusByID[status.ChannelID] = status
	}

	// 配置清单缺失时从主站引用恢复最小通道列表。
	if len(channels) == 0 {
		channelIDs := make(map[string]bool, len(masters))
		for _, master := range masters {
			if master.ChannelID != "" {
				channelIDs[master.ChannelID] = true
			}
		}
		for id := range channelIDs {
			channels = append(channels, model.ChannelConfig{ChannelID: id, ChannelName: id, Enabled: true})
		}
	}

	// 合并配置与运行状态，生成每个通道的展示状态。
	items := make([]model.RealtimeLayerItem, 0, len(channels))
	for _, channel := range channels {
		status, hasStatus := channelStatusByID[channel.ChannelID]
		stateText := "状态未知"
		stateClass := "status-neutral"
		errorMessage := ""
		diagnosis := model.DiagnosisStatus{}
		diagnosisError := ""
		if hasStatus {
			diagnosis = status.Diagnosis
			diagnosisError = diagnosisMessage(status.Diagnosis)
		}
		switch {
		case !reachable:
			stateText = "后端不可达"
			stateClass = "status-bad"
		case !channel.Enabled:
			stateText = "通道未打开"
			stateClass = "status-neutral"
		case hasStatus && (diagnosisError != "" || status.LastErrorMessage != ""):
			stateText = "通道异常"
			stateClass = "status-bad"
			errorMessage = defaultString(diagnosisError, status.LastErrorMessage)
		case hasStatus && status.Opened:
			stateText = "通道在线"
			stateClass = "status-ok"
		case hasStatus:
			stateText = "通道未打开"
			stateClass = "status-warn"
		}
		items = append(items, model.RealtimeLayerItem{
			ID:           channel.ChannelID,
			Name:         defaultString(channel.ChannelName, channel.ChannelID),
			OnlineCount:  deviceOnlineByChannel[channel.ChannelID],
			TotalCount:   deviceTotalByChannel[channel.ChannelID],
			StateText:    stateText,
			StateClass:   stateClass,
			Diagnosis:    diagnosis,
			ErrorMessage: errorMessage,
		})
	}
	return items
}

// buildRealtimeMasterItems 构建页面或接口使用的结果列表。
func buildRealtimeMasterItems(
	rows []model.RealtimeRow,
	masters []model.MasterNodeConfig,
	systemStatus model.SystemStatus,
	reachable bool,
) []model.RealtimeLayerItem {
	totalByMaster := make(map[string]int, len(masters))
	onlineByMaster := make(map[string]int, len(masters))
	for _, row := range rows {
		totalByMaster[row.MasterID]++
		if row.HasStatus && row.Online {
			onlineByMaster[row.MasterID]++
		}
	}

	statusByID := make(map[string]model.MasterNodeStatus, len(systemStatus.MasterStatusList))
	for _, status := range systemStatus.MasterStatusList {
		statusByID[status.MasterID] = status
	}

	if len(masters) == 0 {
		seen := make(map[string]bool, len(rows))
		for _, row := range rows {
			if row.MasterID != "" && !seen[row.MasterID] {
				seen[row.MasterID] = true
				masters = append(masters, model.MasterNodeConfig{MasterID: row.MasterID, MasterName: row.MasterName, Enabled: true})
			}
		}
	}

	items := make([]model.RealtimeLayerItem, 0, len(masters))
	for _, master := range masters {
		status, hasStatus := statusByID[master.MasterID]
		stateText := "状态未知"
		stateClass := "status-neutral"
		errorMessage := ""
		diagnosis := model.DiagnosisStatus{}
		diagnosisError := ""
		if hasStatus {
			diagnosis = status.Diagnosis
			diagnosisError = diagnosisMessage(status.Diagnosis)
		}
		switch {
		case !reachable:
			stateText = "后端不可达"
			stateClass = "status-bad"
		case !master.Enabled:
			stateText = "已禁用"
			stateClass = "status-neutral"
		case hasStatus && (diagnosisError != "" || status.LastErrorMessage != ""):
			stateText = "主站异常"
			stateClass = "status-bad"
			errorMessage = defaultString(diagnosisError, status.LastErrorMessage)
		case hasStatus && status.Online && status.LastCollectSuccess:
			stateText = "主站在线"
			stateClass = "status-ok"
		case hasStatus:
			stateText = "未采集"
			stateClass = "status-warn"
		}
		items = append(items, model.RealtimeLayerItem{
			ID:           master.MasterID,
			Name:         defaultString(master.MasterName, master.MasterID),
			OnlineCount:  onlineByMaster[master.MasterID],
			TotalCount:   totalByMaster[master.MasterID],
			StateText:    stateText,
			StateClass:   stateClass,
			Diagnosis:    diagnosis,
			ErrorMessage: errorMessage,
		})
	}
	return items
}

// buildRealtimeDeviceSummary 构建实时数据设备摘要。
func buildRealtimeDeviceSummary(rows []model.RealtimeRow, reachable bool, primaryAvailable bool) model.RealtimeDeviceSummary {
	// 统计在线、实时值和报警设备，并保留首个有效诊断。
	total := len(rows)
	online := 0
	errorMessage := ""
	diagnosis := model.DiagnosisStatus{}
	hasRealtime := false
	alarmDevices := 0
	alarmDeviceNames := make([]string, 0, 3)
	seenAlarmDevices := make(map[string]struct{}, len(rows))
	for _, row := range rows {
		if row.HasStatus && row.Online {
			online++
		}
		if row.HasRealtime {
			hasRealtime = true
		}
		if row.Diagnosis.ErrorCode == "data_alarm" {
			key := strings.TrimSpace(row.DeviceID)
			if key == "" {
				key = strings.TrimSpace(row.DeviceName)
			}
			if _, exists := seenAlarmDevices[key]; !exists {
				seenAlarmDevices[key] = struct{}{}
				alarmDevices++
				name := strings.TrimSpace(row.DeviceName)
				if name == "" {
					name = strings.TrimSpace(row.DeviceID)
				}
				if name == "" {
					name = "未知设备"
				}
				alarmDeviceNames = append(alarmDeviceNames, name)
			}
		}
		if !diagnosisHasIssue(diagnosis) && diagnosisHasIssue(row.Diagnosis) {
			diagnosis = row.Diagnosis
		}
		if errorMessage == "" {
			errorMessage = defaultString(diagnosisMessage(row.Diagnosis), row.ErrorMessage)
		}
	}
	// 先构造正常态摘要，再按不可达、无数据、报警和离线顺序降级。
	summary := model.RealtimeDeviceSummary{
		OnlineCount:        online,
		OfflineCount:       total - online,
		AlarmCount:         alarmDevices,
		AlarmDeviceNames:   alarmDeviceNames,
		AlarmDevicePreview: append([]string(nil), alarmDeviceNames[:minInt(len(alarmDeviceNames), 3)]...),
		AlarmDeviceText:    alarmDeviceDisplayText(alarmDeviceNames, alarmDevices),
		TotalCount:         total,
		StateText:          "设备正常",
		StateClass:         "status-ok",
		DetailText:         "实时数据持续更新",
		Diagnosis:          diagnosis,
		ErrorMessage:       errorMessage,
	}
	switch {
	case !reachable:
		summary.StateText = "状态不可确认"
		summary.StateClass = "status-bad"
		summary.DetailText = "后端服务未连接"
	case !primaryAvailable:
		summary.StateText = "数据不可用"
		summary.StateClass = "status-bad"
		summary.DetailText = "实时数据暂时无法获取"
	case total == 0:
		summary.StateText = "暂无采集设备"
		summary.StateClass = "status-neutral"
		summary.DetailText = "请到采集管理配置通道、主站和设备类型"
	case !hasRealtime:
		summary.StateText = "暂无实时数据"
		summary.StateClass = "status-warn"
		summary.DetailText = "等待轮询采集产生有效值"
	case alarmDevices > 0:
		summary.StateText = "存在报警设备"
		summary.StateClass = "status-warn"
		summary.DetailText = fmt.Sprintf("%d 台设备正在报警", alarmDevices)
	case online < total:
		summary.StateText = "部分设备异常"
		summary.StateClass = "status-warn"
		summary.DetailText = "存在离线或未更新设备"
	}
	return summary
}

// alarmDeviceDisplayText 汇总报警设备名称并限制预览数量。
func alarmDeviceDisplayText(names []string, total int) string {
	if total <= 0 || len(names) == 0 {
		return "当前暂无报警"
	}
	limit := minInt(len(names), 3)
	visible := strings.Join(names[:limit], "、")
	if total > limit {
		return fmt.Sprintf("%s 等 %d 台设备报警", visible, total)
	}
	return "报警设备：" + visible
}

// realtimeEmptyStateText 根据数据可用性返回实时页空状态说明。
func realtimeEmptyStateText(rows []model.RealtimeRow, reachable bool, primaryAvailable bool, status model.SystemStatus) string {
	if !reachable {
		return "当前没有实时数据（后端不可达）"
	}
	if !primaryAvailable {
		return "实时数据暂时无法获取"
	}
	if len(rows) == 0 {
		return "暂无采集设备，请到采集管理中配置通道、主站和设备类型"
	}
	if status.PollingState == "config_applying" {
		return "配置应用中，实时数据暂未刷新"
	}
	if status.PollingState == "polling_rebuilding" {
		return "轮询重建中，实时数据暂未刷新"
	}
	if !status.PollingRunning {
		return "轮询未启动，暂无实时数据"
	}
	return "暂无实时数据"
}

// realtimeTableStatusText 返回实时数据表当前刷新状态。
func realtimeTableStatusText(reachable bool, warning string) string {
	if !reachable {
		return "后端不可达"
	}
	if warning != "" {
		return "降级展示"
	}
	return "实时刷新正常"
}

// StartPolling 启动后端轮询服务。
func (s *ConsoleService) StartPolling(ctx context.Context) model.ActionFeedback {
	summary, err := s.backend.StartPolling(ctx)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	return model.ActionFeedback{
		Success: true,
		Message: fmt.Sprintf("轮询已启动，当前状态：%s", pollingStateText(summary.PollingRunning)),
	}
}

// StopPolling 停止后端轮询服务。
func (s *ConsoleService) StopPolling(ctx context.Context) model.ActionFeedback {
	summary, err := s.backend.StopPolling(ctx)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	return model.ActionFeedback{
		Success: true,
		Message: fmt.Sprintf("轮询已停止，当前状态：%s", pollingStateText(summary.PollingRunning)),
	}
}

// 配置行是实时页面的稳定基线；状态与快照只能覆盖运行字段，不能改变配置层级和名称。
func buildRealtimeRowFromConfig(
	device model.DeviceConfig,
	status model.DeviceStatus,
	hasStatus bool,
	snapshot model.DeviceRealtimeSnapshot,
	hasRealtime bool,
	masterByID map[string]model.MasterNodeConfig,
	channelByID map[string]model.ChannelConfig,
	deviceTemplates []model.DeviceTemplateDefinition,
	templatesAvailable bool,
) model.RealtimeRow {
	master := masterByID[device.MasterID]
	channel := channelByID[master.ChannelID]
	templateID, templateName, templateFields, groupingEnabled, realtimeGroups := realtimeTemplateInfo(master, snapshot, hasRealtime, deviceTemplates, templatesAvailable)
	rowStableKey := realtimeDOMKey("row", master.ChannelID, device.MasterID, device.DeviceID)
	row := model.RealtimeRow{
		DeviceID:                device.DeviceID,
		StableKey:               rowStableKey,
		SummaryKey:              realtimeDOMKey("summary", master.ChannelID, device.MasterID, device.DeviceID),
		DeviceName:              device.DeviceName,
		ChannelID:               master.ChannelID,
		ChannelName:             defaultString(channel.ChannelName, master.ChannelID),
		MasterID:                device.MasterID,
		MasterName:              defaultString(master.MasterName, device.MasterID),
		TemplateID:              templateID,
		TemplateName:            templateName,
		TemplateFields:          templateFields,
		RealtimeGroupingEnabled: groupingEnabled,
		HasStatus:               hasStatus,
		HasRealtime:             hasRealtime,
		Online:                  hasStatus && status.Online,
		UpdatedAtMS:             status.UpdatedAtMS,
		Diagnosis:               status.Diagnosis,
		ErrorMessage:            "",
		CommunicationQuality:    qualityText(status.CommunicationQuality),
		SummaryText:             "暂无数据",
		ResistanceText:          "暂无数据",
	}

	if hasRealtime {
		row.UpdatedAtMS = maxUint64(row.UpdatedAtMS, snapshot.SampleTimeMS)
		row.CommunicationQuality = qualityText(chooseString(snapshot.CommunicationQuality, row.CommunicationQuality))
		row.SummaryPoints = buildRealtimeSummaryPointsForFields(snapshot, templateFields)
		for index := range row.SummaryPoints {
			row.SummaryPoints[index].StableKey = realtimeDOMKey(
				"point",
				master.ChannelID,
				device.MasterID,
				device.DeviceID,
				row.SummaryPoints[index].Key,
			)
		}
		row.RealtimeGroups = buildRealtimePointGroups(row.SummaryPoints, templateFields, groupingEnabled, realtimeGroups)
		row.SummaryText = realtimeSummaryText(row.SummaryPoints)
		row.ResistanceText = pointText(snapshot.Resistance, snapshot.HasResistance)
	}

	if !hasStatus {
		row.CommunicationQuality = "暂无数据"
		return row
	}

	if status.UpdatedAtMS == 0 && status.LastFailureTimeMS == 0 && status.LastSuccessTimeMS == 0 {
		row.HasStatus = false
		row.CommunicationQuality = "暂无数据"
		return row
	}

	diagnosisError := diagnosisMessage(status.Diagnosis)
	if !status.LastCollectSuccess {
		row.ErrorMessage = firstSpecificRealtimeRowMessage(status.LastErrorMessage, diagnosisError)
		if row.CommunicationQuality == "" || row.CommunicationQuality == "-" || row.CommunicationQuality == "未知" {
			row.CommunicationQuality = "异常"
		}
		return row
	}

	if !status.Online {
		row.ErrorMessage = firstSpecificRealtimeRowMessage(diagnosisError, status.LastErrorMessage, "设备离线")
	}

	return row
}

// realtimeDOMKey 使用带长度边界的结构化 key，避免合法 ID 内含 "::" 时互相碰撞。
func realtimeDOMKey(kind string, parts ...string) string {
	values := append([]string{kind}, parts...)
	var builder strings.Builder
	for _, value := range values {
		fmt.Fprintf(&builder, "%d:%s|", len(utf16.Encode([]rune(value))), value)
	}
	return builder.String()
}

// firstSpecificRealtimeRowMessage 返回首条非通用的设备诊断信息。
func firstSpecificRealtimeRowMessage(messages ...string) string {
	for _, message := range messages {
		text := strings.TrimSpace(message)
		if text != "" && !isGenericRealtimeRowMessage(text) {
			return text
		}
	}
	return ""
}

// isGenericRealtimeRowMessage 判断设备信息是否属于无细节的通用提示。
func isGenericRealtimeRowMessage(message string) bool {
	switch strings.TrimSpace(message) {
	case "存在设备暂无有效采集数据或采集失败",
		"暂无有效采集数据",
		"暂无有效采集值",
		"暂无采集记录",
		"采集失败",
		"暂无数据":
		return true
	default:
		return false
	}
}

// buildRealtimeRowFromStatus 汇总并返回当前展示状态。
func buildRealtimeRowFromStatus(
	status model.DeviceStatus,
	snapshot model.DeviceRealtimeSnapshot,
	hasRealtime bool,
	masterByID map[string]model.MasterNodeConfig,
	channelByID map[string]model.ChannelConfig,
	deviceTemplates []model.DeviceTemplateDefinition,
	templatesAvailable bool,
) model.RealtimeRow {
	return buildRealtimeRowFromConfig(
		model.DeviceConfig{
			DeviceID:   status.DeviceID,
			DeviceName: status.DeviceName,
			MasterID:   status.MasterID,
		},
		status,
		true,
		snapshot,
		hasRealtime,
		masterByID,
		channelByID,
		deviceTemplates,
		templatesAvailable,
	)
}

// realtimeTemplateInfo 汇总并返回当前展示状态。
func realtimeTemplateInfo(
	master model.MasterNodeConfig,
	snapshot model.DeviceRealtimeSnapshot,
	hasRealtime bool,
	deviceTemplates []model.DeviceTemplateDefinition,
	templatesAvailable bool,
) (string, string, []model.DeviceTemplateField, bool, []model.DeviceTemplateRealtimeGroup) {
	templateID := strings.TrimSpace(master.DeviceTemplate)
	templateName := ""
	if hasRealtime {
		if snapshot.TemplateID != "" {
			templateID = strings.TrimSpace(snapshot.TemplateID)
		}
		templateName = strings.TrimSpace(snapshot.TemplateName)
	}

	var fields []model.DeviceTemplateField
	groupingEnabled := false
	var realtimeGroups []model.DeviceTemplateRealtimeGroup
	if templatesAvailable && templateID != "" {
		if definition, ok := model.FindDeviceTemplateIn(deviceTemplates, templateID); ok {
			fields = definition.Fields
			groupingEnabled = definition.RealtimeGroupingEnabled
			realtimeGroups = definition.RealtimeGroups
			if templateName == "" {
				templateName = strings.TrimSpace(definition.DisplayName)
			}
		}
	}
	if templateName == "" {
		templateName = defaultString(templateID, "设备类型信息不可用")
	}
	return templateID, templateName, fields, groupingEnabled, realtimeGroups
}

// buildRealtimePointGroups 构建页面或接口使用的结果列表。
func buildRealtimePointGroups(
	points []model.RealtimePointRow,
	fields []model.DeviceTemplateField,
	enabled bool,
	groups []model.DeviceTemplateRealtimeGroup,
) []model.RealtimePointGroup {
	if !enabled || len(points) == 0 || len(groups) == 0 {
		return nil
	}
	ordered := append([]model.DeviceTemplateRealtimeGroup(nil), groups...)
	sort.SliceStable(ordered, func(left, right int) bool {
		if ordered[left].Order != ordered[right].Order {
			return ordered[left].Order < ordered[right].Order
		}
		return ordered[left].ID < ordered[right].ID
	})
	groupByField := make(map[string]string, len(fields))
	for _, field := range fields {
		groupByField[strings.TrimSpace(field.Key)] = strings.TrimSpace(field.RealtimeGroupID)
	}
	pointsByGroup := make(map[string][]model.RealtimePointRow, len(ordered))
	for _, point := range points {
		groupID := groupByField[strings.TrimSpace(point.Key)]
		if groupID != "" {
			pointsByGroup[groupID] = append(pointsByGroup[groupID], point)
		}
	}
	result := make([]model.RealtimePointGroup, 0, len(ordered))
	for _, group := range ordered {
		groupPoints := pointsByGroup[group.ID]
		if len(groupPoints) == 0 {
			continue
		}
		result = append(result, model.RealtimePointGroup{
			ID: group.ID, Name: group.Name, Order: group.Order, Points: groupPoints,
		})
	}
	return result
}

// buildChannelSerialBindings 构建页面或接口使用的结果列表。
func buildChannelSerialBindings(
	channels []model.ChannelConfig,
	serialPorts []model.SerialPortInfo,
) map[string]model.ChannelSerialBindingView {
	bindings := make(map[string]model.ChannelSerialBindingView, len(channels))
	for _, channel := range channels {
		bindings[channel.ChannelID] = buildChannelSerialBinding(channel, serialPorts)
	}

	return bindings
}

// 串口候选项只提供诊断信息。即使当前绑定未被扫描发现，也必须保留配置值供用户识别。
func buildChannelSerialBinding(
	channel model.ChannelConfig,
	serialPorts []model.SerialPortInfo,
) model.ChannelSerialBindingView {
	currentPath := configuredChannelPort(channel)
	binding := model.ChannelSerialBindingView{
		CurrentPortPath: currentPath,
		Candidates:      make([]model.ChannelSerialOption, 0, len(serialPorts)),
	}

	for _, port := range serialPorts {
		option := model.ChannelSerialOption{
			Path:          port.Path,
			DisplayName:   port.DisplayName,
			Kind:          port.Kind,
			Available:     port.Available,
			Busy:          port.Busy,
			Description:   port.Description,
			SymlinkByID:   port.SymlinkByID,
			SymlinkByPath: port.SymlinkByPath,
			Driver:        port.Driver,
			PhysicalHint:  port.PhysicalHint,
			IsCurrent:     currentPath != "" && port.Path == currentPath,
		}

		if option.IsCurrent {
			binding.CurrentPortFound = true
		}

		binding.Candidates = append(binding.Candidates, option)
	}

	return binding
}

// buildRealtimeSummaryPoints 构建设备摘要中的关键数据点位。
func buildRealtimeSummaryPoints(snapshot model.DeviceRealtimeSnapshot) []model.RealtimePointRow {
	return buildRealtimePointRows(snapshot, nil, true)
}

// buildRealtimeSummaryPointsForFields 按字段实时展示偏好构建实时监控点位。
func buildRealtimeSummaryPointsForFields(snapshot model.DeviceRealtimeSnapshot, fields []model.DeviceTemplateField) []model.RealtimePointRow {
	return buildRealtimePointRows(snapshot, fields, false)
}

func buildRealtimePointRows(snapshot model.DeviceRealtimeSnapshot, fields []model.DeviceTemplateField, summaryOnly bool) []model.RealtimePointRow {
	points := snapshot.Points
	if len(points) == 0 && snapshot.HasResistance && snapshot.Resistance != nil {
		points = []model.PointValue{*snapshot.Resistance}
	}
	if len(points) == 0 {
		return nil
	}

	sorted := append([]model.PointValue(nil), points...)
	sort.SliceStable(sorted, func(left, right int) bool {
		if sorted[left].DisplayOrder != sorted[right].DisplayOrder {
			return sorted[left].DisplayOrder < sorted[right].DisplayOrder
		}
		return pointKey(sorted[left]) < pointKey(sorted[right])
	})

	selected := make([]model.PointValue, 0, len(sorted))
	if !summaryOnly {
		displayByKey := make(map[string]bool, len(fields))
		for _, field := range fields {
			displayByKey[strings.TrimSpace(field.Key)] = field.ShowInRealtime
		}
		for _, point := range sorted {
			if displayByKey[strings.TrimSpace(pointKey(point))] {
				selected = append(selected, point)
			}
		}
	} else {
		for _, point := range sorted {
			if point.Summary {
				selected = append(selected, point)
			}
		}
	}
	if summaryOnly {
		limit := 3
		if strings.EqualFold(snapshot.TemplateID, model.EM100InsulationMonitorDeviceTemplateID) {
			limit = 5
		}
		if len(selected) > limit {
			selected = selected[:limit]
		}
	}

	rows := make([]model.RealtimePointRow, 0, len(selected))
	for _, point := range selected {
		text := pointTextValueForTemplate(snapshot.TemplateID, snapshot.TemplateName, point)
		valueText := text
		unit := ""
		unitSuffix := " " + strings.TrimSpace(point.Unit)
		if point.Valid && strings.TrimSpace(point.Unit) != "" && strings.HasSuffix(text, unitSuffix) {
			valueText = strings.TrimSuffix(text, unitSuffix)
			unit = strings.TrimSpace(point.Unit)
		}
		message := strings.TrimSpace(point.Message)
		stateClass := "status-ok"
		if !point.Valid {
			text = strings.TrimSpace(point.DisplayText)
			if text == "" {
				text = "数据无效"
			}
			valueText = text
			unit = ""
			if message == "" {
				message = "数据无效或设备故障"
			}
			stateClass = "status-warn"
		} else if qualityText(point.Quality) != "良好" && qualityText(point.Quality) != "暂无数据" {
			stateClass = "status-warn"
		}
		rows = append(rows, model.RealtimePointRow{
			Key:          pointKey(point),
			Name:         displayDataItemName(pointName(point), pointKey(point)),
			Text:         text,
			ValueText:    valueText,
			Unit:         unit,
			Quality:      qualityText(point.Quality),
			Valid:        point.Valid,
			Message:      message,
			StateClass:   stateClass,
			Summary:      point.Summary,
			DisplayOrder: point.DisplayOrder,
		})
	}
	return rows
}

// realtimeSummaryText 汇总实时页面当前状态的显示文本。
func realtimeSummaryText(points []model.RealtimePointRow) string {
	if len(points) == 0 {
		return "暂无数据"
	}
	parts := make([]string, 0, len(points))
	for _, point := range points {
		name := displayDataItemName(point.Name, point.Key)
		parts = append(parts, strings.TrimSpace(name+" "+point.Text))
	}
	return strings.Join(parts, " / ")
}
