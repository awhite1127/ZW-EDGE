package service

// 本文件负责单设备历史查询、点位选择、趋势图与维护操作。
// 历史记录由后端分页查询，Web 层只补充显示模型和图表坐标。

import (
	"context"
	"edge-web/internal/ipc"
	"edge-web/internal/model"
	"errors"
	"fmt"
	"math"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"
)

// LoadDeviceHistory 将点位选项、查询记录、统计摘要和图表一次性组装；后端不可达时
// 仍尽量使用配置与模板信息渲染可理解的空状态。
func (s *ConsoleService) LoadDeviceHistory(ctx context.Context, deviceID string, period string, pointKey string) model.DeviceHistoryLoadResult {
	// 规范化查询条件并准备周期、导出地址等基础页面状态。
	deviceID = strings.TrimSpace(deviceID)
	period = normalizeHistoryPeriod(period)
	pointKey = strings.TrimSpace(pointKey)
	pageData := model.DeviceHistoryPageData{
		DeviceID:           deviceID,
		CurrentPeriod:      period,
		CurrentPeriodLabel: historyPeriodLabel(period),
		HistoryState: model.SectionState{
			Available: true,
		},
	}
	pageData.PeriodOptions = buildHistoryPeriodOptions(deviceID, pointKey, period)
	if deviceID != "" {
		pageData.ExportURL = deviceHistoryExportURL(deviceID, pointKey, period)
	}
	if deviceID == "" {
		pageData.HistoryState.Available = false
		pageData.HistoryState.ErrorMessage = "缺少设备，无法加载历史数据"
		return model.DeviceHistoryLoadResult{
			PageData:             pageData,
			BackendReachable:     true,
			PrimaryDataAvailable: false,
		}
	}

	// 当前运行状态用于补充点位选项和默认点位。
	status, hasStatus := model.DeviceStatus{}, false
	if systemStatus, statusErr := s.backend.GetSystemStatus(ctx); statusErr == nil {
		for _, item := range systemStatus.DeviceStatusList {
			if item.DeviceID == deviceID {
				status = item
				hasStatus = true
				break
			}
		}
	}
	realtimePoints := historyPointOptionsFromStatus(status, hasStatus)
	if pointKey == "" {
		pointKey = defaultHistoryPointKey(realtimePoints)
	}

	// 查询指定设备、周期和点位的历史视图。
	view, viewErr := s.backend.GetDeviceHistoryView(ctx, model.DeviceHistoryQuery{
		DeviceID:     deviceID,
		Days:         historyQueryDays(period),
		PointKey:     pointKey,
		SamplePeriod: period,
	})
	if viewErr != nil {
		reachable := !isBackendUnavailable(viewErr)
		pageData.HistoryState.Available = false
		pageData.HistoryState.ErrorMessage = deviceHistoryLoadErrorMessage(viewErr, reachable)
		return model.DeviceHistoryLoadResult{
			PageData:             pageData,
			BackendReachable:     reachable,
			PrimaryDataAvailable: false,
		}
	}

	// 填充设备、主站和通道的页面标识信息。
	pageData.HasDevice = true
	pageData.DeviceID = view.Device.DeviceID
	pageData.DeviceName = defaultString(view.Device.DeviceName, "未命名设备")
	pageData.MasterID = view.Master.MasterID
	pageData.MasterName = defaultString(view.Master.MasterName, "未命名主站")
	if view.Master.TargetAddress > 0 {
		pageData.MasterAddressText = fmt.Sprintf("%d", view.Master.TargetAddress)
	}
	pageData.ChannelID = view.Channel.ChannelID
	pageData.ChannelName = defaultString(view.Channel.ChannelName, "未命名通道")

	// 合并实时状态、设备类型与历史结果中的点位选项。
	history := normalizeDeviceHistoryRecords(view.HistoryRecords)
	if pointKey == "" && len(history) > 0 {
		pointKey = history[0].PointKey
	}
	pageData.CurrentPointKey = pointKey
	templatePoints := s.historyPointOptionsFromDeviceTemplate(ctx, view.Master.DeviceTemplate)
	pageData.AvailablePoints = mergeHistoryPointOptions(
		mergeDeviceHistoryPointOptions(realtimePoints, templatePoints),
		view.HistoryPoints,
		history,
		pointKey,
		deviceID,
		period,
	)
	if pageData.CurrentPointKey == "" {
		pageData.CurrentPointKey = defaultHistoryPointKey(pageData.AvailablePoints)
	}
	pageData.CurrentPointName = historyPointName(pageData.AvailablePoints, pageData.CurrentPointKey)
	pageData.CurrentPointUnit = historyPointUnit(pageData.AvailablePoints, pageData.CurrentPointKey)
	pageData.CurrentPointPrecision = historyPointPrecision(pageData.AvailablePoints, pageData.CurrentPointKey)
	pageData.PeriodOptions = buildHistoryPeriodOptions(deviceID, pageData.CurrentPointKey, period)
	pageData.ExportURL = deviceHistoryExportURL(deviceID, pageData.CurrentPointKey, period)

	if pageData.ChannelID == "" && len(history) > 0 {
		pageData.ChannelID = history[len(history)-1].ChannelID
	}
	if pageData.ChannelName == "" {
		pageData.ChannelName = "未命名通道"
	}

	// 匹配当前点位的启用报警规则，并生成表格、摘要、曲线和阈值线。
	var activeRule *model.AlarmRule
	if rules, err := s.backend.ListAlarmRules(ctx, deviceID); err == nil {
		for index := range rules {
			if rules[index].PointKey == pageData.CurrentPointKey && rules[index].Enabled {
				rule := normalizeAlarmRuleForView(rules[index])
				activeRule = &rule
				break
			}
		}
	}
	pageData.HistoryRecords = buildDeviceHistoryRows(history)
	pageData.Summary = buildDeviceHistorySummary(history)
	pageData.Chart, pageData.AlarmThresholds = buildDeviceHistoryChart(history, period, activeRule)
	pageData.HasHistory = len(history) > 0
	if !pageData.HasHistory {
		pageData.Chart.Message = historyEmptyMessage(period)
	}

	return model.DeviceHistoryLoadResult{
		PageData:             pageData,
		BackendReachable:     true,
		PrimaryDataAvailable: true,
	}
}

