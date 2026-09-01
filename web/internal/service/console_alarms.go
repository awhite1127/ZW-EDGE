package service

// 本文件把后端按“设备 + 数据项”存储的告警规则转换为页面按“主站 + 数据项”维护的视图，
// 并在提交时重新展开为设备级规则。

import (
	"context"
	"edge-web/internal/model"
	"errors"
	"fmt"
	"sort"
	"strings"
)

// buildAlarmRuleConfigItems 构建告警规则配置项目。
func buildAlarmRuleConfigItems(
	devices []model.DeviceConfig,
	masters []model.MasterNodeConfig,
	templates []model.DeviceTemplateDefinition,
	rules []model.AlarmRule,
) ([]model.AlarmRuleConfigItem, []model.AlarmRuleDeviceOption) {
	devicesByMaster := groupDevicesByMaster(devices)
	ruleByKey := make(map[string]model.AlarmRule, len(rules))
	for _, rule := range rules {
		ruleByKey[alarmRuleKey(rule.DeviceID, rule.PointKey)] = normalizeAlarmRuleForView(rule)
	}

	items := make([]model.AlarmRuleConfigItem, 0)
	masterOptions := make([]model.AlarmRuleDeviceOption, 0, len(masters))
	for _, master := range masters {
		masterID := strings.TrimSpace(master.MasterID)
		if masterID == "" {
			continue
		}
		templateID := strings.TrimSpace(master.DeviceTemplate)
		template, found := model.FindDeviceTemplateIn(templates, templateID)
		if !found {
			continue
		}
		option := model.AlarmRuleDeviceOption{
			MasterID:   masterID,
			MasterName: displayNameOrID(master.MasterName, masterID),
			Points:     make([]model.AlarmRulePointOption, 0, len(template.Fields)),
		}
		masterDevices := devicesByMaster[masterID]
		deviceNames := make([]string, 0, len(masterDevices))
		for _, device := range masterDevices {
			deviceNames = append(deviceNames, displayNameOrID(device.DeviceName, device.DeviceID))
		}
		fields := append([]model.DeviceTemplateField(nil), template.Fields...)
		sort.SliceStable(fields, func(i, j int) bool {
			if fields[i].DisplayOrder != fields[j].DisplayOrder {
				return fields[i].DisplayOrder < fields[j].DisplayOrder
			}
			return fields[i].Key < fields[j].Key
		})
		for _, field := range fields {
			if strings.TrimSpace(field.Key) == "" || !field.Summary {
				continue
			}
			pointName := displayNameOrID(field.DisplayName, field.Key)
			// 同一主站下可能有多台设备。页面只显示一行，因此用代表规则展示共同配置；
			// 如果各设备规则不一致，则显式标记为“规则不一致”，避免误导用户以为可无损编辑。
			representative, existingCount, enabledCount, inconsistent := aggregateMasterPointRule(masterDevices, field.Key, ruleByKey)
			hasRule := existingCount > 0
			if !hasRule {
				representative = defaultAlarmRule("", field.Key)
			}
			statusText := alarmRuleAggregateStatus(len(masterDevices), existingCount, enabledCount, inconsistent)
			highText := alarmDirectionThresholdText(representative.HighEnabled, representative.HighThreshold, field.Precision, field.Unit)
			lowText := alarmDirectionThresholdText(representative.LowEnabled, representative.LowThreshold, field.Precision, field.Unit)
			levelText := alarmLevelText(representative.Level)
			limitTypeText := alarmLimitTypeText(representative)
			triggerText := fmt.Sprintf("%d 次", normalizedAlarmCount(representative.TriggerCount))
			recoveryText := fmt.Sprintf("%d 次", normalizedAlarmCount(representative.RecoveryCount))
			if inconsistent {
				limitTypeText = "规则不一致"
				highText = "规则不一致"
				lowText = "规则不一致"
				levelText = "规则不一致"
				triggerText = "规则不一致"
				recoveryText = "规则不一致"
			}
			item := model.AlarmRuleConfigItem{
				MasterID:           masterID,
				MasterName:         option.MasterName,
				TemplateID:         template.ID,
				TemplateName:       displayNameOrID(template.DisplayName, template.ID),
				PointKey:           field.Key,
				PointName:          pointName,
				Unit:               field.Unit,
				Precision:          field.Precision,
				CoveredDeviceCount: len(masterDevices),
				CoveredDeviceNames: strings.Join(deviceNames, "、"),
				ExistingRuleCount:  existingCount,
				HasRule:            hasRule,
				Rule:               representative,
				StatusText:         statusText,
				LimitTypeText:      limitTypeText,
				CoverageText:       alarmCoverageText(len(masterDevices)),
				LevelText:          levelText,
				HighText:           highText,
				LowText:            lowText,
				TriggerText:        triggerText,
				RecoveryText:       recoveryText,
			}
			items = append(items, item)
			option.Points = append(option.Points, model.AlarmRulePointOption{
				MasterID:  masterID,
				PointKey:  field.Key,
				PointName: pointName,
				Unit:      field.Unit,
				Precision: field.Precision,
			})
		}
		if len(option.Points) > 0 {
			masterOptions = append(masterOptions, option)
		}
	}
	sort.SliceStable(items, func(i, j int) bool {
		if items[i].MasterName != items[j].MasterName {
			return items[i].MasterName < items[j].MasterName
		}
		return items[i].PointName < items[j].PointName
	})
	return items, masterOptions
}

