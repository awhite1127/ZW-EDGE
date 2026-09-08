// 兼容历史事件、第三方系统输出及没有结构化诊断的旧消息；仅负责展示，不参与业务分类。
package httpserver

// 本文件把底层英文错误、IPC 状态和现场诊断信息收敛为可展示的中文错误，同时保留安全兜底文案。

import (
	"strings"
)

var internalParameterPathReplacer = strings.NewReplacer(
	"modbus_server_settings.", "",
	"modbus_register_mapping.", "",
	"listen_address", "监听地址",
	"listen_port", "监听端口",
	"unit_id", "Unit ID",
	"max_clients", "最大客户端数",
	"idle_timeout_seconds", "客户端空闲超时",
	"max_read_registers", "单次最大读取寄存器数",
	"mapping_id", "映射 ID",
	"device_id", "设备",
	"point_key", "数据项",
	"device_name_snapshot", "设备名称快照",
	"point_name_snapshot", "数据项名称快照",
	"value_multiplier", "倍率",
	"value_offset", "偏移",
	"data_type", "数据类型",
	"byte_order", "字节序",
	"word_order", "字序",
	"start_address", "数据寄存器地址",
	"quality_address", "质量寄存器地址",
	"params.realtime_grouping_enabled", "实时展示分组开关",
	"params.realtime_groups", "实时展示分组",
	"params.read_blocks", "读取区块",
	"params.write_commands", "写入命令",
	"params.fields", "数据项",
	"fields.realtime_group_id", "数据项的实时展示分组",
	"realtime_groups.id", "实时展示分组 ID",
	"realtime_groups.name", "实时展示分组名称",
)

// channelErrorText 将通道底层错误转换为可展示的中文说明。
func channelErrorText(message string) string {
	return userVisibleErrorSummaryText(message)
}

// userVisibleErrorSummaryText 提取适合页面摘要区域展示的错误信息。
func userVisibleErrorSummaryText(message string) string {
	raw := strings.TrimSpace(message)
	if raw == "" || raw == "-" {
		return "无"
	}

	normalized := stripErrorPrefix(raw)
	lower := strings.ToLower(normalized)
	switch {
	case strings.Contains(normalized, "TCP 连接失败"):
		return "TCP 连接失败：请检查远端 IP / 主机名、端口和设备 TCP 服务状态。"
	case strings.Contains(normalized, "TCP 连接超时"):
		return "TCP 连接超时：请检查网络连通性、网关配置和远端端口是否开放。"
	case strings.Contains(normalized, "TCP 发送失败"):
		return "TCP 发送失败：请检查网络链路、远端连接状态和 Modbus TCP 服务稳定性。"
	case strings.Contains(normalized, "TCP 响应超时"):
		return "TCP 响应超时：请检查 Unit ID、寄存器范围和响应超时配置。"
	case strings.Contains(normalized, "TCP 远端关闭连接"):
		return "TCP 远端关闭连接：请检查远端 Modbus TCP 服务和请求参数。"
	case strings.Contains(normalized, "MBAP"):
		return "Modbus TCP 响应异常：" + localizedMBAPErrorText(normalized)
	case strings.Contains(normalized, "Unit ID 不匹配"):
		return "Modbus TCP Unit ID 不匹配：请检查主站 Unit ID / 从站地址和网关映射。"
	case strings.Contains(normalized, "Modbus TCP 异常码"):
		return "Modbus TCP 异常响应：" + normalized
	case strings.Contains(normalized, "Byte Count 异常"), strings.Contains(normalized, "Modbus TCP 响应数据长度异常"):
		return "Modbus TCP 响应数据长度异常：请检查寄存器数量和远端响应格式。"
	case strings.Contains(lower, "permission denied"), strings.Contains(normalized, "权限不足"), strings.Contains(normalized, "访问被拒绝"):
		return "通道打开失败：当前用户没有串口访问权限，请检查 dialout 权限或设备权限配置。"
	case strings.Contains(lower, "no such file or directory"), strings.Contains(normalized, "路径不存在"), strings.Contains(normalized, "未找到串口设备"), strings.Contains(normalized, "串口设备不存在"):
		return "通道打开失败：串口设备不存在，请检查串口路径是否正确。"
	case strings.Contains(lower, "device or resource busy"), strings.Contains(lower, "device busy"), strings.Contains(lower, "port busy"), strings.Contains(normalized, "被占用"):
		return "通道打开失败：串口设备已被其他程序占用，请关闭占用程序后重试。"
	case strings.Contains(lower, "input/output error"),
		strings.Contains(lower, "tcgetattr"),
		strings.Contains(normalized, "输入输出错误"),
		strings.Contains(normalized, "读取串口属性失败"),
		strings.Contains(normalized, "配置串口参数失败"),
		strings.Contains(normalized, "打开串口失败"),
		strings.Contains(normalized, "串口设备无法打开或初始化"):
		return "通道打开失败：串口设备不可访问，请检查串口是否真实存在、虚拟机/系统是否正确映射串口、设备是否异常。"
	case strings.Contains(normalized, "串口响应超时"),
		strings.Contains(normalized, "通道响应超时"),
		strings.Contains(normalized, "主控无响应"),
		strings.Contains(lower, "response timeout"),
		strings.Contains(lower, "timeout"):
		return "主站通信超时：请检查设备接线、通信链路、Unit ID / 从站地址和协议参数配置。"
	case strings.Contains(normalized, "输入寄存器"),
		strings.Contains(lower, "fc04"),
		strings.Contains(lower, "read input"):
		return "输入寄存器读取失败"
	case strings.Contains(normalized, "保持寄存器"),
		strings.Contains(lower, "fc03"),
		strings.Contains(lower, "read holding"):
		return "保持寄存器读取失败"
	case strings.Contains(normalized, "读取寄存器失败"),
		strings.Contains(normalized, "Modbus 请求失败"):
		return "寄存器读取失败"
	case strings.Contains(normalized, "响应解析失败"),
		strings.Contains(normalized, "CRC"),
		strings.Contains(lower, "parse"):
		return "主站通信异常：响应数据校验失败或响应格式异常，请检查通信干扰、波特率、校验位和接线质量。"
	}

	return userVisibleErrorText(normalized)
}