func normalizeHistoryPeriod(period string) string {
	switch strings.ToLower(strings.TrimSpace(period)) {
	case "hour":
		return "hour"
	default:
		return "day"
	}
}

func historyPeriodLabel(period string) string {
	if normalizeHistoryPeriod(period) == "hour" {
		return "最近 24 小时"
	}
	return "最近 30 天"
}

func historyQueryDays(period string) uint32 {
	if normalizeHistoryPeriod(period) == "hour" {
		return 1
	}
	return 30
}

func historyEmptyMessage(period string) string {
	return "设备产生历史记录后，可在此查看趋势曲线。"
}

func deviceHistoryURL(deviceID string, pointKey string, period string) string {
	values := url.Values{}
	values.Set("device_id", deviceID)
	if strings.TrimSpace(pointKey) != "" {
		values.Set("point_key", strings.TrimSpace(pointKey))
	}
	values.Set("period", normalizeHistoryPeriod(period))
	return "/device-history?" + values.Encode()
}

func deviceHistoryExportURL(deviceID string, pointKey string, period string) string {
	values := url.Values{}
	values.Set("device_id", strings.TrimSpace(deviceID))
	if strings.TrimSpace(pointKey) != "" {
		values.Set("point_key", strings.TrimSpace(pointKey))
	}
	values.Set("period", normalizeHistoryPeriod(period))
	return "/history/export.csv?" + values.Encode()
}

func buildHistoryPeriodOptions(deviceID string, pointKey string, currentPeriod string) []model.DeviceHistoryPeriodOption {
	currentPeriod = normalizeHistoryPeriod(currentPeriod)
	return []model.DeviceHistoryPeriodOption{
		{
			Value:    "hour",
			Label:    "最近 24 小时",
			Selected: currentPeriod == "hour",
			URL:      deviceHistoryURL(deviceID, pointKey, "hour"),
		},
		{
			Value:    "day",
			Label:    "最近 30 天",
			Selected: currentPeriod == "day",
			URL:      deviceHistoryURL(deviceID, pointKey, "day"),
		},
	}
}

