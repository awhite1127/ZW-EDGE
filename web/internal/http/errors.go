package httpserver

import (
	"edge-web/internal/model"
	"strings"
)

// diagnosisHasUserMessage 判断诊断状态是否含有可展示信息。
func diagnosisHasUserMessage(diagnosis model.DiagnosisStatus) bool {
	code := strings.TrimSpace(diagnosis.ErrorCode)
	if code == "" || strings.EqualFold(code, "NONE") {
		return false
	}
	return strings.TrimSpace(diagnosis.Message) != ""
}

// diagnosisUserMessage 从诊断状态中提取用户可见信息。
func diagnosisUserMessage(diagnosis model.DiagnosisStatus) string {
	if !diagnosisHasUserMessage(diagnosis) {
		return ""
	}
	return strings.TrimSpace(diagnosis.Message)
}

// localizedStatusErrorMessage 优先使用诊断信息，否则转换备用错误。
func localizedStatusErrorMessage(diagnosis model.DiagnosisStatus, fallback string) string {
	fallback = strings.TrimSpace(fallback)
	if message := diagnosisUserMessage(diagnosis); message != "" {
		return message
	}
	return optionalUserVisibleErrorText(fallback)
}

// API 成功响应中也可能携带后端诊断文本，因此在写 JSON 前递归本地化已知页面模型。
// 未知类型原样返回，避免反射式修改业务数据。
func localizeSectionStates(states ...*model.SectionState) {
	for _, state := range states {
		state.ErrorMessage = optionalUserVisibleErrorMessage(state.ErrorMessage)
	}
}

