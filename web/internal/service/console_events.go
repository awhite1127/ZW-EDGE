package service

// 本文件聚合当前告警、历史事件和告警规则三个相互独立的数据源；任一数据源失败时，
// 其余标签页仍可使用，错误通过各自 SectionState 返回页面。

import (
	"context"
	"net/url"
	"strconv"
	"strings"
	"sync"

	"edge-web/internal/model"
)

// LoadEvents 加载事件。
func (s *ConsoleService) LoadEvents(ctx context.Context, query model.EventsPageQuery) model.EventsPageData {
	// 事件页由三个独立数据源组成。这里允许局部失败：某一块不可用时，其余标签页仍可展示。
	result := model.EventsPageData{
		ActiveTab:         normalizeEventsTab(query.Tab),
		EventsState:       model.SectionState{},
		ActiveAlarmsState: model.SectionState{},
		AlarmRulesState:   model.SectionState{},
		LevelFilter:       normalizeEventLevelFilter(query.Level),
		SourceFilter:      normalizeEventSourceFilter(query.Source),
		SearchQuery:       strings.TrimSpace(query.Search),
		TimeRange:         normalizeEventTimeRange(query.TimeRange),
		Page:              normalizePositiveInt(query.Page, 1),
		PageSize:          normalizeEventPageSize(query.PageSize),
		PageSizeOptions:   []int{10, 20, 50},
	}
	result.AllEventsURL = eventsPageLevelURL("all", result.SourceFilter, result.SearchQuery, result.TimeRange, result.PageSize)
	result.ErrorEventsURL = eventsPageLevelURL("error", result.SourceFilter, result.SearchQuery, result.TimeRange, result.PageSize)
	result.WarningEventsURL = eventsPageLevelURL("warning", result.SourceFilter, result.SearchQuery, result.TimeRange, result.PageSize)
	result.InfoEventsURL = eventsPageLevelURL("info", result.SourceFilter, result.SearchQuery, result.TimeRange, result.PageSize)
	result.ExportURL = eventsExportURL(result.LevelFilter, result.SourceFilter, result.SearchQuery, result.TimeRange)

	if result.ActiveTab == "history" {
		// 历史标签不读取活动告警、主站、模板和规则，避免一次页面查看触发无关 IPC。
		result.ActiveAlarmsState.Available = true
		result.AlarmRulesState.Available = true
		var wg sync.WaitGroup
		devices := startLoad(ctx, &wg, s.backend.ListDevices)
		eventHistory := startLoad(ctx, &wg, func(ctx context.Context) (model.EventHistoryResult, error) {
			return s.backend.QueryServiceEvents(ctx, model.EventHistoryQuery{
				Level:     result.LevelFilter,
				Source:    result.SourceFilter,
				Search:    result.SearchQuery,
				TimeRange: result.TimeRange,
				Page:      result.Page,
				PageSize:  result.PageSize,
			})
		})
		wg.Wait()
		applyEventsHistoryData(&result, eventHistory.value, eventHistory.err, devices.value, devices.err)
		result.BasePageData.BackendReachable = result.EventsState.Available
		return result
	}

	// 告警管理页不读取历史事件；其余相互独立的数据源并行获取。
	result.EventsState.Available = true
	var wg sync.WaitGroup
	alarms := startLoad(ctx, &wg, s.backend.ListActiveAlarms)
	devices := startLoad(ctx, &wg, s.backend.ListDevices)
	masters := startLoad(ctx, &wg, s.backend.ListMasters)
	summary := startLoad(ctx, &wg, s.backend.GetConfigSummary)
	rules := startLoad(ctx, &wg, func(ctx context.Context) ([]model.AlarmRule, error) {
		return s.backend.ListAlarmRules(ctx, "")
	})
	wg.Wait()

	applyActiveAlarmData(&result, alarms.value, alarms.err)
	applyAlarmRuleData(
		&result,
		devices.value, devices.err,
		masters.value, masters.err,
		summary.value, summary.err,
		rules.value, rules.err,
	)
	result.BasePageData.BackendReachable = result.ActiveAlarmsState.Available || result.AlarmRulesState.Available
	return result
}

// LoadActiveAlarmRefresh 仅加载自动刷新区域真正需要的活动告警。
func (s *ConsoleService) LoadActiveAlarmRefresh(ctx context.Context) model.EventsPageData {
	result := model.EventsPageData{
		ActiveTab:         "alarms",
		EventsState:       model.SectionState{Available: true},
		AlarmRulesState:   model.SectionState{Available: true},
		ActiveAlarmsState: model.SectionState{},
	}
	alarms, err := s.backend.ListActiveAlarms(ctx)
	applyActiveAlarmData(&result, alarms, err)
	result.BasePageData.BackendReachable = result.ActiveAlarmsState.Available
	return result
}