func historyPointOptionsFromStatus(status model.DeviceStatus, hasStatus bool) []model.DeviceHistoryPointOption {
	if !hasStatus {
		return nil
	}
	points := make([]model.PointValue, 0, len(status.Points))
	for _, point := range status.Points {
		if point.HistoryEnabled && strings.TrimSpace(point.Key) != "" {
			points = append(points, point)
		}
	}
	sort.SliceStable(points, func(i, j int) bool {
		if points[i].DisplayOrder == points[j].DisplayOrder {
			return points[i].Key < points[j].Key
		}
		return points[i].DisplayOrder < points[j].DisplayOrder
	})
	options := make([]model.DeviceHistoryPointOption, 0, len(points))
	for _, point := range points {
		options = append(options, model.DeviceHistoryPointOption{
			Key:       point.Key,
			Name:      displayDataItemName(pointName(point), point.Key),
			Unit:      point.Unit,
			Precision: point.Precision,
		})
	}
	return options
}

func (s *ConsoleService) historyPointOptionsFromDeviceTemplate(ctx context.Context, templateID string) []model.DeviceHistoryPointOption {
	templateID = strings.TrimSpace(templateID)
	if templateID == "" {
		return nil
	}
	configSummary, err := s.backend.GetConfigSummary(ctx)
	if err != nil {
		return nil
	}
	template, ok := model.FindDeviceTemplateIn(normalizedDeviceTemplates(configSummary.DeviceTemplates), templateID)
	if !ok {
		return nil
	}
	fields := make([]model.DeviceTemplateField, 0, len(template.Fields))
	for _, field := range template.Fields {
		if field.HistoryEnabled && strings.TrimSpace(field.Key) != "" {
			fields = append(fields, field)
		}
	}
	sort.SliceStable(fields, func(i, j int) bool {
		if fields[i].DisplayOrder == fields[j].DisplayOrder {
			return fields[i].Key < fields[j].Key
		}
		return fields[i].DisplayOrder < fields[j].DisplayOrder
	})
	options := make([]model.DeviceHistoryPointOption, 0, len(fields))
	for _, field := range fields {
		options = append(options, model.DeviceHistoryPointOption{
			Key:       strings.TrimSpace(field.Key),
			Name:      displayDataItemName(field.DisplayName, field.Key),
			Unit:      field.Unit,
			Precision: field.Precision,
		})
	}
	if len(options) == 0 {
		if point, ok := preferredTemplatePoint(template.Fields); ok {
			options = append(options, model.DeviceHistoryPointOption{
				Key:       point.Key,
				Name:      displayDataItemName(point.Name, point.Key),
				Unit:      point.Unit,
				Precision: point.Precision,
			})
		}
	}
	return options
}

func mergeDeviceHistoryPointOptions(lists ...[]model.DeviceHistoryPointOption) []model.DeviceHistoryPointOption {
	options := make([]model.DeviceHistoryPointOption, 0)
	seen := make(map[string]int)
	for _, list := range lists {
		for _, option := range list {
			key := strings.TrimSpace(option.Key)
			if key == "" {
				continue
			}
			option.Key = key
			if index, ok := seen[key]; ok {
				if options[index].Name == "" {
					options[index].Name = option.Name
				}
				if options[index].Unit == "" {
					options[index].Unit = option.Unit
				}
				if options[index].Precision == 0 {
					options[index].Precision = option.Precision
				}
				continue
			}
			seen[key] = len(options)
			options = append(options, option)
		}
	}
	return options
}

