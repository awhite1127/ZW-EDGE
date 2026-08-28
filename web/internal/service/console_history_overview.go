package service

// 本文件负责历史数据总览、拓扑筛选与批量摘要视图。
// 总览固定使用聚合 IPC，避免按设备和点位重复查询历史库。

import (
	"context"
	"edge-web/internal/model"
	"fmt"
	"net/url"
	"sort"
	"strings"
	"sync"
)

func (s *ConsoleService) LoadHistoryOverview(ctx context.Context, query model.HistoryOverviewQuery) model.HistoryOverviewPageData {
	pageData := model.HistoryOverviewPageData{
		BasePageData:          model.BasePageData{BackendReachable: true},
		EmptyStateText:        "暂无可查看的历史数据项",
		EmptyStateDescription: "配置设备并产生历史记录后，可在此查看趋势曲线。",
		TableStatusText:       "趋势入口",
		LatestHistoryTimeText: "--",
		CurrentFilterText:     "全部设备",
		RefreshURL:            "/history",
		ExportURL:             "/history/export.csv",
	}

	// 四个数据源互不依赖，并行读取可避免历史总览首屏串行等待多次 IPC。
	var wg sync.WaitGroup
	maintenance := startLoad(ctx, &wg, s.backend.GetDataMaintenanceSummary)
	snapshot := startLoad(ctx, &wg, s.backend.GetRealtimeViewSnapshot)
	configSummary := startLoad(ctx, &wg, s.backend.GetConfigSummary)
	summaries := startLoad(ctx, &wg, s.backend.GetHistoryOverviewSummaries)
	wg.Wait()

	if maintenance.err == nil {
		pageData.Maintenance = maintenance.value
		pageData.MaintenanceState.Available = true
	} else {
		pageData.MaintenanceState.ErrorMessage = maintenance.err.Error()
	}

	// 实时拓扑提供设备层级和当前状态，是历史总览的基础数据。
	if snapshot.err != nil {
		pageData.BackendReachable = false
		pageData.ErrorMessage = "历史数据总览获取失败：" + snapshot.err.Error()
		return pageData
	}

	// 加载设备类型并建立拓扑索引，随后解析当前筛选条件。
	if configSummary.err != nil {
		pageData.ErrorMessage = partialError("设备类型获取", configSummary.err)
	}
	deviceTemplates := normalizedDeviceTemplates(configSummary.value.DeviceTemplates)
	pageData.HasDevices = len(snapshot.value.Devices) > 0 || len(snapshot.value.SystemStatus.DeviceStatusList) > 0
	if !pageData.HasDevices {
		return pageData
	}

	statusByID := make(map[string]model.DeviceStatus, len(snapshot.value.SystemStatus.DeviceStatusList))
	for _, status := range snapshot.value.SystemStatus.DeviceStatusList {
		statusByID[status.DeviceID] = status
	}
	realtimeByID := make(map[string]model.DeviceRealtimeSnapshot, len(snapshot.value.DeviceRealtimeSnapshots))
	for _, item := range snapshot.value.DeviceRealtimeSnapshots {
		if validRealtimeSnapshot(item) {
			realtimeByID[item.DeviceID] = item
		}
	}
	masterByID := make(map[string]model.MasterNodeConfig, len(snapshot.value.Masters))
	for _, master := range snapshot.value.Masters {
		masterByID[master.MasterID] = master
	}
	channelByID := make(map[string]model.ChannelConfig, len(snapshot.value.Channels))
	for _, channel := range snapshot.value.Channels {
		channelByID[channel.ChannelID] = channel
	}
	filter, invalidFilterMessage := resolveHistoryOverviewFilter(query, snapshot.value, masterByID, channelByID, statusByID)
	pageData.FilterTree = buildHistoryFilterTree(snapshot.value, filter)
	pageData.DeviceOptions, pageData.CurrentDeviceID, pageData.CurrentDeviceName = buildHistoryOverviewDeviceOptions(snapshot.value, filter)
	pageData.ShowDeviceSelector = len(pageData.DeviceOptions) > 1
	pageData.HasHistoryFilter = filter.Kind != ""
	pageData.CurrentFilterText = historyFilterText(filter)
	pageData.RefreshURL = historyOverviewFilterURL(filter.Kind, filter.ID)
	pageData.ExportURL = historyOverviewExportURL(filter.Kind, filter.ID)
	pageData.InvalidFilterMessage = invalidFilterMessage

	// 批量摘要是当前正式协议的一部分，一次查询覆盖全部 device/point/period，
	// 避免失败时退回 device × point × day/hour 的 N+1 IPC 与 SQLite 查询。
	if summaries.err != nil {
		pageData.ErrorMessage = partialError("历史摘要获取", summaries.err)
		pageData.EmptyStateText = "历史数据摘要暂时无法获取"
		pageData.EmptyStateDescription = "请稍后刷新；实时采集与设备配置不受影响。"
		return pageData
	}
	pageData.Rows = buildHistoryOverviewRowsFromBatch(
		snapshot.value,
		filter,
		statusByID,
		realtimeByID,
		masterByID,
		channelByID,
		deviceTemplates,
		summaries.value,
	)
	if pageData.HasHistoryFilter && len(pageData.Rows) == 0 {
		pageData.EmptyStateText = "当前筛选条件下暂无历史数据项"
		pageData.EmptyStateDescription = "可切换其他通道、主站或设备，或清除筛选查看全部历史数据项。"
	}
	historyOverviewApplyStats(&pageData)
	return pageData
}