// groupDevicesByMaster 按主站标识对设备分组。
func groupDevicesByMaster(devices []model.DeviceConfig) map[string][]model.DeviceConfig {
	result := make(map[string][]model.DeviceConfig)
	for _, device := range devices {
		masterID := strings.TrimSpace(device.MasterID)
		if masterID == "" {
			continue
		}
		result[masterID] = append(result[masterID], device)
	}
	for masterID := range result {
		sort.SliceStable(result[masterID], func(i, j int) bool {
			left := displayNameOrID(result[masterID][i].DeviceName, result[masterID][i].DeviceID)
			right := displayNameOrID(result[masterID][j].DeviceName, result[masterID][j].DeviceID)
			if left != right {
				return left < right
			}
			return result[masterID][i].DeviceID < result[masterID][j].DeviceID
		})
	}
	return result
}

// aggregateMasterPointRule 聚合主站点位规则。
func aggregateMasterPointRule(
	devices []model.DeviceConfig,
	pointKey string,
	ruleByKey map[string]model.AlarmRule,
) (model.AlarmRule, int, int, bool) {
	// 返回值同时表达“代表规则”“已有规则数量”“启用数量”和“一致性”，供列表页生成批量配置状态。
	var representative model.AlarmRule
	existingCount := 0
	enabledCount := 0
	inconsistent := false
	for _, device := range devices {
		rule, ok := ruleByKey[alarmRuleKey(device.DeviceID, pointKey)]
		if !ok {
			continue
		}
		existingCount++
		if rule.Enabled {
			enabledCount++
		}
		if existingCount == 1 {
			representative = rule
			continue
		}
		if !alarmRulesEquivalent(representative, rule) {
			inconsistent = true
		}
	}
	if existingCount == 0 {
		representative = defaultAlarmRule("", pointKey)
	}
	return representative, existingCount, enabledCount, inconsistent
}

// alarmRulesEquivalent 判断两条告警规则的业务配置是否等价。
func alarmRulesEquivalent(left model.AlarmRule, right model.AlarmRule) bool {
	return left.HighEnabled == right.HighEnabled &&
		left.HighThreshold == right.HighThreshold &&
		left.LowEnabled == right.LowEnabled &&
		left.LowThreshold == right.LowThreshold &&
		normalizeAlarmLevel(left.Level) == normalizeAlarmLevel(right.Level) &&
		left.Hysteresis == right.Hysteresis &&
		normalizedAlarmCount(left.TriggerCount) == normalizedAlarmCount(right.TriggerCount) &&
		normalizedAlarmCount(left.RecoveryCount) == normalizedAlarmCount(right.RecoveryCount)
}

// alarmRuleAggregateStatus 汇总并返回当前状态。
func alarmRuleAggregateStatus(deviceCount int, existingCount int, enabledCount int, inconsistent bool) string {
	if inconsistent {
		return "规则不一致"
	}
	if existingCount == 0 {
		return "未配置"
	}
	if enabledCount == 0 {
		return "已停用"
	}
	if existingCount == deviceCount && enabledCount == deviceCount {
		return "已启用"
	}
	return "部分启用"
}

// alarmCoverageText 返回报警规则覆盖的数据项数量说明。
func alarmCoverageText(count int) string {
	if count == 0 {
		return "暂无可作用设备"
	}
	return fmt.Sprintf("作用于 %d 台设备", count)
}

// alarmLimitTypeText 汇总报警规则启用的上下限类型。
func alarmLimitTypeText(rule model.AlarmRule) string {
	if !rule.Enabled {
		return "不启用"
	}
	if rule.HighEnabled && rule.LowEnabled {
		return "上下限"
	}
	if rule.HighEnabled {
		return "仅上限"
	}
	if rule.LowEnabled {
		return "仅下限"
	}
	return "不启用"
}

// alarmRuleKey 生成稳定的对象标识。
func alarmRuleKey(deviceID string, pointKey string) string {
	return deviceID + "\x00" + pointKey
}