// userVisibleErrorDetailText 转换保留必要细节的用户可见错误。
func userVisibleErrorDetailText(message string) string {
	raw := strings.TrimSpace(message)
	if raw == "" || raw == "-" {
		return "-"
	}

	normalized := stripErrorPrefix(raw)
	translated := userVisibleErrorSummaryText(normalized)
	if translated != "" && translated != "无" && translated != normalized {
		return translated
	}
	if containsChinese(normalized) && !containsASCIIWord(normalized) {
		return normalized
	}
	return fallbackForEnglishDetail(normalized, "操作失败")
}

// localizedMBAPErrorText 转换常见 MBAP 协议校验错误。
func localizedMBAPErrorText(message string) string {
	lower := strings.ToLower(message)
	switch {
	case strings.Contains(lower, "transaction id"):
		return "MBAP 事务标识不匹配"
	case strings.Contains(lower, "protocol id"):
		return "MBAP 协议标识异常"
	case strings.Contains(lower, "length"):
		return "MBAP 长度字段异常"
	default:
		return "MBAP 响应头异常"
	}
}

// translateSystemErrorDetail 翻译系统错误详情。
func translateSystemErrorDetail(lower string) string {
	switch {
	case strings.Contains(lower, "permission denied"):
		return "串口访问被拒绝"
	case strings.Contains(lower, "no such file or directory"):
		return "串口路径不存在"
	case strings.Contains(lower, "device or resource busy"):
		return "串口被占用"
	case strings.Contains(lower, "input/output error"):
		return "串口设备无法打开或初始化"
	case strings.Contains(lower, "timed out"), strings.Contains(lower, "timeout"):
		return "通信超时"
	case strings.Contains(lower, "invalid argument"):
		return "参数无效"
	default:
		return "请检查串口配置和设备状态"
	}
}

// userVisibleErrorText 返回用户可理解的错误详情文本。
func userVisibleErrorText(message string) string {
	return normalizeUserVisibleMessage(message, "无")
}

// userVisibleErrorMessage 返回用户可理解的错误摘要。
func userVisibleErrorMessage(message string) string {
	return normalizeUserVisibleMessage(message, "操作失败")
}