type historyOverviewFilter struct {
	Kind  string
	ID    string
	Label string
}

func resolveHistoryOverviewFilter(
	query model.HistoryOverviewQuery,
	snapshot model.RealtimeViewSnapshot,
	masterByID map[string]model.MasterNodeConfig,
	channelByID map[string]model.ChannelConfig,
	statusByID map[string]model.DeviceStatus,
) (historyOverviewFilter, string) {
	deviceID := strings.TrimSpace(query.DeviceID)
	masterID := strings.TrimSpace(query.MasterID)
	channelID := strings.TrimSpace(query.ChannelID)
	if masterID != "" {
		master, ok := masterByID[masterID]
		if !ok {
			return historyOverviewFilter{}, "指定主站不存在，已显示全部历史数据项。"
		}
		devices := historyOverviewDevicesForMaster(snapshot, masterID)
		if len(devices) == 0 {
			return historyOverviewFilter{Kind: "master", ID: masterID, Label: displayNameOrID(master.MasterName, masterID)}, ""
		}
		selected := devices[0]
		message := ""
		if deviceID != "" {
			found := false
			for _, device := range devices {
				if device.DeviceID == deviceID {
					selected = device
					found = true
					break
				}
			}
			if !found {
				message = "原设备不属于当前主站，已自动选择该主站的第一台有效设备。"
			}
		}
		return historyOverviewFilter{
			Kind:  "device",
			ID:    selected.DeviceID,
			Label: displayNameOrID(selected.DeviceName, selected.DeviceID),
		}, message
	}
	if deviceID != "" {
		for _, device := range snapshot.Devices {
			if device.DeviceID == deviceID {
				return historyOverviewFilter{Kind: "device", ID: deviceID, Label: displayNameOrID(device.DeviceName, deviceID)}, ""
			}
		}
		if status, ok := statusByID[deviceID]; ok {
			return historyOverviewFilter{Kind: "device", ID: deviceID, Label: displayNameOrID(status.DeviceName, deviceID)}, ""
		}
		return historyOverviewFilter{}, "指定设备不存在，已显示全部历史数据项。"
	}
	if channelID != "" {
		if channel, ok := channelByID[channelID]; ok {
			return historyOverviewFilter{Kind: "channel", ID: channelID, Label: displayNameOrID(channel.ChannelName, channelID)}, ""
		}
		return historyOverviewFilter{}, "指定通道不存在，已显示全部历史数据项。"
	}
	return historyOverviewFilter{}, ""
}

