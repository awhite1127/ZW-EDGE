package service

// 本文件集中维护面向中文界面的状态、协议、时间和诊断文案转换；不参与运行态决策。

import (
	"edge-web/internal/model"
	"fmt"
	"math"
	"strings"
	"time"
)

// displayNameOrID 生成稳定的对象标识。
func displayNameOrID(name string, id string) string {
	name = strings.TrimSpace(name)
	if name != "" {
		return name
	}
	return strings.TrimSpace(id)
}

// historyRecordTimeText 格式化历史记录的完整采样时间。
func historyRecordTimeText(record model.HistoryRecord) string {
	if record.BucketStartMS != 0 {
		return time.UnixMilli(int64(record.BucketStartMS)).Local().Format(historyBucketTimeLayout(record.SamplePeriod))
	}
	if record.TimestampMS != 0 {
		return time.UnixMilli(int64(record.TimestampMS)).Local().Format("2006-01-02 15:04:05")
	}
	if text := strings.TrimSpace(record.BucketText); text != "" {
		return text
	}
	return strings.TrimSpace(record.Date)
}

// historyBucketTimeLayout 返回历史时间桶使用的日期格式。
func historyBucketTimeLayout(samplePeriod string) string {
	switch strings.ToLower(strings.TrimSpace(samplePeriod)) {
	case "day":
		return "2006-01-02"
	case "raw_10min":
		return "2006-01-02 15:04"
	default:
		return "2006-01-02 15:00"
	}
}

// historyRecordDateText 格式化历史记录的采样日期。
func historyRecordDateText(record model.HistoryRecord) string {
	if record.BucketStartMS != 0 {
		return time.UnixMilli(int64(record.BucketStartMS)).Local().Format("2006-01-02")
	}
	if record.TimestampMS != 0 {
		return time.UnixMilli(int64(record.TimestampMS)).Local().Format("2006-01-02")
	}
	return defaultString(record.Date, historyRecordTimeText(record))
}

// formatHistoryValue 格式化历史数据值。
func formatHistoryValue(value float64, precision uint32, unit string) string {
	if math.IsNaN(value) || math.IsInf(value, 0) {
		return "-"
	}
	text := fmt.Sprintf("%.*f", int(precision), value)
	unit = strings.TrimSpace(unit)
	if unit == "" {
		return text
	}
	return text + " " + unit
}

// backendStateText 返回后端连接状态文本。
func backendStateText(reachable bool) string {
	if reachable {
		return "后端可达"
	}
	return "后端服务未连接"
}

// pollingStateLabel 返回轮询运行阶段的中文标签。
func pollingStateLabel(state string, running bool) string {
	switch strings.ToLower(strings.TrimSpace(state)) {
	case "running":
		return "轮询运行中"
	case "fault":
		return "采集异常"
	case "stopping":
		return "轮询停止中"
	case "config_applying":
		return "配置应用中"
	case "polling_rebuilding":
		return "轮询重建中"
	case "stopped", "":
		if running {
			return "轮询运行中"
		}
		return "轮询已停止"
	default:
		if running {
			return "轮询运行中"
		}
		return "轮询已停止"
	}
}

// configuredChannelPort 返回通道配置对应的端口描述。
func configuredChannelPort(channel model.ChannelConfig) string {
	if channel.DevicePath != "" {
		return channel.DevicePath
	}
	return channel.PortName
}

// channelTypeText 返回通信通道类型的中文名称。
func channelTypeText(channelType string) string {
	switch strings.ToLower(strings.TrimSpace(channelType)) {
	case "modbus_tcp":
		return "Modbus TCP"
	case "modbus_rtu_serial":
		return "Modbus RTU 串口"
	default:
		return defaultString(channelType, "未知通道")
	}
}

// masterProtocolText 返回主站协议的展示名称。
func masterProtocolText(protocol string) string {
	switch strings.ToLower(strings.TrimSpace(protocol)) {
	case "modbus_tcp":
		return "Modbus TCP"
	case "modbus_rtu":
		return "Modbus RTU"
	default:
		return defaultString(protocol, "未知协议")
	}
}

// channelEndpointText 返回通道串口或网络端点信息。
func channelEndpointText(channel model.ChannelConfig) string {
	if strings.EqualFold(strings.TrimSpace(channel.ChannelType), "modbus_tcp") {
		host := strings.TrimSpace(channel.TCPHost)
		if host == "" {
			host = "未配置远端"
		}
		port := channel.TCPPort
		if port == 0 {
			port = 502
		}
		return fmt.Sprintf("%s:%d", host, port)
	}
	return defaultString(configuredChannelPort(channel), "未配置串口")
}