// optionalUserVisibleErrorText 转换非空错误详情，空值保持为空。
func optionalUserVisibleErrorText(message string) string {
	if strings.TrimSpace(message) == "" || strings.TrimSpace(message) == "-" {
		return message
	}
	return userVisibleErrorText(message)
}

// optionalUserVisibleErrorMessage 转换非空错误摘要，空值保持为空。
func optionalUserVisibleErrorMessage(message string) string {
	if strings.TrimSpace(message) == "" {
		return ""
	}
	return userVisibleErrorMessage(message)
}

// 错误本地化采用“已知前缀翻译 + 英文兜底”策略：既保留现场可操作信息，
// 又避免把 SQL、文件路径或协议内部细节直接暴露给普通页面。
func normalizeUserVisibleMessage(message string, fallback string) string {
	raw := strings.TrimSpace(message)
	if raw == "" || raw == "-" {
		return fallback
	}

	normalized := stripErrorPrefix(raw)
	normalized = localizeInternalParameterPaths(normalized)
	if translated, ok := translateKnownUserMessage(normalized); ok {
		return translated
	}

	if prefix, detail, ok := splitMessagePrefix(normalized); ok {
		if translated, detailOK := translateKnownUserMessage(detail); detailOK {
			return prefix + translated
		}
		if containsChinese(prefix) && !containsChinese(detail) {
			return prefix + fallbackForEnglishDetail(detail, fallback)
		}
	}

	if containsChinese(normalized) {
		return normalized
	}
	return fallbackForEnglishDetail(normalized, fallback)
}

// IPC 参数路径是开发定位信息，不应直接出现在页面提示中。
// 已知设备类型字段保留业务含义；未知 params 路径则降级为统一中文说明。
func localizeInternalParameterPaths(message string) string {
	localized := internalParameterPathReplacer.Replace(message)
	if strings.Contains(strings.ToLower(localized), "params.") {
		return "提交的设备类型配置参数格式不正确"
	}
	return localized
}

// stripErrorPrefix 移除底层错误消息中的已知前缀。
func stripErrorPrefix(message string) string {
	trimmed := strings.TrimSpace(message)
	if trimmed == "" {
		return trimmed
	}

	if parts := strings.SplitN(trimmed, ": ", 2); len(parts) == 2 {
		head := strings.ToLower(strings.TrimSpace(parts[0]))
		switch head {
		case "backend_error", "invalid_request", "invalid_argument", "bad_request", "internal", "error":
			return strings.TrimSpace(parts[1])
		}
	}

	return trimmed
}

// splitMessagePrefix 拆分错误消息的前缀与正文。
func splitMessagePrefix(message string) (string, string, bool) {
	separators := []string{"：", ": ", ":"}
	for _, separator := range separators {
		index := strings.LastIndex(message, separator)
		if index <= 0 {
			continue
		}
		prefix := strings.TrimSpace(message[:index])
		detail := strings.TrimSpace(message[index+len(separator):])
		if prefix == "" || detail == "" {
			continue
		}
		return prefix + "：", detail, true
	}
	return "", "", false
}

// fallbackForEnglishDetail 为未本地化的英文详情生成中文兜底文本。
func fallbackForEnglishDetail(message string, fallback string) string {
	lower := strings.ToLower(message)
	switch {
	case strings.Contains(lower, "serial"), strings.Contains(lower, "/dev/"), strings.Contains(lower, "tty"):
		return "串口通信异常"
	case strings.Contains(lower, "json"), strings.Contains(lower, "decode"), strings.Contains(lower, "encode"), strings.Contains(lower, "parse"):
		return "数据解析失败"
	case strings.Contains(lower, "connection"), strings.Contains(lower, "network"), strings.Contains(lower, "fetch"), strings.Contains(lower, "host"):
		return "网络连接异常"
	default:
		return fallback
	}
}