func historyOverviewDevicesForMaster(snapshot model.RealtimeViewSnapshot, masterID string) []model.DeviceConfig {
	devices := make([]model.DeviceConfig, 0)
	seen := make(map[string]bool)
	for _, device := range snapshot.Devices {
		if device.MasterID != masterID || strings.TrimSpace(device.DeviceID) == "" || seen[device.DeviceID] {
			continue
		}
		seen[device.DeviceID] = true
		devices = append(devices, device)
	}
	for _, status := range snapshot.SystemStatus.DeviceStatusList {
		if status.MasterID != masterID || strings.TrimSpace(status.DeviceID) == "" || seen[status.DeviceID] {
			continue
		}
		seen[status.DeviceID] = true
		devices = append(devices, model.DeviceConfig{
			DeviceID:   status.DeviceID,
			DeviceName: status.DeviceName,
			MasterID:   status.MasterID,
		})
	}
	sort.SliceStable(devices, func(i, j int) bool {
		left := displayNameOrID(devices[i].DeviceName, devices[i].DeviceID)
		right := displayNameOrID(devices[j].DeviceName, devices[j].DeviceID)
		if left != right {
			return left < right
		}
		return devices[i].DeviceID < devices[j].DeviceID
	})
	return devices
}

func buildHistoryOverviewDeviceOptions(
	snapshot model.RealtimeViewSnapshot,
	filter historyOverviewFilter,
) ([]model.HistoryOverviewDeviceOption, string, string) {
	if filter.Kind != "device" || filter.ID == "" {
		return nil, "", ""
	}
	var selected model.DeviceConfig
	found := false
	for _, device := range snapshot.Devices {
		if device.DeviceID == filter.ID {
			selected = device
			found = true
			break
		}
	}
	if !found {
		for _, status := range snapshot.SystemStatus.DeviceStatusList {
			if status.DeviceID == filter.ID {
				selected = model.DeviceConfig{
					DeviceID:   status.DeviceID,
					DeviceName: status.DeviceName,
					MasterID:   status.MasterID,
				}
				found = true
				break
			}
		}
	}
	if !found {
		return nil, "", ""
	}
	devices := historyOverviewDevicesForMaster(snapshot, selected.MasterID)
	options := make([]model.HistoryOverviewDeviceOption, 0, len(devices))
	for _, device := range devices {
		options = append(options, model.HistoryOverviewDeviceOption{
			ID:       device.DeviceID,
			Label:    displayNameOrID(device.DeviceName, device.DeviceID),
			URL:      historyOverviewFilterURL("device", device.DeviceID),
			Selected: device.DeviceID == selected.DeviceID,
		})
	}
	return options, selected.DeviceID, displayNameOrID(selected.DeviceName, selected.DeviceID)
}