// channelParamText 汇总通道的关键通信参数。
func channelParamText(channel model.ChannelConfig) string {
	if strings.EqualFold(strings.TrimSpace(channel.ChannelType), "modbus_tcp") {
		connectTimeout := channel.ConnectTimeoutMS
		if connectTimeout == 0 {
			connectTimeout = 3000
		}
		return fmt.Sprintf("建连 %dms / 响应 %dms / %d次", connectTimeout, channel.ResponseTimeoutMS, channel.RetryCount)
	}
	return fmt.Sprintf("%d / %dms / %d次", channel.BaudRate, channel.ResponseTimeoutMS, channel.RetryCount)
}

// pointText 格式化点位值、精度和单位。
func pointText(point *model.PointValue, hasValue bool) string {
	if !hasValue || point == nil {
		return "暂无数据"
	}
	return pointTextValue(*point)
}

// pointTextValue 格式化点位数值及工程单位。
func pointTextValue(point model.PointValue) string {
	return pointTextValueForTemplate("", "", point)
}

// pointTextValueForTemplate 按设备类型格式化点位值。
func pointTextValueForTemplate(templateID string, templateName string, point model.PointValue) string {
	if isEM100Template(templateID, templateName) && pointKey(point) == "current_status" {
		if strings.TrimSpace(point.DisplayText) != "" {
			return strings.TrimSpace(point.DisplayText)
		}
		return fmt.Sprintf("未知状态 %.0f", point.RawValue)
	}
	if strings.TrimSpace(point.DisplayText) != "" {
		return fmt.Sprintf("%s（%.0f）", strings.TrimSpace(point.DisplayText), point.RawValue)
	}
	precision := int(point.Precision)
	if precision > 6 {
		precision = 6
	}
	text := fmt.Sprintf("%.*f", precision, point.Value)
	unit := strings.TrimSpace(point.Unit)
	if unit != "" {
		return text + " " + unit
	}
	return text
}

// pointKey 生成稳定的对象标识。
func pointKey(point model.PointValue) string {
	return point.Key
}

// pointName 返回点位的中文显示名称。
func pointName(point model.PointValue) string {
	return point.Name
}

// historyDataItemDisplayName 返回历史数据项的显示名称。
func historyDataItemDisplayName(name string, key string, unit string) string {
	displayName := displayDataItemName(name, key)
	unit = strings.TrimSpace(unit)
	if unit == "" {
		return displayName
	}
	return displayName + " / " + unit
}

// displayDataItemName 返回数据项的中文显示名称。
func displayDataItemName(name string, key string) string {
	name = strings.TrimSpace(name)
	key = strings.TrimSpace(key)
	if name != "" && !looksLikeInternalFieldName(name) {
		return name
	}
	if text := knownDataItemName(key); text != "" {
		return text
	}
	if text := knownDataItemName(name); text != "" {
		return text
	}
	return "数据项"
}

// functionCodeText 返回 Modbus 功能码的中文说明。
func functionCodeText(code uint32) string {
	if code == 0 {
		return "-"
	}
	return fmt.Sprintf("FC%02X", code)
}

// knownDataItemName 返回已知数据项的固定中文名称。
func knownDataItemName(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "resistance", "ground_resistance":
		return "接地电阻"
	case "temperature":
		return "温度"
	case "battery_voltage":
		return "电池电压"
	case "signal_strength":
		return "信号强度"
	case "current_status":
		return "当前状态"
	case "running_status_flags":
		return "运行状态标志"
	case "motor_stop_time":
		return "电机停机时间"
	case "last_test_time":
		return "最近一次绝缘测试时间"
	case "leakage_current":
		return "剩余电流"
	case "insulation_resistance":
		return "绝缘电阻"
	case "voltage":
		return "电压"
	case "humidity":
		return "湿度"
	default:
		return ""
	}
}

// looksLikeInternalFieldName 判断名称是否形似内部字段标识。
func looksLikeInternalFieldName(value string) bool {
	value = strings.TrimSpace(value)
	if value == "" {
		return false
	}
	hasLetter := false
	for _, char := range value {
		switch {
		case char >= 'a' && char <= 'z':
			hasLetter = true
		case char >= 'A' && char <= 'Z':
			hasLetter = true
		case char >= '0' && char <= '9':
		case char == '_':
		default:
			return false
		}
	}
	return hasLetter
}

// qualityText 返回采集质量的中文说明。
func qualityText(value string) string {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "good":
		return "良好"
	case "bad":
		return "异常"
	case "stale":
		return "过期"
	case "partial":
		return "部分通讯异常"
	case "unknown", "", "-":
		return "暂无数据"
	default:
		if hasNonASCII(value) {
			return value
		}
		return "未知"
	}
}

// hasNonASCII 判断是否具有NonASCII。
func hasNonASCII(value string) bool {
	for _, char := range value {
		if char > 127 {
			return true
		}
	}
	return false
}