// translateKnownUserMessage 翻译已知用户消息。
func translateKnownUserMessage(message string) (string, bool) {
	raw := strings.TrimSpace(message)
	if raw == "" || raw == "-" {
		return "", false
	}
	// MQTT 配置保存/应用结果已经由后端生成可读中文前缀，保留其具体本地应用错误。
	if strings.HasPrefix(raw, "配置已保存，但运行应用失败：") ||
		strings.HasPrefix(raw, "配置保存失败：") {
		return raw, true
	}
	if strings.Contains(raw, "输入输出错误") {
		return "串口设备无法打开或初始化", true
	}
	// 后端和历史数据仍保留内部“设备模板”命名，统一在用户可见边界转换术语。
	if strings.Contains(raw, "所属主控未配置设备模板") {
		return strings.ReplaceAll(raw, "所属主控未配置设备模板", "所属主站未配置设备类型"), true
	}
	if strings.Contains(raw, "设备模板") {
		return strings.ReplaceAll(raw, "设备模板", "设备类型"), true
	}
	lower := strings.ToLower(raw)
	switch {
	case strings.Contains(lower, "device not found:"):
		return "设备不存在：" + strings.TrimSpace(raw[strings.LastIndex(raw, ":")+1:]), true
	case strings.Contains(raw, "设备由主控配置自动生成"):
		return "设备由主站配置的设备数量、起始地址和设备类型读取区块自动生成，请在主站配置中调整。", true
	case strings.Contains(raw, "当前采集链路仅接受 Modbus RTU 或 Modbus TCP 主控"):
		return "当前采集链路仅接受 Modbus RTU 或 Modbus TCP 主站", true
	case strings.Contains(raw, "当前采集链路仅接受 Modbus RTU 主控"):
		return "当前采集链路仅接受 Modbus RTU 主站", true
	case strings.Contains(lower, "mbap transaction id"):
		return "MBAP 事务标识不匹配", true
	case strings.Contains(lower, "mbap protocol id"):
		return "MBAP 协议标识异常", true
	case strings.Contains(lower, "mbap length"):
		return "MBAP 长度字段异常", true
	case strings.Contains(raw, "主控寄存器数量必须为"):
		return "当前主站配置包含已停用的连续采集数量规则，请重新打开主站配置并直接填写设备数量。", true
	case strings.Contains(raw, "主控寄存器数量必须大于 0"):
		return "当前主站配置缺少有效设备数量，请重新打开主站配置并填写大于 0 的设备数量。", true
	case strings.Contains(raw, "单次读取寄存器数量超出范围"), strings.Contains(raw, "单次读取保持寄存器数量超出范围"):
		return "单次读取寄存器数量不能超过 125。", true
	case strings.Contains(raw, "该设备寄存器范围与同主控下已有设备重叠"):
		return "该设备寄存器范围与同主站下已有设备重叠，请调整块内偏移。", true
	case strings.Contains(lower, "channel is still referenced by master:"):
		return "仍有主站绑定该通道，无法删除：" + strings.TrimSpace(raw[strings.LastIndex(raw, ":")+1:]), true
	case strings.Contains(lower, "master created:"):
		return "主站已创建：" + strings.TrimSpace(raw[strings.LastIndex(raw, ":")+1:]), true
	case strings.Contains(lower, "master saved:"):
		return "主站已保存：" + strings.TrimSpace(raw[strings.LastIndex(raw, ":")+1:]), true
	case strings.Contains(lower, "master deleted:"):
		return "主站已删除：" + strings.TrimSpace(raw[strings.LastIndex(raw, ":")+1:]), true
	case strings.Contains(lower, "channel is not ready"):
		return "通道尚未就绪", true
	case strings.Contains(lower, "failed to open target channel"):
		return "打开目标通道失败", true
	case strings.Contains(lower, "protocol is invalid"):
		return "协议类型无效", true
	case strings.Contains(lower, "failed to read serial attributes"):
		return "读取串口属性失败：" + translateSystemErrorDetail(lower), true
	case strings.Contains(lower, "failed to configure serial port"):
		return "配置串口参数失败：" + translateSystemErrorDetail(lower), true
	case strings.Contains(lower, "failed to write to serial port"):
		return "写入串口失败：" + translateSystemErrorDetail(lower), true
	case strings.Contains(lower, "failed to read from serial port"):
		return "读取串口数据失败：" + translateSystemErrorDetail(lower), true
	case strings.Contains(lower, "failed to open serial port"), strings.Contains(lower, "open serial port failed"):
		return "打开串口失败：" + translateSystemErrorDetail(lower), true
	case strings.Contains(lower, "serial port not found"):
		return "未找到串口设备", true
	case strings.Contains(lower, "invalid baud rate"):
		return "波特率无效", true
	case strings.Contains(lower, "failed to fetch"):
		return "请求失败", true
	case strings.Contains(lower, "network error"):
		return "网络请求失败", true
	case strings.Contains(lower, "internal server error"):
		return "服务器内部错误", true
	case strings.Contains(lower, "bad request"):
		return "请求参数无效", true
	case strings.Contains(lower, "unauthorized"):
		return "未授权访问", true
	case strings.Contains(lower, "forbidden"):
		return "无权执行当前操作", true
	case strings.Contains(lower, "not found"):
		return "目标不存在", true
	case strings.Contains(lower, "conflict"):
		return "资源状态冲突", true
	case strings.Contains(lower, "context deadline exceeded"), strings.Contains(lower, "timed out"), strings.Contains(lower, "timeout"):
		return "操作超时", true
	case strings.Contains(lower, "cancelled"), strings.Contains(lower, "canceled"):
		return "操作已取消", true
	case strings.Contains(lower, "unavailable"):
		return "服务暂不可用", true
	case strings.Contains(lower, "connection refused"):
		return "连接被拒绝", true
	case strings.Contains(lower, "connection reset by peer"):
		return "连接已重置", true
	case strings.Contains(lower, "broken pipe"):
		return "连接已中断", true
	case strings.Contains(lower, "unexpected eof"):
		return "连接意外中断", true
	case lower == "eof" || strings.Contains(lower, " eof"):
		return "连接已断开", true
	case strings.Contains(lower, "no route to host"):
		return "无法到达目标主机", true
	case strings.Contains(lower, "host is down"):
		return "目标主机不可用", true
	case strings.Contains(lower, "permission denied"):
		return "访问被拒绝", true
	case strings.Contains(lower, "read-only file system"):
		return "文件系统为只读", true
	case strings.Contains(lower, "file exists"):
		return "目标已存在", true
	case strings.Contains(lower, "device or resource busy"), strings.Contains(lower, "device busy"), strings.Contains(lower, "port busy"):
		return "设备被占用", true
	case strings.Contains(lower, "input/output error"):
		return "串口设备无法打开或初始化", true
	case strings.Contains(lower, "no such file or directory"):
		return "路径不存在", true
	case strings.Contains(lower, "invalid argument"), strings.Contains(lower, "invalid parameter"):
		return "请求参数无效", true
	case strings.Contains(lower, "invalid character"):
		return "JSON 内容格式错误", true
	case strings.Contains(lower, "unexpected end of json input"):
		return "JSON 内容不完整", true
	case strings.Contains(lower, "malformed json"):
		return "JSON 格式错误", true
	case strings.Contains(lower, "decode failed"):
		return "数据解析失败", true
	case strings.Contains(lower, "encode failed"):
		return "数据编码失败", true
	case strings.Contains(lower, "parse failed"):
		return "解析失败", true
	case strings.Contains(lower, "create failed"):
		return "创建失败", true
	case strings.Contains(lower, "update failed"):
		return "更新失败", true
	case strings.Contains(lower, "delete failed"):
		return "删除失败", true
	case strings.Contains(lower, "save failed"):
		return "保存失败", true
	case strings.Contains(lower, "load failed"):
		return "加载失败", true
	case strings.Contains(lower, "login failed"):
		return "登录失败", true
	case strings.Contains(lower, "start polling failed"), strings.Contains(lower, "stop polling failed"):
		return "轮询服务操作失败", true
	case strings.Contains(lower, "unknown error"), lower == "error", lower == "failed":
		return fallbackForEnglishDetail(lower, "操作失败"), true
	}
	if containsChinese(raw) {
		return raw, true
	}

	return "", false
}

// containsChinese 判断文本是否包含中文字符。
func containsChinese(text string) bool {
	for _, r := range text {
		if r >= 0x4e00 && r <= 0x9fff {
			return true
		}
	}
	return false
}

// containsASCIIWord 判断是否包含ASCIIWord。
func containsASCIIWord(text string) bool {
	for _, r := range text {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') {
			return true
		}
	}
	return false
}