// applyActiveAlarmData 汇总活动告警数量和确认状态。
func applyActiveAlarmData(result *model.EventsPageData, alarms []model.ActiveAlarm, err error) {
	if result == nil {
		return
	}
	if err != nil {
		result.ActiveAlarmsState.ErrorMessage = err.Error()
		return
	}
	result.ActiveAlarms = alarms
	result.ActiveAlarmCount = len(alarms)
	for _, alarm := range alarms {
		if !alarm.Acknowledged {
			result.UnacknowledgedAlarmCount++
		}
	}
	// 告警管理页必须让每条活动告警都可确认；列表自身负责滚动，不截断为前五条。
	result.ActiveAlarmPreview = alarms
	result.RemainingAlarmCount = 0
	result.ActiveAlarmsState.Available = true
}

// applyEventsHistoryData 应用历史事件筛选、统计和分页。
func applyEventsHistoryData(
	result *model.EventsPageData,
	history model.EventHistoryResult,
	eventsErr error,
	devices []model.DeviceConfig,
	devicesErr error,
) {
	if result == nil {
		return
	}
	if eventsErr != nil {
		result.EventsState.ErrorMessage = eventsErr.Error()
		return
	}
	result.Events = history.Rows
	if devicesErr == nil {
		result.Events = applyCurrentDeviceNamesToEvents(result.Events, devices)
	}
	result.ErrorCount = history.LevelStats.Error
	result.WarningCount = history.LevelStats.Warning
	result.InfoCount = history.LevelStats.Info
	result.TotalEventCount = result.ErrorCount + result.WarningCount + result.InfoCount
	result.HasAnyEvents = result.TotalEventCount > 0
	result.SourceOptions = buildEventSourceOptions(history.SourceStats)

	result.FilteredEventCount = history.Total
	result.TotalPages = totalPages(result.FilteredEventCount, result.PageSize)
	if result.Page > result.TotalPages {
		result.Page = result.TotalPages
	}
	if result.Page < 1 {
		result.Page = 1
	}
	result.PrevPage = result.Page - 1
	result.NextPage = result.Page + 1
	result.HasPrevPage = result.Page > 1
	result.HasNextPage = result.Page < result.TotalPages

	if result.FilteredEventCount > 0 {
		result.PageStart = (result.Page-1)*result.PageSize + 1
		result.PageEnd = result.PageStart + len(result.Events) - 1
		if result.PageEnd > result.FilteredEventCount {
			result.PageEnd = result.FilteredEventCount
		}
	}
	result.EventsState.Available = true
}

// applyAlarmRuleData 组装按主站和数据项展示的告警规则。
func applyAlarmRuleData(
	result *model.EventsPageData,
	devices []model.DeviceConfig,
	devicesErr error,
	masters []model.MasterNodeConfig,
	mastersErr error,
	summary model.ConfigSummary,
	summaryErr error,
	rules []model.AlarmRule,
	rulesErr error,
) {
	if result == nil {
		return
	}
	if devicesErr != nil || mastersErr != nil || summaryErr != nil || rulesErr != nil {
		result.AlarmRulesState.ErrorMessage = firstNonEmptyError(devicesErr, mastersErr, summaryErr, rulesErr)
		return
	}
	// 后端规则按“设备 + 数据项”存储，页面按“主站 + 数据项”批量配置，所以这里做一次聚合视图。
	result.AlarmRuleItems, result.AlarmRuleDevices = buildAlarmRuleConfigItems(devices, masters, summary.DeviceTemplates, rules)
	result.ConfigurableCount = len(result.AlarmRuleItems)
	for _, item := range result.AlarmRuleItems {
		if item.StatusText == "已启用" {
			result.EnabledRuleCount++
		}
	}
	switch {
	case len(masters) == 0:
		result.AlarmRulesEmptyText = "尚未配置主站，请先到采集管理配置主站。"
	case len(devices) == 0:
		result.AlarmRulesEmptyText = "尚未生成设备，请检查设备类型、显式设备数量和主站配置。"
	default:
		result.AlarmRulesEmptyText = "当前设备类型没有关键数据，暂无可配置告警的数据项。"
	}
	result.AlarmRulesState.Available = true
}

// AcknowledgeActiveAlarm 确认活动告警并返回更新后的状态。
func (s *ConsoleService) AcknowledgeActiveAlarm(
	ctx context.Context,
	deviceID string,
	pointKey string,
	acknowledgedBy string,
	activeSinceMS uint64,
) model.ActionFeedback {
	deviceID = strings.TrimSpace(deviceID)
	pointKey = strings.TrimSpace(pointKey)
	acknowledgedBy = strings.TrimSpace(acknowledgedBy)
	if deviceID == "" || pointKey == "" || acknowledgedBy == "" || activeSinceMS == 0 {
		return model.ActionFeedback{Success: false, Message: "告警确认参数不完整，请刷新后重试"}
	}
	result, err := s.backend.AcknowledgeActiveAlarm(ctx, model.ActiveAlarmAcknowledgeRequest{
		DeviceID: deviceID, PointKey: pointKey, AcknowledgedBy: acknowledgedBy, ActiveSinceMS: &activeSinceMS,
	})
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	message := strings.TrimSpace(result.Message)
	if message == "" {
		message = "告警已确认"
	}
	return model.ActionFeedback{Success: true, Message: message}
}