func mergeHistoryPointOptions(
	realtimeOptions []model.DeviceHistoryPointOption,
	historyPoints []model.HistoryPointSummary,
	history []model.HistoryRecord,
	currentPointKey string,
	deviceID string,
	period string,
) []model.DeviceHistoryPointOption {
	options := append([]model.DeviceHistoryPointOption{}, realtimeOptions...)
	seen := make(map[string]int, len(options))
	for index := range options {
		seen[options[index].Key] = index
	}
	for _, point := range historyPoints {
		key := strings.TrimSpace(point.PointKey)
		if key == "" {
			continue
		}
		if index, ok := seen[key]; ok {
			if options[index].Name == "" {
				options[index].Name = displayDataItemName(point.PointName, key)
			}
			if options[index].Unit == "" {
				options[index].Unit = point.Unit
			}
			if options[index].Precision == 0 {
				options[index].Precision = point.Precision
			}
			continue
		}
		seen[key] = len(options)
		options = append(options, model.DeviceHistoryPointOption{
			Key:       key,
			Name:      displayDataItemName(point.PointName, key),
			Unit:      point.Unit,
			Precision: point.Precision,
		})
	}
	for _, record := range history {
		key := strings.TrimSpace(record.PointKey)
		if key == "" {
			continue
		}
		if index, ok := seen[key]; ok {
			if options[index].Name == "" {
				options[index].Name = displayDataItemName(record.PointName, key)
			}
			if options[index].Unit == "" {
				options[index].Unit = record.Unit
			}
			if options[index].Precision == 0 {
				options[index].Precision = record.Precision
			}
			continue
		}
		seen[key] = len(options)
		options = append(options, model.DeviceHistoryPointOption{
			Key:       key,
			Name:      displayDataItemName(record.PointName, key),
			Unit:      record.Unit,
			Precision: record.Precision,
		})
	}
	if currentPointKey != "" {
		if _, ok := seen[currentPointKey]; !ok {
			options = append(options, model.DeviceHistoryPointOption{
				Key:  currentPointKey,
				Name: displayDataItemName("", currentPointKey),
			})
		}
	}
	for index := range options {
		options[index].Selected = options[index].Key == currentPointKey
		options[index].URL = deviceHistoryURL(deviceID, options[index].Key, period)
	}
	return options
}

func defaultHistoryPointKey(options []model.DeviceHistoryPointOption) string {
	for _, option := range options {
		if option.Key == "resistance" {
			return option.Key
		}
	}
	for _, option := range options {
		if strings.TrimSpace(option.Key) != "" {
			return option.Key
		}
	}
	return ""
}

func historyPointName(options []model.DeviceHistoryPointOption, key string) string {
	for _, option := range options {
		if option.Key == key {
			return displayDataItemName(option.Name, option.Key)
		}
	}
	return displayDataItemName("", key)
}

func historyPointUnit(options []model.DeviceHistoryPointOption, key string) string {
	for _, option := range options {
		if option.Key == key {
			return option.Unit
		}
	}
	return ""
}

func historyPointPrecision(options []model.DeviceHistoryPointOption, key string) uint32 {
	for _, option := range options {
		if option.Key == key {
			return option.Precision
		}
	}
	return 0
}

func deviceHistoryLoadErrorMessage(err error, reachable bool) string {
	if err == nil {
		return ""
	}
	if !reachable {
		return "无法连接后端服务，请检查 edge-controller 是否正在运行"
	}

	var callErr *ipc.CallError
	if errors.As(err, &callErr) && strings.TrimSpace(callErr.Message) != "" {
		return callErr.Message
	}
	return "历史数据读取失败：" + err.Error()
}

// 图表要求时间递增且同一时刻只有一个值；这里在服务端统一排序去重，模板无需再处理。
func normalizeDeviceHistoryRecords(records []model.HistoryRecord) []model.HistoryRecord {
	result := make([]model.HistoryRecord, 0, len(records))
	for _, record := range records {
		if historyRecordTimeText(record) == "" || math.IsNaN(record.Value) || math.IsInf(record.Value, 0) {
			continue
		}
		result = append(result, record)
	}
	sort.SliceStable(result, func(i, j int) bool {
		if result[i].BucketStartMS == result[j].BucketStartMS {
			if result[i].TimestampMS == result[j].TimestampMS {
				return result[i].PointKey < result[j].PointKey
			}
			return result[i].TimestampMS < result[j].TimestampMS
		}
		return result[i].BucketStartMS < result[j].BucketStartMS
	})
	return result
}