// buildHistoryFilterTree 只根据完整实时拓扑构造层级，不从当前分页历史记录反推设备，
// 因此暂时没有历史值的已配置设备仍会出现在筛选树中。
func buildHistoryFilterTree(snapshot model.RealtimeViewSnapshot, filter historyOverviewFilter) model.HistoryFilterTree {
	// 建立设备异常标记以及通道、主站、设备的归属关系。
	issueByDevice := make(map[string]bool)
	for _, status := range snapshot.SystemStatus.DeviceStatusList {
		issueByDevice[status.DeviceID] = !status.Online || !status.LastCollectSuccess || diagnosisMessage(status.Diagnosis) != ""
	}
	mastersByChannel := make(map[string][]model.MasterNodeConfig)
	for _, master := range snapshot.Masters {
		mastersByChannel[master.ChannelID] = append(mastersByChannel[master.ChannelID], master)
	}
	devicesByMaster := make(map[string][]model.DeviceConfig)
	for _, master := range snapshot.Masters {
		devicesByMaster[master.MasterID] = historyOverviewDevicesForMaster(snapshot, master.MasterID)
	}
	// 各层级按显示名称稳定排序，确保筛选树顺序一致。
	for channelID := range mastersByChannel {
		sort.SliceStable(mastersByChannel[channelID], func(i, j int) bool {
			left := displayNameOrID(mastersByChannel[channelID][i].MasterName, mastersByChannel[channelID][i].MasterID)
			right := displayNameOrID(mastersByChannel[channelID][j].MasterName, mastersByChannel[channelID][j].MasterID)
			if left != right {
				return left < right
			}
			return mastersByChannel[channelID][i].MasterID < mastersByChannel[channelID][j].MasterID
		})
	}
	for masterID := range devicesByMaster {
		sort.SliceStable(devicesByMaster[masterID], func(i, j int) bool {
			left := displayNameOrID(devicesByMaster[masterID][i].DeviceName, devicesByMaster[masterID][i].DeviceID)
			right := displayNameOrID(devicesByMaster[masterID][j].DeviceName, devicesByMaster[masterID][j].DeviceID)
			if left != right {
				return left < right
			}
			return devicesByMaster[masterID][i].DeviceID < devicesByMaster[masterID][j].DeviceID
		})
	}

	channels := append([]model.ChannelConfig(nil), snapshot.Channels...)
	sort.SliceStable(channels, func(i, j int) bool {
		left := displayNameOrID(channels[i].ChannelName, channels[i].ChannelID)
		right := displayNameOrID(channels[j].ChannelName, channels[j].ChannelID)
		if left != right {
			return left < right
		}
		return channels[i].ChannelID < channels[j].ChannelID
	})

	// 自顶向下构造筛选节点，并向上累计设备数和异常数。
	tree := model.HistoryFilterTree{
		AllURL:    "/history",
		AllActive: filter.Kind == "",
		Channels:  make([]model.HistoryFilterNode, 0, len(channels)),
	}
	for _, channel := range channels {
		channelID := strings.TrimSpace(channel.ChannelID)
		if channelID == "" {
			continue
		}
		channelNode := model.HistoryFilterNode{
			Type:     "channel",
			ID:       channelID,
			Label:    displayNameOrID(channel.ChannelName, channelID),
			URL:      historyOverviewFilterURL("channel", channelID),
			Active:   filter.Kind == "channel" && filter.ID == channelID,
			Expanded: true,
		}
		for _, master := range mastersByChannel[channelID] {
			masterID := strings.TrimSpace(master.MasterID)
			if masterID == "" {
				continue
			}
			masterNode := model.HistoryFilterNode{
				Type:   "master",
				ID:     masterID,
				Label:  displayNameOrID(master.MasterName, masterID),
				URL:    historyOverviewFilterURL("master", masterID),
				Active: filter.Kind == "master" && filter.ID == masterID,
			}
			for _, device := range devicesByMaster[masterID] {
				deviceID := strings.TrimSpace(device.DeviceID)
				if deviceID == "" {
					continue
				}
				hasIssue := issueByDevice[deviceID]
				if filter.Kind == "device" && filter.ID == deviceID {
					masterNode.Active = true
				}
				masterNode.DeviceCount++
				if hasIssue {
					masterNode.IssueCount++
				}
			}
			masterNode.HasIssue = masterNode.IssueCount > 0
			if masterNode.Active {
				channelNode.Expanded = true
			}
			channelNode.DeviceCount += masterNode.DeviceCount
			channelNode.IssueCount += masterNode.IssueCount
			channelNode.Children = append(channelNode.Children, masterNode)
		}
		channelNode.HasIssue = channelNode.IssueCount > 0
		tree.Channels = append(tree.Channels, channelNode)
	}
	return tree
}

func historyOverviewFilterURL(kind string, id string) string {
	values := url.Values{}
	switch kind {
	case "device":
		values.Set("device_id", id)
	case "master":
		values.Set("master_id", id)
	case "channel":
		values.Set("channel_id", id)
	default:
		return "/history"
	}
	return "/history?" + values.Encode()
}