// normalizeAlarmRuleForView 规范化告警规则用于视图。
func normalizeAlarmRuleForView(rule model.AlarmRule) model.AlarmRule {
	if rule.Level == "" {
		rule.Level = "warning"
	}
	if rule.TriggerCount == 0 {
		rule.TriggerCount = 1
	}
	if rule.RecoveryCount == 0 {
		rule.RecoveryCount = 1
	}
	return rule
}

// defaultAlarmRule 生成默认告警规则。
func defaultAlarmRule(deviceID string, pointKey string) model.AlarmRule {
	return model.AlarmRule{
		DeviceID:      deviceID,
		PointKey:      pointKey,
		Level:         "warning",
		TriggerCount:  1,
		RecoveryCount: 1,
	}
}

// alarmLevelText 返回报警级别的中文名称。
func alarmLevelText(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "严重"
	default:
		return "告警"
	}
}

// alarmDirectionText 返回报警越限方向的中文名称。
func alarmDirectionText(direction string) string {
	switch strings.ToLower(strings.TrimSpace(direction)) {
	case "high":
		return "上限"
	case "low":
		return "下限"
	default:
		return "-"
	}
}

// alarmDirectionThresholdText 格式化单侧报警阈值及单位。
func alarmDirectionThresholdText(enabled bool, threshold float64, precision uint32, unit string) string {
	if !enabled {
		return "未启用"
	}
	return formatHistoryValue(threshold, precision, unit)
}

// normalizedAlarmCount 将告警连续次数约束为有效值。
func normalizedAlarmCount(value uint32) uint32 {
	if value == 0 {
		return 1
	}
	return value
}

// SaveMasterAlarmRule 保存主站点位告警规则并同步到所属设备。
func (s *ConsoleService) SaveMasterAlarmRule(ctx context.Context, request model.MasterAlarmRuleRequest) model.ActionFeedback {
	request.MasterID = strings.TrimSpace(request.MasterID)
	request.PointKey = strings.TrimSpace(request.PointKey)
	// Web 只规范化表单文本；告警级别和阈值关系由后端领域 validator 决定。
	request.Level = strings.ToLower(strings.TrimSpace(request.Level))
	if message := validateMasterAlarmRuleRequest(request); message != "" {
		return model.ActionFeedback{Success: false, Message: message}
	}
	targets, err := s.masterAlarmTargets(ctx, request.MasterID)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	if len(targets) == 0 {
		return model.ActionFeedback{Success: false, Message: "该主站下没有可应用的设备"}
	}
	// 前端以主站维度保存，后端执行层以设备维度判定；这里把一次主站配置展开为多条设备规则。
	successCount := 0
	failures := make([]string, 0)
	for _, device := range targets {
		rule := model.AlarmRule{
			DeviceID:      device.DeviceID,
			PointKey:      request.PointKey,
			Enabled:       request.Enabled,
			HighEnabled:   request.HighEnabled,
			HighThreshold: request.HighThreshold,
			LowEnabled:    request.LowEnabled,
			LowThreshold:  request.LowThreshold,
			Level:         request.Level,
			Hysteresis:    request.Hysteresis,
			TriggerCount:  request.TriggerCount,
			RecoveryCount: request.RecoveryCount,
		}
		if _, err := s.backend.UpsertAlarmRule(ctx, rule); err != nil {
			failures = append(failures, displayNameOrID(device.DeviceName, device.DeviceID)+": "+err.Error())
			continue
		}
		successCount++
	}
	if len(failures) > 0 {
		return model.ActionFeedback{
			Success: false,
			Message: fmt.Sprintf("告警规则部分应用失败：成功 %d 个，失败 %d 个。%s", successCount, len(failures), strings.Join(failures, "；")),
		}
	}
	return model.ActionFeedback{Success: true, Message: fmt.Sprintf("告警规则已应用到 %d 个设备。", successCount)}
}

// DisableMasterAlarmRule 停用主站点位告警规则。
func (s *ConsoleService) DisableMasterAlarmRule(ctx context.Context, masterID string, pointKey string) model.ActionFeedback {
	masterID = strings.TrimSpace(masterID)
	pointKey = strings.TrimSpace(pointKey)
	if masterID == "" || pointKey == "" {
		return model.ActionFeedback{Success: false, Message: "主站和数据项不能为空"}
	}
	rules, err := s.masterExistingRules(ctx, masterID, pointKey)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	if len(rules) == 0 {
		return model.ActionFeedback{Success: false, Message: "暂无可停用规则"}
	}
	// 停用保留规则阈值，便于之后恢复；删除才真正移除后端规则记录。
	successCount := 0
	failures := make([]string, 0)
	for _, rule := range rules {
		rule.Enabled = false
		if _, err := s.backend.UpsertAlarmRule(ctx, rule); err != nil {
			failures = append(failures, rule.DeviceID+": "+err.Error())
			continue
		}
		successCount++
	}
	if len(failures) > 0 {
		return model.ActionFeedback{
			Success: false,
			Message: fmt.Sprintf("告警规则部分停用失败：成功 %d 个，失败 %d 个。%s", successCount, len(failures), strings.Join(failures, "；")),
		}
	}
	return model.ActionFeedback{Success: true, Message: fmt.Sprintf("已停用该主站下 %d 个设备的告警规则。", successCount)}
}