func buildDeviceHistoryRows(records []model.HistoryRecord) []model.DeviceHistoryRecordRow {
	rows := make([]model.DeviceHistoryRecordRow, 0, len(records))
	for index := len(records) - 1; index >= 0; index-- {
		record := records[index]
		valueText := formatHistoryValue(record.Value, record.Precision, record.Unit)
		rows = append(rows, model.DeviceHistoryRecordRow{
			TimeText:     historyRecordTimeText(record),
			Date:         historyRecordDateText(record),
			PointName:    displayDataItemName(record.PointName, record.PointKey),
			DisplayValue: valueText,
			ValueText:    valueText,
			Quality:      qualityText(record.Quality),
			TimestampMS:  record.TimestampMS,
			MasterID:     record.MasterID,
			ChannelID:    record.ChannelID,
			SamplePeriod: record.SamplePeriod,
			SampleInfo:   record.Message,
		})
	}
	return rows
}

func (s *ConsoleService) CleanupExpiredData(ctx context.Context) model.ActionFeedback {
	summary, err := s.backend.CleanupExpiredData(ctx)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := strings.TrimSpace(summary.LastCleanupResult)
	if message == "" {
		message = "超期数据清理已完成"
	}
	return model.ActionFeedback{Success: true, Message: message}
}

func (s *ConsoleService) GetDataMaintenanceSummary(ctx context.Context) (model.DataMaintenanceSummary, error) {
	return s.backend.GetDataMaintenanceSummary(ctx)
}

func buildDeviceHistorySummary(records []model.HistoryRecord) model.DeviceHistorySummary {
	if len(records) == 0 {
		return model.DeviceHistorySummary{}
	}

	minValue := records[0].Value
	maxValue := records[0].Value
	sum := 0.0
	for _, record := range records {
		if record.Value < minValue {
			minValue = record.Value
		}
		if record.Value > maxValue {
			maxValue = record.Value
		}
		sum += record.Value
	}
	latest := records[len(records)-1]
	return model.DeviceHistorySummary{
		HasData:          true,
		LatestValueText:  formatHistoryValue(latest.Value, latest.Precision, latest.Unit),
		LatestDate:       historyRecordTimeText(latest),
		MaxValueText:     formatHistoryValue(maxValue, latest.Precision, latest.Unit),
		MinValueText:     formatHistoryValue(minValue, latest.Precision, latest.Unit),
		AverageValueText: formatHistoryValue(sum/float64(len(records)), latest.Precision, latest.Unit),
		RecordCount:      len(records),
	}
}