func historyOverviewExportURL(kind string, id string) string {
	values := url.Values{}
	switch kind {
	case "device":
		values.Set("device_id", id)
	case "master":
		values.Set("master_id", id)
	case "channel":
		values.Set("channel_id", id)
	default:
		return "/history/export.csv"
	}
	return "/history/export.csv?" + values.Encode()
}

func historyFilterText(filter historyOverviewFilter) string {
	switch filter.Kind {
	case "device":
		return "设备 " + filter.Label
	case "master":
		return "主站 " + filter.Label
	case "channel":
		return "通道 " + filter.Label
	default:
		return "全部设备"
	}
}

type historyOverviewPoint struct {
	Key       string
	Name      string
	Unit      string
	Precision uint32
}

type historyOverviewSummaryKey struct {
	DeviceID     string
	PointKey     string
	SamplePeriod string
}

func buildHistoryOverviewRowsFromBatch(
	snapshot model.RealtimeViewSnapshot,
	filter historyOverviewFilter,
	statusByID map[string]model.DeviceStatus,
	realtimeByID map[string]model.DeviceRealtimeSnapshot,
	masterByID map[string]model.MasterNodeConfig,
	channelByID map[string]model.ChannelConfig,
	deviceTemplates []model.DeviceTemplateDefinition,
	summaries []model.HistoryOverviewSummary,
) []model.HistoryOverviewRow {
	summaryByKey := make(map[historyOverviewSummaryKey]model.HistoryOverviewSummary, len(summaries))
	pointsByDevice := make(map[string][]historyOverviewPoint)
	for _, summary := range summaries {
		deviceID := strings.TrimSpace(summary.DeviceID)
		pointKey := strings.TrimSpace(summary.PointKey)
		period := strings.TrimSpace(summary.SamplePeriod)
		if deviceID == "" || pointKey == "" || (period != "day" && period != "hour") {
			continue
		}
		summary.DeviceID = deviceID
		summary.PointKey = pointKey
		summary.SamplePeriod = period
		summaryByKey[historyOverviewSummaryKey{DeviceID: deviceID, PointKey: pointKey, SamplePeriod: period}] = summary
		pointsByDevice[deviceID] = append(pointsByDevice[deviceID], historyOverviewPointFromSummary(summary))
	}

	buildRows := func(device model.DeviceConfig, status model.DeviceStatus, hasStatus bool) []model.HistoryOverviewRow {
		if !historyOverviewDeviceMatchesFilter(device, filter, masterByID) {
			return nil
		}
		realtimeSnapshot, hasRealtime := realtimeByID[device.DeviceID]
		master := masterByID[device.MasterID]
		channel := channelByID[master.ChannelID]
		template, _ := model.FindDeviceTemplateIn(deviceTemplates, master.DeviceTemplate)
		points := mergeHistoryOverviewPointLists(
			historyOverviewConfiguredPoints(status, hasStatus, realtimeSnapshot, hasRealtime, template),
			pointsByDevice[device.DeviceID],
		)
		rows := make([]model.HistoryOverviewRow, 0, len(points))
		for _, point := range points {
			rows = append(rows, buildHistoryOverviewRowFromBatch(
				device, master, channel, status, hasStatus, realtimeSnapshot, hasRealtime,
				point, summaryByKey,
			))
		}
		return rows
	}

	rows := make([]model.HistoryOverviewRow, 0, maxInt(len(snapshot.Devices), len(snapshot.SystemStatus.DeviceStatusList)))
	for _, device := range snapshot.Devices {
		status, hasStatus := statusByID[device.DeviceID]
		rows = append(rows, buildRows(device, status, hasStatus)...)
	}
	if len(rows) == 0 {
		for _, status := range snapshot.SystemStatus.DeviceStatusList {
			rows = append(rows, buildRows(model.DeviceConfig{
				DeviceID:   status.DeviceID,
				DeviceName: status.DeviceName,
				MasterID:   status.MasterID,
			}, status, true)...)
		}
	}
	return rows
}

