package service

// 本文件将设备配置、模板定义、实时值和可执行命令合并为设备详情视图。
// 主动读写命令仍通过后端 IPC 执行，Web 层只负责校验页面参数和组织展示结果。

import (
	"context"
	"edge-web/internal/model"
	"fmt"
	"strings"
	"sync"
	"time"
)

func (s *ConsoleService) GetDeviceDetail(ctx context.Context, deviceID string) (model.DeviceDetailResponse, error) {
	deviceID = strings.TrimSpace(deviceID)
	if deviceID == "" {
		return model.DeviceDetailResponse{}, fmt.Errorf("缺少设备 ID")
	}

	// 并行获取历史视图、设备类型和运行状态，缩短详情弹窗加载时间。
	var (
		view          model.DeviceHistoryView
		configSummary model.ConfigSummary
		systemStatus  model.SystemStatus
		viewErr       error
		statusErr     error
		wg            sync.WaitGroup
	)
	wg.Add(3)
	go func() {
		defer wg.Done()
		view, viewErr = s.backend.GetDeviceHistoryView(ctx, model.DeviceHistoryQuery{DeviceID: deviceID, Days: 30})
	}()
	go func() {
		defer wg.Done()
		configSummary, _ = s.backend.GetConfigSummary(ctx)
	}()
	go func() {
		defer wg.Done()
		systemStatus, statusErr = s.backend.GetSystemStatus(ctx)
	}()
	wg.Wait()

	if viewErr != nil {
		return model.DeviceDetailResponse{}, viewErr
	}

	// 设备类型由 controller 单一提供；摘要不可用或引用失效时只展示未知类型，
	// 不在 Web 侧注入另一套字段和写命令定义。
	templates := normalizedDeviceTemplates(configSummary.DeviceTemplates)
	template, hasTemplate := model.FindDeviceTemplateIn(templates, view.Master.DeviceTemplate)
	if !hasTemplate {
		template = model.DeviceTemplateDefinition{
			ID:          view.Master.DeviceTemplate,
			DisplayName: defaultString(view.Master.DeviceTemplate, "未知设备类型"),
		}
	}

	// 从系统状态中定位当前设备，并整理最近历史记录。
	status, hasStatus := model.DeviceStatus{}, false
	if statusErr == nil {
		for _, item := range systemStatus.DeviceStatusList {
			if item.DeviceID == view.Device.DeviceID {
				status = item
				hasStatus = true
				break
			}
		}
	}

	history := normalizeDeviceHistoryRecords(view.HistoryRecords)
	historyRows := buildDeviceHistoryRows(history)
	if len(historyRows) > 10 {
		historyRows = historyRows[:10]
	}

	// 提取摘要点位并计算页面展示所需的设备状态。
	summaryFields := make([]model.DeviceTemplateField, 0, len(template.Fields))
	for _, field := range template.Fields {
		if field.Summary {
			summaryFields = append(summaryFields, field)
		}
	}

	updatedAt := status.UpdatedAtMS
	if updatedAt == 0 {
		updatedAt = maxUint64(status.LastSuccessTimeMS, status.LastFailureTimeMS)
	}
	detailStatus := model.DeviceDetailStatus{
		Enabled:              view.Device.Enabled,
		Online:               hasStatus && status.Online,
		HasStatus:            hasStatus,
		CommunicationQuality: qualityText(status.CommunicationQuality),
		LastCollectTimeMS:    updatedAt,
		LastCollectTime:      formatDetailTimestamp(updatedAt),
		StatusSummary:        deviceDetailStatusSummary(status, hasStatus),
		Diagnosis:            status.Diagnosis,
	}

	// 合并配置、点位、写命令和历史摘要，形成完整详情响应。
	return model.DeviceDetailResponse{
		Device:       view.Device,
		DeviceLabel:  defaultString(view.Device.DeviceName, view.Device.DeviceID),
		TemplateID:   template.ID,
		TemplateName: defaultString(template.DisplayName, template.ID),
		Master:       view.Master,
		MasterLabel:  defaultString(view.Master.MasterName, view.Master.MasterID),
		Channel:      view.Channel,
		ChannelLabel: defaultString(view.Channel.ChannelName, view.Channel.ChannelID),
		ProtocolText: masterProtocolText(view.Master.Protocol),
		SlaveAddress: view.Master.TargetAddress,
		Status:       detailStatus,
		ReadProfile: model.DeviceDetailReadProfile{
			TrueStartRegister:   uint32(view.Master.BlockStartRegister) + uint32(view.Device.RegisterOffset),
			DeviceAddressStride: template.DeviceAddressStride,
			ReadBlocks:          append([]model.DeviceTemplateReadBlock(nil), template.ReadBlocks...),
			SummaryFields:       summaryFields,
		},
		Points:        decorateDeviceDetailPoints(template.ID, template.DisplayName, status.Points),
		WriteCommands: template.WriteCommands,
		History: model.DeviceDetailHistory{
			HasHistory:     len(historyRows) > 0,
			HistoryRecords: historyRows,
			Summary:        buildDeviceHistorySummary(history),
			FullHistoryURL: deviceHistoryURL(deviceID, "", ""),
		},
	}, nil
}