func buildDeviceHistoryChart(records []model.HistoryRecord, period string, rule *model.AlarmRule) (model.DeviceHistoryChart, []model.DeviceHistoryThresholdLine) {
	// 过滤无效测量值，同时保留原始下标用于识别数据断点。
	validRecords := make([]model.HistoryRecord, 0, len(records))
	validOriginalIndexes := make([]int, 0, len(records))
	for index, record := range records {
		if historyRecordValidForChart(record) {
			validRecords = append(validRecords, record)
			validOriginalIndexes = append(validOriginalIndexes, index)
		}
	}
	chart := model.DeviceHistoryChart{HasData: len(validRecords) > 0}
	if len(records) == 0 {
		chart.Message = historyEmptyMessage(period)
		return chart, nil
	}
	if len(validRecords) == 0 {
		chart.Message = "当前周期没有可用于绘图的有效测量数据。"
		return chart, nil
	}

	// 统计有效值范围和最大精度，用于计算纵轴刻度。
	minValue := validRecords[0].Value
	maxValue := validRecords[0].Value
	minIndex := 0
	maxIndex := 0
	precision := validRecords[0].Precision
	for index, record := range validRecords {
		if record.Value < minValue {
			minValue = record.Value
			minIndex = index
		}
		if record.Value > maxValue {
			maxValue = record.Value
			maxIndex = index
		}
		if record.Precision > precision {
			precision = record.Precision
		}
	}
	axis := calculateHistoryChartAxis(minValue, maxValue, precision)

	// 选择可纳入当前视图的报警阈值，避免阈值过远压缩实际趋势。
	type thresholdCandidate struct {
		kind  string
		value float64
	}
	candidates := make([]thresholdCandidate, 0, 2)
	if rule != nil {
		if rule.HighEnabled {
			candidates = append(candidates, thresholdCandidate{kind: "high", value: rule.HighThreshold})
		}
		if rule.LowEnabled {
			candidates = append(candidates, thresholdCandidate{kind: "low", value: rule.LowThreshold})
		}
	}
	includedThresholds := make(map[string]bool, len(candidates))
	combinedMin, combinedMax := minValue, maxValue
	baseSpan := axis.maximum - axis.minimum
	focusSpan := maxValue - minValue
	if focusSpan <= 0 {
		focusSpan = baseSpan
	}
	for _, candidate := range candidates {
		if math.IsNaN(candidate.value) || math.IsInf(candidate.value, 0) {
			continue
		}
		candidateMin := math.Min(combinedMin, candidate.value)
		candidateMax := math.Max(combinedMax, candidate.value)
		alreadyVisible := candidate.value >= axis.minimum && candidate.value <= axis.maximum
		// 阈值加入后若使实际测量跨度压缩到不足约 40%，则改用视图外提示。
		if alreadyVisible || candidateMax-candidateMin <= focusSpan*2.5 {
			includedThresholds[candidate.kind] = true
			combinedMin, combinedMax = candidateMin, candidateMax
		}
	}
	if len(includedThresholds) > 0 {
		axis = calculateHistoryChartAxis(combinedMin, combinedMax, precision)
	}

	// 将采样时间和值映射到 SVG 坐标，并在无效值或长时间间隔处断开折线。
	const (
		left   = 8.0
		right  = 94.0
		top    = 10.0
		bottom = 82.0
	)
	valueRange := axis.maximum - axis.minimum
	points := make([]model.DeviceHistoryChartPoint, 0, len(validRecords))
	allPointParts := make([]string, 0, len(validRecords))
	segments := make([]string, 0, 2)
	currentSegment := make([]string, 0, len(validRecords))
	firstTime := historyChartRecordTime(validRecords[0])
	lastTime := historyChartRecordTime(validRecords[len(validRecords)-1])
	timeRange := lastTime - firstTime
	expectedInterval := historyChartExpectedInterval(period)
	for index, record := range validRecords {
		x := left
		currentTime := historyChartRecordTime(record)
		if timeRange > 0 && currentTime >= firstTime {
			x = left + (right-left)*float64(currentTime-firstTime)/float64(timeRange)
		} else if len(validRecords) > 1 {
			x = left + (right-left)*float64(index)/float64(len(validRecords)-1)
		}
		ratio := (record.Value - axis.minimum) / valueRange
		y := bottom - (bottom-top)*ratio
		xText := fmt.Sprintf("%.2f", x)
		yText := fmt.Sprintf("%.2f", y)
		points = append(points, model.DeviceHistoryChartPoint{
			X:         xText,
			Y:         yText,
			Date:      historyRecordTimeText(record),
			ValueText: formatHistoryValue(record.Value, record.Precision, record.Unit),
		})
		pointPart := xText + "," + yText
		allPointParts = append(allPointParts, pointPart)
		if index > 0 {
			previousTime := historyChartRecordTime(validRecords[index-1])
			invalidBetween := validOriginalIndexes[index]-validOriginalIndexes[index-1] > 1
			longGap := expectedInterval > 0 && previousTime > 0 && currentTime > previousTime &&
				currentTime-previousTime > expectedInterval*5/2
			if invalidBetween || longGap {
				if len(currentSegment) >= 2 {
					segments = append(segments, strings.Join(currentSegment, " "))
				}
				currentSegment = currentSegment[:0]
			}
		}
		currentSegment = append(currentSegment, pointPart)
	}
	if len(currentSegment) >= 2 {
		segments = append(segments, strings.Join(currentSegment, " "))
	}

	// 选择有限数量的数值标签，并补齐坐标轴和时间范围。
	chart.HasLine = len(segments) > 0
	chart.Points = points
	chart.Segments = segments
	labelIndexes := selectHistoryChartLabelIndexes(validRecords, minIndex, maxIndex, maxValue-minValue, 6)
	chart.Labels = make([]model.DeviceHistoryChartLabel, 0, len(labelIndexes))
	for _, index := range labelIndexes {
		className := "history-value-label-above"
		y, _ := strconv.ParseFloat(points[index].Y, 64)
		if y <= 24 {
			className = "history-value-label-below"
		}
		if index == 0 {
			className += " history-value-label-first"
		} else if index == len(points)-1 {
			className += " history-value-label-last"
		}
		chart.Labels = append(chart.Labels, model.DeviceHistoryChartLabel{
			X: points[index].X, Y: points[index].Y, ValueText: points[index].ValueText, ClassName: className,
		})
	}
	chart.PolylinePoints = strings.Join(allPointParts, " ")
	latest := validRecords[len(validRecords)-1]
	chart.AxisMin = axis.minimum
	chart.AxisMax = axis.maximum
	chart.MinLabel = formatHistoryAxisValue(axis.minimum, precision, axis.step, latest.Unit)
	chart.MaxLabel = formatHistoryAxisValue(axis.maximum, precision, axis.step, latest.Unit)
	chart.YTicks = buildHistoryChartTicks(axis, precision, latest.Unit, top, bottom)
	chart.StartDate = historyRecordTimeText(validRecords[0])
	chart.EndDate = historyRecordTimeText(latest)
	// 生成视图内阈值线；未纳入坐标轴的阈值仅显示越界提示。
	lines := make([]model.DeviceHistoryThresholdLine, 0, 2)
	for _, candidate := range candidates {
		label := map[string]string{"high": "上限", "low": "下限"}[candidate.kind] + " " +
			formatHistoryValue(candidate.value, latest.Precision, latest.Unit)
		if !includedThresholds[candidate.kind] {
			lines = append(lines, model.DeviceHistoryThresholdLine{
				Kind: candidate.kind, Label: label + "（超出当前视图）", OutOfView: true,
			})
			continue
		}
		ratio := (candidate.value - axis.minimum) / valueRange
		y := bottom - (bottom-top)*ratio
		labelClass := ""
		if y <= 22 {
			labelClass = "history-threshold-label-below"
		}
		lines = append(lines, model.DeviceHistoryThresholdLine{
			Kind: candidate.kind, Label: label, Y: fmt.Sprintf("%.2f", y), LabelClass: labelClass,
		})
	}
	return chart, lines
}