func buildHistoryOverviewRowFromBatch(
	device model.DeviceConfig,
	master model.MasterNodeConfig,
	channel model.ChannelConfig,
	status model.DeviceStatus,
	hasStatus bool,
	realtimeSnapshot model.DeviceRealtimeSnapshot,
	hasRealtime bool,
	point historyOverviewPoint,
	summaryByKey map[historyOverviewSummaryKey]model.HistoryOverviewSummary,
) model.HistoryOverviewRow {
	day, hasDay := summaryByKey[historyOverviewSummaryKey{DeviceID: device.DeviceID, PointKey: point.Key, SamplePeriod: "day"}]
	hour, hasHour := summaryByKey[historyOverviewSummaryKey{DeviceID: device.DeviceID, PointKey: point.Key, SamplePeriod: "hour"}]
	if hasDay {
		point = mergeHistoryOverviewPoint(point, historyOverviewPointFromSummary(day))
	}
	if hasHour {
		point = mergeHistoryOverviewPoint(point, historyOverviewPointFromSummary(hour))
	}
	if currentPoint, ok := findRealtimePointByKey(status.Points, point.Key); ok {
		point = mergeHistoryOverviewPoint(point, pointFromRealtimeValue(currentPoint))
	} else if hasRealtime {
		if currentPoint, ok := findRealtimePointByKey(realtimeSnapshot.Points, point.Key); ok {
			point = mergeHistoryOverviewPoint(point, pointFromRealtimeValue(currentPoint))
		}
	}

	row := newHistoryOverviewRow(device, master, channel, status, hasStatus, point)
	totalRecords := day.RecordCount + hour.RecordCount
	maxIntValue := uint64(^uint(0) >> 1)
	if totalRecords > maxIntValue {
		row.RecordCount = int(maxIntValue)
	} else {
		row.RecordCount = int(totalRecords)
	}
	latest, hasLatest := latestHistoryOverviewSummary(day, hasDay, hour, hasHour)
	if hasLatest {
		latestRecord := historyRecordFromOverviewSummary(latest)
		row.LatestValueText = formatHistoryValue(latest.LatestValue, latest.Precision, latest.Unit)
		row.LastUpdatedText = historyRecordTimeText(latestRecord)
		row.LastUpdatedMS = historyOverviewRecordTimeMS(latestRecord)
	}
	row.HasHistory = row.RecordCount > 0
	return row
}

func latestHistoryOverviewSummary(
	day model.HistoryOverviewSummary,
	hasDay bool,
	hour model.HistoryOverviewSummary,
	hasHour bool,
) (model.HistoryOverviewSummary, bool) {
	if !hasDay {
		return hour, hasHour
	}
	if !hasHour {
		return day, true
	}
	if historyOverviewSummaryTimeMS(hour) >= historyOverviewSummaryTimeMS(day) {
		return hour, true
	}
	return day, true
}

func historyOverviewSummaryTimeMS(summary model.HistoryOverviewSummary) uint64 {
	if summary.LatestBucketStartMS > 0 {
		return summary.LatestBucketStartMS
	}
	return summary.LatestTimestampMS
}

func historyRecordFromOverviewSummary(summary model.HistoryOverviewSummary) model.HistoryRecord {
	return model.HistoryRecord{
		DeviceID:      summary.DeviceID,
		MasterID:      summary.MasterID,
		ChannelID:     summary.ChannelID,
		SamplePeriod:  summary.SamplePeriod,
		BucketStartMS: summary.LatestBucketStartMS,
		TimestampMS:   summary.LatestTimestampMS,
		PointKey:      summary.PointKey,
		PointName:     summary.PointName,
		Unit:          summary.Unit,
		Precision:     summary.Precision,
		Value:         summary.LatestValue,
		Valid:         true,
	}
}

func historyOverviewPointFromSummary(summary model.HistoryOverviewSummary) historyOverviewPoint {
	return historyOverviewPoint{
		Key:       summary.PointKey,
		Name:      summary.PointName,
		Unit:      summary.Unit,
		Precision: summary.Precision,
	}
}

