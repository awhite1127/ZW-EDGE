package httpserver

// 本文件处理 HTML 页面请求：解析查询参数、调用 ConsoleService 组装视图并统一渲染模板。

import (
	"log"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"edge-web/internal/model"
)

// handleOverviewPage 处理概览页面请求。
func (s *Server) handleOverviewPage(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" && r.URL.Path != "/overview" {
		http.NotFound(w, r)
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := s.console.LoadOverview(ctx)
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"系统概览",
			"overview",
			"查看后端运行状态、采集概况、配置规模和关键事件。",
			r,
		),
		pageData.BasePageData,
	)
	applySystemDisplayName(&pageData.BasePageData, pageData.Settings)

	if !pageData.SystemStatusState.Available &&
		!pageData.ConfigState.Available {
		pageData.BackendReachable = false
	}

	s.renderPage(w, "overview", pageData)
}

// handleCommunicationTracesPage 处理通讯报文页面请求。
func (s *Server) handleCommunicationTracesPage(w http.ResponseWriter, r *http.Request) {
	channelID := strings.TrimSpace(r.PathValue("id"))
	if channelID == "" {
		http.NotFound(w, r)
		return
	}

	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := model.CommunicationTracesPageData{
		BasePageData: s.basePageData(
			"通道通讯报文",
			"collection",
			"查看当前通道最近 Modbus 请求 / 响应记录，用于现场通讯诊断。",
			r,
		),
		ChannelID: channelID,
		BackURL:   s.pageBackURL(r, "/collection#channels"),
	}
	s.applySystemDisplayNameFromBackend(ctx, &pageData.BasePageData)
	s.applyPollingState(ctx, &pageData.BasePageData)
	s.renderPage(w, "communication_traces", pageData)
}

// handleCollectionPage 处理采集管理页面请求。
func (s *Server) handleCollectionPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	base := s.basePageData(
		"采集管理",
		"collection",
		"统一管理通信通道、采集主站和按显式数量生成的设备。",
		r,
	)

	collection := s.console.LoadCollection(ctx)
	channels := collection.Channels
	masters := collection.Masters
	devices := collection.Devices

	s.applySystemDisplayNameFromBackend(ctx, &base)
	base.PollingKnown = collection.PollingKnown
	base.PollingRunning = collection.PollingRunning
	base.PollingState = collection.PollingState

	pageData := model.CollectionPageData{
		BasePageData: base,
		Channels: model.ChannelsPageData{
			BasePageData:      base,
			Rows:              channels.Rows,
			SerialPorts:       channels.SerialPorts,
			SerialPortsState:  channels.SerialPortsState,
			ShowPollingNotice: false,
		},
		Masters: model.MastersPageData{
			BasePageData:      base,
			Rows:              masters.Rows,
			Channels:          masters.ChannelOptions,
			DeviceTemplates:   masters.DeviceTemplates,
			ShowPollingNotice: false,
		},
		Devices: model.DevicesPageData{
			BasePageData:      base,
			Rows:              devices.Rows,
			ShowPollingNotice: false,
			ChannelCount:      len(channels.Rows),
			MasterCount:       len(masters.Rows),
		},
		WarningMessage: combineWarningMessages(
			channels.WarningMessage,
			masters.WarningMessage,
			devices.WarningMessage,
		),
		ChannelCount: len(channels.Rows),
		MasterCount:  len(masters.Rows),
		DeviceCount:  len(devices.Rows),
	}
	pageData.ErrorMessage = pageData.WarningMessage
	pageData.BackendReachable = channels.BackendReachable && masters.BackendReachable && devices.BackendReachable
	pageData.Channels.BackendReachable = channels.BackendReachable
	pageData.Channels.ErrorMessage = channels.WarningMessage
	pageData.Masters.BackendReachable = masters.BackendReachable
	pageData.Masters.ErrorMessage = masters.WarningMessage
	pageData.Devices.BackendReachable = devices.BackendReachable
	pageData.Devices.ErrorMessage = devices.WarningMessage

	s.renderPage(w, "collection", pageData)
}

// combineWarningMessages 合并并去重页面警告信息。
func combineWarningMessages(messages ...string) string {
	seen := make(map[string]struct{}, len(messages))
	combined := make([]string, 0, len(messages))
	for _, message := range messages {
		message = strings.TrimSpace(message)
		if message == "" {
			continue
		}
		if _, ok := seen[message]; ok {
			continue
		}
		seen[message] = struct{}{}
		combined = append(combined, message)
	}
	return strings.Join(combined, "；")
}

// handleHistoryOverviewPage 处理历史数据概览页面请求。
func (s *Server) handleHistoryOverviewPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := s.console.LoadHistoryOverview(ctx, model.HistoryOverviewQuery{
		ChannelID: strings.TrimSpace(r.URL.Query().Get("channel_id")),
		MasterID:  strings.TrimSpace(r.URL.Query().Get("master_id")),
		DeviceID:  strings.TrimSpace(r.URL.Query().Get("device_id")),
	})
	warningMessage := pageData.ErrorMessage
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"历史数据",
			"history",
			"按设备查看历史趋势摘要，并进入单设备曲线详情。",
			r,
		),
		pageData.BasePageData,
	)
	s.applySystemDisplayNameFromBackend(ctx, &pageData.BasePageData)
	pageData.ErrorMessage = warningMessage
	s.renderPage(w, "history", pageData)
}