type historyChartAxis struct {
	minimum float64
	maximum float64
	step    float64
}

func historyRecordValidForChart(record model.HistoryRecord) bool {
	if !record.Valid || math.IsNaN(record.Value) || math.IsInf(record.Value, 0) {
		return false
	}
	switch strings.ToLower(strings.TrimSpace(record.Quality)) {
	case "bad", "invalid", "error":
		return false
	default:
		return true
	}
}

func historyChartRecordTime(record model.HistoryRecord) uint64 {
	if record.BucketStartMS > 0 {
		return record.BucketStartMS
	}
	return record.TimestampMS
}

func historyChartExpectedInterval(period string) uint64 {
	if normalizeHistoryPeriod(period) == "hour" {
		return uint64(time.Hour / time.Millisecond)
	}
	return uint64(24 * time.Hour / time.Millisecond)
}

func calculateHistoryChartAxis(minimum, maximum float64, precision uint32) historyChartAxis {
	precision = minUint32(precision, 8)
	quantum := math.Pow10(-int(precision))
	span := maximum - minimum
	rawMin, rawMax := minimum, maximum
	if span > 0 {
		padding := math.Max(span*0.12, quantum*0.5)
		rawMin -= padding
		rawMax += padding
	} else {
		halfRange := math.Max(math.Abs(minimum)*0.02, quantum*2)
		rawMin -= halfRange
		rawMax += halfRange
	}
	if rawMax <= rawMin || math.IsNaN(rawMin) || math.IsNaN(rawMax) ||
		math.IsInf(rawMin, 0) || math.IsInf(rawMax, 0) {
		rawMin, rawMax = minimum-quantum*2, maximum+quantum*2
	}
	step := niceHistoryChartStep((rawMax - rawMin) / 4)
	axisMin := math.Floor(rawMin/step) * step
	axisMax := math.Ceil(rawMax/step) * step
	if axisMax <= axisMin {
		axisMin, axisMax = rawMin, rawMax
	}
	return historyChartAxis{minimum: axisMin, maximum: axisMax, step: step}
}