func (s *ConsoleService) ExecuteDeviceCommand(
	ctx context.Context,
	request model.DeviceCommandExecuteRequest,
) (model.DeviceCommandExecuteResponse, error) {
	request.DeviceID = strings.TrimSpace(request.DeviceID)
	request.CommandKey = strings.TrimSpace(request.CommandKey)
	if request.Values == nil {
		request.Values = map[string]uint16{}
	}
	return s.backend.ExecuteDeviceCommand(ctx, request)
}

func (s *ConsoleService) ReadEM100EventRecord(ctx context.Context, deviceID string) (model.EM100RecordReadResponse, error) {
	deviceID = strings.TrimSpace(deviceID)
	if deviceID == "" {
		return model.EM100RecordReadResponse{}, fmt.Errorf("缺少设备 ID")
	}
	return s.backend.ReadEM100EventRecord(ctx, deviceID)
}

func (s *ConsoleService) ReadEM100TestRecord(ctx context.Context, deviceID string) (model.EM100RecordReadResponse, error) {
	deviceID = strings.TrimSpace(deviceID)
	if deviceID == "" {
		return model.EM100RecordReadResponse{}, fmt.Errorf("缺少设备 ID")
	}
	return s.backend.ReadEM100TestRecord(ctx, deviceID)
}

func formatDetailTimestamp(timestampMS uint64) string {
	if timestampMS == 0 {
		return "-"
	}
	return time.UnixMilli(int64(timestampMS)).Local().Format("2006-01-02 15:04:05")
}

func deviceDetailStatusSummary(status model.DeviceStatus, hasStatus bool) string {
	if !hasStatus {
		return "暂无采集状态"
	}
	if message := firstSpecificRealtimeRowMessage(diagnosisMessage(status.Diagnosis), status.LastErrorMessage); message != "" {
		return message
	}
	if status.Online && status.LastCollectSuccess {
		return "采集正常"
	}
	if !status.Online {
		return "设备离线或暂无有效数据"
	}
	return "状态待确认"
}