// handleSettingsDeviceTypesPage 处理设置设备类型页面请求。
func (s *Server) handleSettingsDeviceTypesPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := s.console.LoadDeviceTemplateSettings(ctx, parseTemplatePageQuery(r))
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"设备类型管理",
			"settings",
			"管理内置和自定义 Modbus 设备类型，并配置实时展示与历史记录。",
			r,
		),
		pageData.BasePageData,
	)
	pageData.SettingsReturnPath = "/settings/device-types?template_page=" + strconv.Itoa(pageData.DeviceTemplatePagination.Page)
	applySystemDisplayName(&pageData.BasePageData, pageData.Settings)
	s.renderPage(w, "settings_device_types", pageData)
}

// handleDeviceTemplateEditorPage 渲染设备类型编辑器页面。
func (s *Server) handleDeviceTemplateEditorPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	returnPage, _ := strconv.Atoi(r.URL.Query().Get("return_page"))
	if returnPage < 1 {
		returnPage = 1
	}
	templateID := strings.TrimSpace(r.URL.Query().Get("id"))
	settings, definition, found, err := s.console.LoadDeviceTemplateEditorSettings(ctx, returnPage, templateID)
	data := model.DeviceTemplateEditorPageData{
		ReturnPath: "/settings/device-types?template_page=" + strconv.Itoa(settings.DeviceTemplatePagination.Page),
	}
	if templateID != "" {
		if err != nil {
			http.Error(w, "设备类型暂时无法读取", http.StatusServiceUnavailable)
			return
		}
		if !found {
			http.Error(w, "设备类型不存在或当前不可编辑", http.StatusNotFound)
			return
		}
		data.Definition = definition
		data.Editing = true
	}
	title := "新增自定义设备类型"
	if data.Editing {
		title = "编辑自定义设备类型"
	}
	data.BasePageData = mergeBasePageData(
		s.basePageData(title, "settings", "分两步配置读取模型、数据项和实时展示分组。", r),
		settings.BasePageData,
	)
	applySystemDisplayName(&data.BasePageData, settings.Settings)
	s.renderPage(w, "device_template_editor", data)
}

// handleDeviceHistoryPage 处理设备历史数据页面请求。
func (s *Server) handleDeviceHistoryPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	deviceID := strings.TrimSpace(r.URL.Query().Get("device_id"))
	period := strings.TrimSpace(r.URL.Query().Get("period"))
	pointKey := strings.TrimSpace(r.URL.Query().Get("point_key"))
	result := s.console.LoadDeviceHistory(ctx, deviceID, period, pointKey)
	pageData := result.PageData
	warningMessage := pageData.ErrorMessage
	pageData.BasePageData = s.basePageData(
		"设备历史数据",
		"history",
		"查看单个设备数据项历史趋势和明细。",
		r,
	)
	pageData.BackURL = s.pageBackURL(r, "/history")
	s.applySystemDisplayNameFromBackend(ctx, &pageData.BasePageData)
	pageData.BackendReachable = result.BackendReachable
	pageData.ErrorMessage = warningMessage
	s.renderPage(w, "device_history", pageData)
}

// pageBackURL 返回页面操作完成后的安全返回地址。
func (s *Server) pageBackURL(r *http.Request, fallback string) string {
	session, _ := s.sessionForRequest(r)
	values := r.URL.Query()
	if raw := strings.TrimSpace(values.Get("return")); raw != "" {
		return safePageBackPath(raw, fallback, session.Role)
	}
	if raw := strings.TrimSpace(values.Get("back")); raw != "" {
		return safePageBackPath(raw, fallback, session.Role)
	}
	return fallback
}

// safePageBackPath 校验并规范化站内页面返回路径。
func safePageBackPath(raw string, fallback string, role string) string {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "" || u.Host != "" || u.Path == "" || u.Path[0] != '/' ||
		strings.HasPrefix(u.Path, "//") || strings.Contains(u.Path, "\\") {
		return fallback
	}
	metadata, ok := pageRouteForPath(u.Path)
	if !ok || !metadata.AllowSecondaryPageReturn || !pageRouteAllowedForRole(metadata, role) {
		return fallback
	}
	target := u.Path
	if u.RawQuery != "" {
		target += "?" + u.RawQuery
	}
	if u.Fragment != "" {
		target += "#" + u.Fragment
	}
	return target
}