func localizeUserFacingData(data interface{}) interface{} {
	switch value := data.(type) {
	// 基础页面及各一级页面模型。
	case model.BasePageData:
		value.ErrorMessage = optionalUserVisibleErrorMessage(value.ErrorMessage)
		if value.Flash != nil {
			flash := *value.Flash
			flash.Text = optionalUserVisibleErrorMessage(flash.Text)
			value.Flash = &flash
		}
		return value
	case model.LoginPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		return value
	case model.OverviewPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		value.SystemStatus = localizeUserFacingData(value.SystemStatus).(model.SystemStatus)
		localizeSectionStates(&value.SettingsState, &value.SystemStatusState, &value.ConfigState, &value.RecentErrorState, &value.EventsState, &value.ActiveAlarmsState)
		value.RecentError = localizeUserFacingData(value.RecentError).(model.ServiceErrorSummary)
		if value.RuntimeSnapshotAvailable {
			value.RuntimeSnapshot = localizeUserFacingData(value.RuntimeSnapshot).(model.SystemOverviewSnapshot)
		}
		value.DiagnosisCard.TypeText = optionalUserVisibleErrorText(value.DiagnosisCard.TypeText)
		value.DiagnosisCard.Suggestion = optionalUserVisibleErrorText(value.DiagnosisCard.Suggestion)
		value.DiagnosisCard.Note = optionalUserVisibleErrorText(value.DiagnosisCard.Note)
		for index := range value.RecentEvents {
			value.RecentEvents[index].Summary = optionalUserVisibleErrorText(value.RecentEvents[index].Summary)
			value.RecentEvents[index].Detail = optionalUserVisibleErrorText(value.RecentEvents[index].Detail)
		}
		return value
	case model.OverviewDiagnosisResponse:
		value.ErrorMessage = optionalUserVisibleErrorMessage(value.ErrorMessage)
		value.Card.TypeText = optionalUserVisibleErrorText(value.Card.TypeText)
		value.Card.Suggestion = optionalUserVisibleErrorText(value.Card.Suggestion)
		value.Card.Note = optionalUserVisibleErrorText(value.Card.Note)
		return value
	// 系统概览快照及设置页运行状态。
	case model.SystemOverviewSnapshot:
		value.Health.Message = optionalUserVisibleErrorMessage(value.Health.Message)
		value.Process.ErrorMessage = optionalUserVisibleErrorMessage(value.Process.ErrorMessage)
		value.Storage.ErrorMessage = optionalUserVisibleErrorMessage(value.Storage.ErrorMessage)
		value.SystemStatus = localizeUserFacingData(value.SystemStatus).(model.SystemStatus)
		value.Polling = localizeUserFacingData(value.Polling).(model.PollingCycleSummary)
		value.CurrentError = localizeUserFacingData(value.CurrentError).(model.ServiceErrorSummary)
		value.MqttRuntime = localizeUserFacingData(value.MqttRuntime).(model.MqttRuntimeStatus)
		value.ModbusServerRuntime = localizeUserFacingData(value.ModbusServerRuntime).(model.ModbusServerRuntimeStatus)
		return value
	case model.EventsPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		localizeSectionStates(&value.EventsState, &value.ActiveAlarmsState, &value.AlarmRulesState)
		return value
	case model.SettingsPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		localizeSectionStates(&value.SettingsState, &value.TimeSettingsState, &value.TimeRuntimeState, &value.NetworkState, &value.NetworkRuntimeState, &value.MqttState, &value.MqttRuntimeState, &value.DeviceTemplateState)
		value.TimeRuntime = localizeUserFacingData(value.TimeRuntime).(model.TimeRuntimeStatus)
		value.NetworkRuntime = localizeUserFacingData(value.NetworkRuntime).(model.NetworkRuntimeStatus)
		value.MqttRuntime = localizeUserFacingData(value.MqttRuntime).(model.MqttRuntimeStatus)
		return value
	case model.ModbusServerPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		value.Snapshot = localizeUserFacingData(value.Snapshot).(model.ModbusServerPageSnapshot)
		localizeSectionStates(&value.SnapshotState, &value.PointsState)
		return value
	case model.OperationsPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		localizeSectionStates(&value.UsersState, &value.MaintenanceState)
		return value
	case model.SettingsRuntimeStatusResponse:
		value.NetworkRuntimeError = optionalUserVisibleErrorMessage(value.NetworkRuntimeError)
		value.MqttRuntimeError = optionalUserVisibleErrorMessage(value.MqttRuntimeError)
		value.TimeRuntimeError = optionalUserVisibleErrorMessage(value.TimeRuntimeError)
		value.TimeRuntime = localizeUserFacingData(value.TimeRuntime).(model.TimeRuntimeStatus)
		value.NetworkRuntime = localizeUserFacingData(value.NetworkRuntime).(model.NetworkRuntimeStatus)
		value.MqttRuntime = localizeUserFacingData(value.MqttRuntime).(model.MqttRuntimeStatus)
		return value
	case model.TimeRuntimeStatus:
		value.LastErrorMessage = optionalUserVisibleErrorMessage(value.LastErrorMessage)
		return value
	case model.NetworkRuntimeStatus:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.MqttRuntimeStatus:
		value.LastErrorMessage = optionalUserVisibleErrorMessage(value.LastErrorMessage)
		value.LastPublishErrorMessage = optionalUserVisibleErrorMessage(value.LastPublishErrorMessage)
		return value
	case model.ModbusServerRuntimeStatus:
		value.LastErrorMessage = optionalUserVisibleErrorMessage(value.LastErrorMessage)
		return value
	case model.ModbusServerPageSnapshot:
		value.RuntimeStatus = localizeUserFacingData(value.RuntimeStatus).(model.ModbusServerRuntimeStatus)
		return value
	// 采集配置、历史数据与实时数据页面。
	case model.ChannelsPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		localizeSectionStates(&value.SerialPortsState)
		for index := range value.Rows {
			value.Rows[index].Status.LastErrorMessage = localizedStatusErrorMessage(
				value.Rows[index].Status.Diagnosis,
				value.Rows[index].Status.LastErrorMessage,
			)
		}
		return value
	case model.CommunicationTracesPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		return value
	case model.CollectionPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		value.Channels = localizeUserFacingData(value.Channels).(model.ChannelsPageData)
		value.Masters = localizeUserFacingData(value.Masters).(model.MastersPageData)
		value.Devices = localizeUserFacingData(value.Devices).(model.DevicesPageData)
		value.WarningMessage = optionalUserVisibleErrorMessage(value.WarningMessage)
		return value
	case model.MastersPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		for index := range value.Rows {
			value.Rows[index].Status.LastErrorMessage = localizedStatusErrorMessage(
				value.Rows[index].Status.Diagnosis,
				value.Rows[index].Status.LastErrorMessage,
			)
		}
		return value
	case model.DevicesPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		for index := range value.Rows {
			value.Rows[index].Status.LastErrorMessage = localizedStatusErrorMessage(
				value.Rows[index].Status.Diagnosis,
				value.Rows[index].Status.LastErrorMessage,
			)
		}
		return value
	case model.HistoryOverviewPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		localizeSectionStates(&value.MaintenanceState)
		return value
	case model.DeviceHistoryPageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		localizeSectionStates(&value.HistoryState)
		return value
	case model.RealtimePageData:
		value.BasePageData = localizeUserFacingData(value.BasePageData).(model.BasePageData)
		value.Dashboard = localizeRealtimeDashboard(value.Dashboard)
		for index := range value.Rows {
			value.Rows[index].ErrorMessage = localizedStatusErrorMessage(
				value.Rows[index].Diagnosis,
				value.Rows[index].ErrorMessage,
			)
		}
		return value
	case model.RealtimeViewResponse:
		value.ErrorMessage = optionalUserVisibleErrorMessage(value.ErrorMessage)
		value.Dashboard = localizeRealtimeDashboard(value.Dashboard)
		for index := range value.Rows {
			value.Rows[index].ErrorMessage = localizedStatusErrorMessage(
				value.Rows[index].Diagnosis,
				value.Rows[index].ErrorMessage,
			)
		}
		return value
	// 后端共享状态与操作结果。
	case model.SystemStatus:
		value.LastStatusMessage = optionalUserVisibleErrorMessage(value.LastStatusMessage)
		value.LastPollCycleErrorMessage = localizedStatusErrorMessage(
			value.Diagnosis,
			value.LastPollCycleErrorMessage,
		)
		for index := range value.ChannelStatusList {
			value.ChannelStatusList[index].LastErrorMessage = localizedStatusErrorMessage(
				value.ChannelStatusList[index].Diagnosis,
				value.ChannelStatusList[index].LastErrorMessage,
			)
		}
		for index := range value.MasterStatusList {
			value.MasterStatusList[index].LastErrorMessage = localizedStatusErrorMessage(
				value.MasterStatusList[index].Diagnosis,
				value.MasterStatusList[index].LastErrorMessage,
			)
		}
		for index := range value.DeviceStatusList {
			value.DeviceStatusList[index].LastErrorMessage = localizedStatusErrorMessage(
				value.DeviceStatusList[index].Diagnosis,
				value.DeviceStatusList[index].LastErrorMessage,
			)
		}
		return value
	case model.PollingCycleSummary:
		value.LastCycleErrorMessage = localizedStatusErrorMessage(value.Diagnosis, value.LastCycleErrorMessage)
		return value
	case model.ServiceErrorSummary:
		value.Message = localizedStatusErrorMessage(value.Diagnosis, value.Message)
		return value
	case model.ActionFeedback:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.SystemSettingsUpdateResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.NetworkSettingsUpdateResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.MqttSettingsUpdateResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.NetworkApplyResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.ChannelConfigUpdateResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		value.WarningMessage = optionalUserVisibleErrorMessage(value.WarningMessage)
		return value
	case model.ChannelConfigDeleteResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.MasterConfigUpdateResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		value.WarningMessage = optionalUserVisibleErrorMessage(value.WarningMessage)
		return value
	case model.MasterConfigDeleteResult:
		value.Message = optionalUserVisibleErrorMessage(value.Message)
		return value
	case model.ModbusWriteMultipleRegistersResponse:
		value.ErrorMessage = optionalUserVisibleErrorMessage(value.ErrorMessage)
		return value
	case model.DeviceCommandExecuteResponse:
		value.WriteResult = localizeUserFacingData(value.WriteResult).(model.ModbusWriteMultipleRegistersResponse)
		return value
	// API 直接返回的各类列表数据。
	case []model.ChannelRow:
		for index := range value {
			value[index].Status.LastErrorMessage = localizedStatusErrorMessage(
				value[index].Status.Diagnosis,
				value[index].Status.LastErrorMessage,
			)
		}
		return value
	case []model.MasterRow:
		for index := range value {
			value[index].Status.LastErrorMessage = localizedStatusErrorMessage(
				value[index].Status.Diagnosis,
				value[index].Status.LastErrorMessage,
			)
		}
		return value
	case []model.DeviceRow:
		for index := range value {
			value[index].Status.LastErrorMessage = localizedStatusErrorMessage(
				value[index].Status.Diagnosis,
				value[index].Status.LastErrorMessage,
			)
		}
		return value
	case []model.RealtimeRow:
		for index := range value {
			value[index].ErrorMessage = localizedStatusErrorMessage(value[index].Diagnosis, value[index].ErrorMessage)
		}
		return value
	default:
		return data
	}
}

// localizeRealtimeDashboard 本地化实时数据看板。
func localizeRealtimeDashboard(value model.RealtimeDashboard) model.RealtimeDashboard {
	value.SystemCore.LastErrorText = localizedStatusErrorMessage(value.SystemCore.Diagnosis, value.SystemCore.LastErrorText)
	value.DeviceSummary.ErrorMessage = localizedStatusErrorMessage(value.DeviceSummary.Diagnosis, value.DeviceSummary.ErrorMessage)
	for index := range value.Channels {
		value.Channels[index].ErrorMessage = localizedStatusErrorMessage(
			value.Channels[index].Diagnosis,
			value.Channels[index].ErrorMessage,
		)
	}
	for index := range value.Masters {
		value.Masters[index].ErrorMessage = localizedStatusErrorMessage(
			value.Masters[index].Diagnosis,
			value.Masters[index].ErrorMessage,
		)
	}
	return value
}