// DeleteMasterAlarmRule 删除主站告警规则。
func (s *ConsoleService) DeleteMasterAlarmRule(ctx context.Context, masterID string, pointKey string) model.ActionFeedback {
	masterID = strings.TrimSpace(masterID)
	pointKey = strings.TrimSpace(pointKey)
	if masterID == "" || pointKey == "" {
		return model.ActionFeedback{Success: false, Message: "主站和数据项不能为空"}
	}
	rules, err := s.masterExistingRules(ctx, masterID, pointKey)
	if err != nil {
		return model.ActionFeedback{Success: false, Message: err.Error()}
	}
	if len(rules) == 0 {
		return model.ActionFeedback{Success: false, Message: "暂无可删除规则"}
	}
	successCount := 0
	failures := make([]string, 0)
	for _, rule := range rules {
		if _, err := s.backend.DeleteAlarmRule(ctx, rule.DeviceID, pointKey); err != nil {
			failures = append(failures, rule.DeviceID+": "+err.Error())
			continue
		}
		successCount++
	}
	if len(failures) > 0 {
		return model.ActionFeedback{
			Success: false,
			Message: fmt.Sprintf("告警规则部分删除失败：成功 %d 个，失败 %d 个。%s", successCount, len(failures), strings.Join(failures, "；")),
		}
	}
	return model.ActionFeedback{Success: true, Message: fmt.Sprintf("已删除该主站下 %d 个设备的告警规则。", successCount)}
}

// masterAlarmTargets 收集主站下需要应用告警规则的设备点位。
func (s *ConsoleService) masterAlarmTargets(ctx context.Context, masterID string) ([]model.DeviceConfig, error) {
	masters, err := s.backend.ListMasters(ctx)
	if err != nil {
		return nil, err
	}
	masterExists := false
	for _, master := range masters {
		if strings.TrimSpace(master.MasterID) == masterID {
			masterExists = true
			break
		}
	}
	if !masterExists {
		return nil, errors.New("主站不存在")
	}
	devices, err := s.backend.ListDevices(ctx)
	if err != nil {
		return nil, err
	}
	targets := make([]model.DeviceConfig, 0)
	for _, device := range devices {
		if strings.TrimSpace(device.MasterID) == masterID {
			targets = append(targets, device)
		}
	}
	sort.SliceStable(targets, func(i, j int) bool {
		left := displayNameOrID(targets[i].DeviceName, targets[i].DeviceID)
		right := displayNameOrID(targets[j].DeviceName, targets[j].DeviceID)
		if left != right {
			return left < right
		}
		return targets[i].DeviceID < targets[j].DeviceID
	})
	return targets, nil
}

// masterExistingRules 构建页面或接口使用的结果列表。
func (s *ConsoleService) masterExistingRules(ctx context.Context, masterID string, pointKey string) ([]model.AlarmRule, error) {
	targets, err := s.masterAlarmTargets(ctx, masterID)
	if err != nil {
		return nil, err
	}
	targetDeviceIDs := make(map[string]struct{}, len(targets))
	for _, device := range targets {
		targetDeviceIDs[device.DeviceID] = struct{}{}
	}
	rules, err := s.backend.ListAlarmRules(ctx, "")
	if err != nil {
		return nil, err
	}
	result := make([]model.AlarmRule, 0)
	for _, rule := range rules {
		if rule.PointKey != pointKey {
			continue
		}
		if _, ok := targetDeviceIDs[rule.DeviceID]; ok {
			result = append(result, normalizeAlarmRuleForView(rule))
		}
	}
	sort.SliceStable(result, func(i, j int) bool {
		return result[i].DeviceID < result[j].DeviceID
	})
	return result, nil
}

// validateMasterAlarmRuleRequest 只校验 Web 交互所需的主站和数据项选择。
// 告警级别、阈值关系、回差和连续次数属于领域规则，不在 Web 层复制。
func validateMasterAlarmRuleRequest(request model.MasterAlarmRuleRequest) string {
	if strings.TrimSpace(request.MasterID) == "" {
		return "请选择主站"
	}
	if strings.TrimSpace(request.PointKey) == "" {
		return "请选择数据项"
	}
	return ""
}

// normalizeAlarmLevel 规范化告警Level。
func normalizeAlarmLevel(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error":
		return "error"
	default:
		return "warning"
	}
}