// eventsPageLevelURL 生成指定事件级别的页面地址。
func eventsPageLevelURL(level string, source string, search string, timeRange string, pageSize int) string {
	values := url.Values{}
	values.Set("tab", "history")
	values.Set("level", normalizeEventLevelFilter(level))
	values.Set("source", normalizeEventSourceFilter(source))
	values.Set("q", strings.TrimSpace(search))
	values.Set("range", normalizeEventTimeRange(timeRange))
	values.Set("page_size", strconv.Itoa(normalizeEventPageSize(pageSize)))
	return "/events?" + values.Encode()
}

// applyCurrentDeviceNamesToEvents 把当前设备显示名称补充到事件记录。
func applyCurrentDeviceNamesToEvents(events []model.ServiceEvent, devices []model.DeviceConfig) []model.ServiceEvent {
	byID := make(map[string]model.DeviceConfig, len(devices))
	for _, device := range devices {
		byID[device.DeviceID] = device
	}
	result := append([]model.ServiceEvent(nil), events...)
	for index := range result {
		device, ok := byID[result[index].TargetID]
		if !ok {
			device, ok = byID[result[index].Diagnosis.TargetID]
		}
		if !ok || strings.TrimSpace(device.DeviceName) == "" {
			continue
		}
		if device.SystemName != "" && device.SystemName != device.DeviceName {
			result[index].Summary = strings.ReplaceAll(result[index].Summary, device.SystemName, device.DeviceName)
			result[index].Detail = strings.ReplaceAll(result[index].Detail, device.SystemName, device.DeviceName)
		}
		if result[index].Diagnosis.TargetID == device.DeviceID {
			result[index].Diagnosis.TargetName = device.DeviceName
		}
	}
	return result
}

// normalizeEventsTab 规范化事件中心当前页签。
func normalizeEventsTab(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "history":
		return "history"
	case "active", "rules", "alarms":
		return "alarms"
	default:
		return "alarms"
	}
}

// eventsExportURL 生成当前事件筛选条件的导出地址。
func eventsExportURL(level string, source string, search string, timeRange string) string {
	values := url.Values{}
	if normalized := normalizeEventLevelFilter(level); normalized != "" && normalized != "all" {
		values.Set("level", normalized)
	}
	if normalized := normalizeEventSourceFilter(source); normalized != "" {
		values.Set("source", normalized)
	}
	if trimmed := strings.TrimSpace(search); trimmed != "" {
		values.Set("q", trimmed)
	}
	if normalized := normalizeEventTimeRange(timeRange); normalized != "" && normalized != "all" {
		values.Set("range", normalized)
	}
	if len(values) == 0 {
		return "/events/diagnostics/export"
	}
	return "/events/diagnostics/export?" + values.Encode()
}

// normalizeEventPageSize 规范化事件页面大小。
func normalizeEventPageSize(value int) int {
	switch value {
	case 10, 20, 50:
		return value
	default:
		return 10
	}
}

// normalizeEventLevelFilter 规范化事件级别筛选条件。
func normalizeEventLevelFilter(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "error"
	case "warning", "warn":
		return "warning"
	case "info":
		return "info"
	default:
		return "all"
	}
}

// normalizeEventTimeRange 规范化事件时间范围。
func normalizeEventTimeRange(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "24h", "3d", "7d", "all":
		return strings.ToLower(strings.TrimSpace(value))
	default:
		return "all"
	}
}

// normalizeEventSourceFilter 规范化事件来源筛选条件。
func normalizeEventSourceFilter(source string) string {
	return strings.ToLower(strings.TrimSpace(source))
}

// buildEventSourceOptions 构建事件来源选项。
func buildEventSourceOptions(stats []model.EventSourceStat) []model.EventFilterOption {
	known := []string{"data_alarm", "alarm_ack", "config_apply", "system", "channel", "master", "device", "polling", "polling_cycle", "channel_startup", "master_collect", "system_startup", "reload_config", "initialize"}
	seen := make(map[string]bool)
	options := make([]model.EventFilterOption, 0, len(known)+len(stats))
	for _, source := range known {
		options = appendEventSourceOption(options, seen, source)
	}
	for _, stat := range stats {
		options = appendEventSourceOption(options, seen, stat.Source)
	}
	return options
}

// appendEventSourceOption 追加事件来源选项。
func appendEventSourceOption(options []model.EventFilterOption, seen map[string]bool, source string) []model.EventFilterOption {
	source = normalizeEventSourceFilter(source)
	if source == "" || seen[source] {
		return options
	}
	seen[source] = true
	return append(options, model.EventFilterOption{
		Value: source,
		Label: model.EventSourceLabel(source),
	})
}

// ClearRecentEvents 清除最近事件。
func (s *ConsoleService) ClearRecentEvents(ctx context.Context) model.ActionFeedback {
	feedback, err := s.backend.ClearRecentEvents(ctx)
	if err != nil {
		if feedback.Message == "" {
			feedback.Message = err.Error()
		}
		feedback.Success = false
		return feedback
	}
	if feedback.Message == "" {
		feedback.Message = "历史事件已清除"
	}
	feedback.Success = true
	return feedback
}