func historyOverviewDeviceMatchesFilter(
	device model.DeviceConfig,
	filter historyOverviewFilter,
	masterByID map[string]model.MasterNodeConfig,
) bool {
	switch filter.Kind {
	case "device":
		return device.DeviceID == filter.ID
	case "master":
		return device.MasterID == filter.ID
	case "channel":
		return masterByID[device.MasterID].ChannelID == filter.ID
	default:
		return true
	}
}

func newHistoryOverviewRow(
	device model.DeviceConfig,
	master model.MasterNodeConfig,
	channel model.ChannelConfig,
	status model.DeviceStatus,
	hasStatus bool,
	point historyOverviewPoint,
) model.HistoryOverviewRow {
	detailURL := deviceHistoryURL(device.DeviceID, point.Key, "day")
	if point.Key == "" {
		detailURL = deviceHistoryURL(device.DeviceID, "", "")
	}
	return model.HistoryOverviewRow{
		DeviceID:                 device.DeviceID,
		DeviceName:               defaultString(device.DeviceName, "未命名设备"),
		MasterID:                 device.MasterID,
		MasterName:               defaultString(master.MasterName, "未命名主站"),
		ChannelID:                master.ChannelID,
		ChannelName:              defaultString(channel.ChannelName, "未命名通道"),
		PointKey:                 point.Key,
		PointName:                displayDataItemName(point.Name, point.Key),
		PointDisplayName:         historyDataItemDisplayName(point.Name, point.Key, point.Unit),
		Unit:                     point.Unit,
		Precision:                point.Precision,
		LatestValueText:          "--",
		LastUpdatedText:          "--",
		DetailURL:                detailURL,
		StatusText:               historyOverviewStatusText(status, hasStatus),
		CommunicationQualityText: qualityText(status.CommunicationQuality),
	}
}

func historyOverviewConfiguredPoints(
	status model.DeviceStatus,
	hasStatus bool,
	realtimeSnapshot model.DeviceRealtimeSnapshot,
	hasRealtime bool,
	template model.DeviceTemplateDefinition,
) []historyOverviewPoint {
	points := make([]historyOverviewPoint, 0)
	if hasStatus {
		points = append(points, historyOverviewPointsFromRealtime(status.Points)...)
	}
	if hasRealtime {
		points = append(points, historyOverviewPointsFromRealtime(realtimeSnapshot.Points)...)
	}
	points = append(points, historyOverviewPointsFromTemplate(template.Fields)...)
	return mergeHistoryOverviewPointLists(points)
}

func historyOverviewPointsFromRealtime(points []model.PointValue) []historyOverviewPoint {
	result := make([]historyOverviewPoint, 0, len(points))
	for _, point := range points {
		if !point.HistoryEnabled || strings.TrimSpace(pointKey(point)) == "" {
			continue
		}
		result = append(result, pointFromRealtimeValue(point))
	}
	return result
}

func historyOverviewPointsFromTemplate(fields []model.DeviceTemplateField) []historyOverviewPoint {
	result := make([]historyOverviewPoint, 0, len(fields))
	for _, field := range fields {
		if !field.HistoryEnabled || strings.TrimSpace(field.Key) == "" {
			continue
		}
		result = append(result, pointFromTemplateField(field))
	}
	return result
}

// 数据项元信息可能来自实时快照、模板和历史接口；按输入优先级补全空字段，
// 同一个 key 只保留一个展示项，避免页面重复列出。
func mergeHistoryOverviewPointLists(lists ...[]historyOverviewPoint) []historyOverviewPoint {
	points := make([]historyOverviewPoint, 0)
	indexByKey := make(map[string]int)
	for _, list := range lists {
		for _, point := range list {
			key := strings.TrimSpace(point.Key)
			if key == "" {
				continue
			}
			point.Key = key
			if index, ok := indexByKey[key]; ok {
				points[index] = mergeHistoryOverviewPoint(points[index], point)
				continue
			}
			indexByKey[key] = len(points)
			points = append(points, point)
		}
	}
	return points
}