// handleRealtimePage 处理实时数据页面请求。
func (s *Server) handleRealtimePage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	result := s.console.LoadRealtime(ctx)
	pageData := model.RealtimePageData{
		BasePageData: s.basePageData(
			"实时监控",
			"realtime",
			"按完整设备清单展示实时数据，并持续同步当前刷新状态。",
			r,
		),
		Rows:      result.Rows,
		Dashboard: result.Dashboard,
	}
	if result.PrimaryDataAvailable {
		pageData.RefreshedAt = time.Now().Format("2006-01-02 15:04:05")
	}
	s.applySystemDisplayNameFromBackend(ctx, &pageData.BasePageData)
	pageData.ErrorMessage = result.WarningMessage
	pageData.BackendReachable = result.BackendReachable
	s.renderPage(w, "realtime", pageData)
}

// handleEventsPage 处理事件页面请求。
func (s *Server) handleEventsPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	liveRefresh := isAlarmLiveRefreshRequest(r)
	var pageData model.EventsPageData
	if liveRefresh {
		// 自动刷新只需要活动告警，不重复读取历史事件、设备类型和告警规则。
		pageData = s.console.LoadActiveAlarmRefresh(ctx)
	} else {
		pageData = s.console.LoadEvents(ctx, parseEventsPageQuery(r))
	}
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"事件告警",
			"events",
			"集中查看当前数据告警、历史事件，并配置设备数据项的上下限告警规则。",
			r,
		),
		pageData.BasePageData,
	)
	if !liveRefresh {
		// XHR 响应只解析告警片段，布局中的系统名称不会进入当前页面 DOM。
		s.applySystemDisplayNameFromBackend(ctx, &pageData.BasePageData)
	}
	s.renderPage(w, "events", pageData)
}

// isAlarmLiveRefreshRequest 判断是否为告警页的局部自动刷新。
func isAlarmLiveRefreshRequest(r *http.Request) bool {
	if r == nil || !strings.EqualFold(strings.TrimSpace(r.Header.Get("X-Requested-With")), "XMLHttpRequest") {
		return false
	}
	switch strings.ToLower(strings.TrimSpace(r.URL.Query().Get("tab"))) {
	case "", "active", "rules", "alarms":
		return true
	default:
		return false
	}
}

// handleSettingsPage 处理设置页面请求。
func (s *Server) handleSettingsPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := s.console.LoadSettings(ctx)
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"系统设置",
			"settings",
			"配置设备运行参数与系统基础功能。",
			r,
		),
		pageData.BasePageData,
	)
	applySystemDisplayName(&pageData.BasePageData, pageData.Settings)
	s.renderPage(w, "settings", pageData)
}

// handleSettingsMqttPage 处理设置MQTT页面请求。
func (s *Server) handleSettingsMqttPage(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := s.console.WithTimeout(r.Context())
	defer cancel()

	pageData := s.console.LoadMqttSettingsPage(ctx)
	pageData.BasePageData = mergeBasePageData(
		s.basePageData(
			"MQTT 北向配置",
			"settings",
			"维护 Broker、身份、发布参数以及 TLS 证书配置。",
			r,
		),
		pageData.BasePageData,
	)
	pageData.SettingsReturnPath = "/settings/mqtt"
	applySystemDisplayName(&pageData.BasePageData, pageData.Settings)
	s.renderPage(w, "settings_mqtt", pageData)
}

// parseTemplatePageQuery 解析设备类型页面查询条件。
func parseTemplatePageQuery(r *http.Request) int {
	page, _ := strconv.Atoi(r.URL.Query().Get("template_page"))
	return page
}

// parseEventsPageQuery 解析事件页面查询条件。
func parseEventsPageQuery(r *http.Request) model.EventsPageQuery {
	query := r.URL.Query()
	page, _ := strconv.Atoi(query.Get("page"))
	pageSize, _ := strconv.Atoi(query.Get("page_size"))
	return model.EventsPageQuery{
		Tab:       query.Get("tab"),
		Level:     query.Get("level"),
		Source:    query.Get("source"),
		Search:    query.Get("q"),
		TimeRange: query.Get("range"),
		Page:      page,
		PageSize:  pageSize,
	}
}

// handleLoginPage 处理登录页面请求。
func (s *Server) handleLoginPage(w http.ResponseWriter, r *http.Request) {
	if s.isAuthenticated(r) {
		session, _ := s.sessionForRequest(r)
		http.Redirect(w, r, safeRedirectPathForRole(r.URL.Query().Get("redirect"), session.Role), http.StatusSeeOther)
		return
	}

	firstBootEntryAvailable := false
	if s.getFirstBootAdminEntryStatus != nil {
		ctx, cancel := s.withConsoleTimeout(r.Context())
		status, err := s.getFirstBootAdminEntryStatus(ctx)
		cancel()
		if err != nil {
			log.Printf("读取首次部署入口状态失败：%s", truncateLogText(err.Error()))
		} else {
			firstBootEntryAvailable = status.Available
		}
	}

	pageData := model.LoginPageData{
		BasePageData: s.basePageData(
			"登录",
			"login",
			"请输入控制台访问密码后继续操作。",
			r,
		),
		Redirect:                     safeRedirectPath(r.URL.Query().Get("redirect")),
		FirstBootAdminEntryAvailable: firstBootEntryAvailable,
	}
	s.renderPage(w, "login", pageData)
}