func niceHistoryChartStep(value float64) float64 {
	if value <= 0 || math.IsNaN(value) || math.IsInf(value, 0) {
		return 1
	}
	exponent := math.Floor(math.Log10(value))
	fraction := value / math.Pow(10, exponent)
	niceFraction := 1.0
	switch {
	case fraction <= 1:
		niceFraction = 1
	case fraction <= 2:
		niceFraction = 2
	case fraction <= 2.5:
		niceFraction = 2.5
	case fraction <= 5:
		niceFraction = 5
	default:
		niceFraction = 10
	}
	return niceFraction * math.Pow(10, exponent)
}

func buildHistoryChartTicks(axis historyChartAxis, precision uint32, unit string, top, bottom float64) []model.DeviceHistoryChartTick {
	count := int(math.Round((axis.maximum - axis.minimum) / axis.step))
	if count < 1 {
		count = 1
	}
	if count > 8 {
		count = 8
	}
	ticks := make([]model.DeviceHistoryChartTick, 0, count+1)
	for index := 0; index <= count; index++ {
		value := axis.minimum + (axis.maximum-axis.minimum)*float64(index)/float64(count)
		ratio := (value - axis.minimum) / (axis.maximum - axis.minimum)
		y := bottom - (bottom-top)*ratio
		ticks = append(ticks, model.DeviceHistoryChartTick{
			Y: fmt.Sprintf("%.2f", y), Label: formatHistoryAxisValue(value, precision, axis.step, unit),
		})
	}
	return ticks
}

func formatHistoryAxisValue(value float64, precision uint32, step float64, unit string) string {
	stepDecimals := 0
	for stepDecimals < 8 {
		scaled := step * math.Pow10(stepDecimals)
		if math.Abs(scaled-math.Round(scaled)) < 1e-8 {
			break
		}
		stepDecimals++
	}
	decimals := maxInt(int(minUint32(precision, 8)), stepDecimals)
	text := strconv.FormatFloat(value, 'f', decimals, 64)
	if unit != "" {
		text += " " + unit
	}
	return text
}

func minUint32(left, right uint32) uint32 {
	if left < right {
		return left
	}
	return right
}

func selectHistoryChartLabelIndexes(
	records []model.HistoryRecord,
	minIndex int,
	maxIndex int,
	valueRange float64,
	limit int,
) []int {
	if len(records) == 0 || limit <= 0 {
		return nil
	}
	selected := map[int]bool{}
	minimumGap := maxInt(1, (len(records)+9)/10)
	canSelect := func(index int) bool {
		for existing := range selected {
			if absInt(existing-index) < minimumGap {
				return false
			}
		}
		return true
	}
	priority := []int{0, len(records) - 1}
	for _, index := range priority {
		if len(selected) < limit {
			selected[index] = true
		}
	}
	for _, index := range []int{maxIndex, minIndex} {
		if len(selected) < limit && canSelect(index) {
			selected[index] = true
		}
	}
	type candidate struct {
		index int
		score float64
	}
	candidates := make([]candidate, 0, len(records))
	jumpThreshold := valueRange * 0.2
	for index := 1; index < len(records)-1; index++ {
		before := records[index].Value - records[index-1].Value
		after := records[index+1].Value - records[index].Value
		turning := (before > 0 && after < 0) || (before < 0 && after > 0)
		jump := valueRange > 0 && math.Max(math.Abs(before), math.Abs(after)) >= jumpThreshold
		if !turning && !jump {
			continue
		}
		candidates = append(candidates, candidate{index: index, score: math.Abs(before) + math.Abs(after)})
	}
	sort.SliceStable(candidates, func(left, right int) bool { return candidates[left].score > candidates[right].score })
	for _, item := range candidates {
		if len(selected) >= limit {
			break
		}
		if canSelect(item.index) {
			selected[item.index] = true
		}
	}
	indexes := make([]int, 0, len(selected))
	for index := range selected {
		indexes = append(indexes, index)
	}
	sort.Ints(indexes)
	return indexes
}

func absInt(value int) int {
	if value < 0 {
		return -value
	}
	return value
}