func decorateDeviceDetailPoints(templateID string, templateName string, points []model.PointValue) []model.PointValue {
	if !isEM100Template(templateID, templateName) || len(points) == 0 {
		return decoratePointDisplayNames(points)
	}

	byKey := make(map[string]model.PointValue, len(points))
	for _, point := range points {
		byKey[pointKey(point)] = point
	}

	decorated := make([]model.PointValue, 0, len(points)+2)
	skip := map[string]bool{
		"motor_stop_year":   true,
		"motor_stop_month":  true,
		"motor_stop_day":    true,
		"motor_stop_hour":   true,
		"motor_stop_minute": true,
		"motor_stop_second": true,
		"last_test_year":    true,
		"last_test_month":   true,
		"last_test_day":     true,
		"last_test_hour":    true,
		"last_test_minute":  true,
		"last_test_second":  true,
	}
	for _, point := range points {
		key := pointKey(point)
		if skip[key] {
			continue
		}
		switch key {
		case "current_status":
			point.Name = "当前状态"
			point.Message = strings.TrimSpace(point.DisplayText)
			if point.Message == "" {
				point.Message = fmt.Sprintf("未知状态 %.0f", point.RawValue)
			}
		case "running_status_flags":
			point.Message = strings.Join(em100RunningFlagTexts(uint16(point.RawValue)), "；")
		}
		decorated = append(decorated, point)
	}

	if motorStop := em100CombinedTimePoint(
		"motor_stop_time",
		"电机停机时间",
		byKey,
		[]string{"motor_stop_year", "motor_stop_month", "motor_stop_day", "motor_stop_hour", "motor_stop_minute", "motor_stop_second"},
		23,
	); motorStop.Key != "" {
		decorated = append(decorated, motorStop)
	}
	if lastTest := em100CombinedTimePoint(
		"last_test_time",
		"最近一次绝缘测试时间",
		byKey,
		[]string{"last_test_year", "last_test_month", "last_test_day", "last_test_hour", "last_test_minute", "last_test_second"},
		50,
	); lastTest.Key != "" {
		decorated = append(decorated, lastTest)
	}
	return decoratePointDisplayNames(decorated)
}

func decoratePointDisplayNames(points []model.PointValue) []model.PointValue {
	decorated := append([]model.PointValue(nil), points...)
	for index := range decorated {
		decorated[index].Name = displayDataItemName(pointName(decorated[index]), pointKey(decorated[index]))
	}
	return decorated
}

func isEM100Template(templateID string, templateName string) bool {
	if strings.EqualFold(strings.TrimSpace(templateID), model.EM100InsulationMonitorDeviceTemplateID) {
		return true
	}
	normalizedName := strings.TrimSpace(templateName)
	return strings.EqualFold(normalizedName, "EM100") || normalizedName == "绝缘监测"
}

func em100RunningFlagTexts(raw uint16) []string {
	if raw == 0 {
		return []string{"无报警 / 无异常标志"}
	}
	definitions := []struct {
		bit  uint
		text string
	}{
		{0, "绝缘电阻低报警"},
		{1, "剩余电流高报警"},
		{2, "高压模块工作中，内部高压发生"},
		{3, "高压隔离单元处于闭合状态"},
		{4, "检测线路上的残压高，释放超时"},
		{5, "内部高压输出低"},
		{6, "线路侧高压输出低"},
		{7, "内部高压模块工作异常"},
		{8, "直流检测模块工作异常"},
		{9, "交流检测模块工作异常"},
	}
	result := make([]string, 0, len(definitions))
	for _, definition := range definitions {
		if raw&(1<<definition.bit) != 0 {
			result = append(result, definition.text)
		}
	}
	if len(result) == 0 {
		result = append(result, "未知运行标志")
	}
	return result
}

func em100CombinedTimePoint(key string, name string, points map[string]model.PointValue, keys []string, order int) model.PointValue {
	values := make([]int, 0, len(keys))
	sampleTime := uint64(0)
	for _, itemKey := range keys {
		point, ok := points[itemKey]
		if !ok {
			return model.PointValue{}
		}
		values = append(values, int(point.RawValue))
		if point.SampleTimeMS > sampleTime {
			sampleTime = point.SampleTimeMS
		}
	}
	return model.PointValue{
		Key:          key,
		Name:         name,
		Value:        0,
		Precision:    0,
		Summary:      false,
		DisplayOrder: order,
		Quality:      "good",
		Valid:        true,
		RawValue:     0,
		Message:      formatEM100SeparatedTime(values),
		SampleTimeMS: sampleTime,
	}
}

func formatEM100SeparatedTime(values []int) string {
	if len(values) != 6 {
		return "暂无有效时间"
	}
	year, month, day, hour, minute, second := values[0], values[1], values[2], values[3], values[4], values[5]
	if year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59 {
		return "暂无有效时间"
	}
	return fmt.Sprintf("%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second)
}