func historyOverviewApplyStats(pageData *model.HistoryOverviewPageData) {
	deviceIDs := make(map[string]struct{})
	pointIDs := make(map[string]struct{})
	var latestTimeMS uint64
	latestTimeText := ""
	totalRecords := 0
	for _, row := range pageData.Rows {
		if strings.TrimSpace(row.DeviceID) != "" {
			deviceIDs[row.DeviceID] = struct{}{}
		}
		pointKey := strings.TrimSpace(row.PointKey)
		if pointKey == "" {
			pointKey = strings.TrimSpace(row.PointDisplayName)
		}
		if strings.TrimSpace(row.DeviceID) != "" && pointKey != "" {
			pointIDs[row.DeviceID+"\x00"+pointKey] = struct{}{}
		}
		totalRecords += row.RecordCount
		if row.LastUpdatedMS > latestTimeMS {
			latestTimeMS = row.LastUpdatedMS
			latestTimeText = row.LastUpdatedText
		}
	}
	pageData.ViewableDeviceCount = len(deviceIDs)
	pageData.HistoryPointCount = len(pointIDs)
	pageData.TotalHistoryRecordCount = totalRecords
	if strings.TrimSpace(latestTimeText) != "" {
		pageData.LatestHistoryTimeText = latestTimeText
	}
	pageData.TableStatusText = fmt.Sprintf("数据项：%d", len(pageData.Rows))
}

func historyOverviewRecordTimeMS(record model.HistoryRecord) uint64 {
	if record.BucketStartMS > 0 {
		return record.BucketStartMS
	}
	return record.TimestampMS
}

func preferredTemplatePoint(fields []model.DeviceTemplateField) (historyOverviewPoint, bool) {
	var firstSummary *model.DeviceTemplateField
	for index := range fields {
		if fields[index].HistoryEnabled && strings.EqualFold(strings.TrimSpace(fields[index].Key), "resistance") {
			return pointFromTemplateField(fields[index]), true
		}
		if firstSummary == nil && fields[index].HistoryEnabled && strings.TrimSpace(fields[index].Key) != "" {
			firstSummary = &fields[index]
		}
	}
	if firstSummary != nil {
		return pointFromTemplateField(*firstSummary), true
	}
	return historyOverviewPoint{}, false
}

func pointFromRealtimeValue(point model.PointValue) historyOverviewPoint {
	return historyOverviewPoint{
		Key:       pointKey(point),
		Name:      displayDataItemName(pointName(point), pointKey(point)),
		Unit:      point.Unit,
		Precision: point.Precision,
	}
}

func pointFromTemplateField(field model.DeviceTemplateField) historyOverviewPoint {
	return historyOverviewPoint{
		Key:       strings.TrimSpace(field.Key),
		Name:      displayDataItemName(field.DisplayName, field.Key),
		Unit:      field.Unit,
		Precision: field.Precision,
	}
}

func mergeHistoryOverviewPoint(primary historyOverviewPoint, fallback historyOverviewPoint) historyOverviewPoint {
	if primary.Key == "" {
		primary.Key = fallback.Key
	}
	if primary.Name == "" {
		primary.Name = fallback.Name
	}
	if primary.Unit == "" {
		primary.Unit = fallback.Unit
	}
	if primary.Precision == 0 {
		primary.Precision = fallback.Precision
	}
	return primary
}

func findRealtimePointByKey(points []model.PointValue, key string) (model.PointValue, bool) {
	key = strings.TrimSpace(key)
	if key == "" {
		return model.PointValue{}, false
	}
	for _, point := range points {
		if pointKey(point) == key {
			return point, true
		}
	}
	return model.PointValue{}, false
}

func historyOverviewStatusText(status model.DeviceStatus, hasStatus bool) string {
	if !hasStatus {
		return "未采集"
	}
	if status.Online {
		return "在线"
	}
	return "离线"
}
