package model

// 本文件维护 Web 与 controller 之间稳定的设备类型 JSON 模型。
// 内置设备类型定义由 controller 单一维护，Web 不复制采集字段或写命令语义。

import "encoding/json"

// EM100InsulationMonitorDeviceTemplateID 参与 Web 端专用状态展示，不包含设备类型定义副本。
const EM100InsulationMonitorDeviceTemplateID = "EM100"

type DeviceTemplateEnumItem struct {
	Value     int64  `json:"value"`
	Label     string `json:"label"`
	SortOrder int    `json:"sort_order"`
}

type DeviceTemplateField struct {
	Key                  string                   `json:"key"`
	DisplayName          string                   `json:"display_name"`
	ParserID             string                   `json:"parser_id"`
	Unit                 string                   `json:"unit"`
	DataType             string                   `json:"data_type"`
	DataTypeText         string                   `json:"data_type_text,omitempty"`
	ReadBlockKey         string                   `json:"read_block_key"`
	RegisterOffset       uint16                   `json:"register_offset"`
	RegisterAddressText  string                   `json:"register_address_text,omitempty"`
	RegisterCount        uint16                   `json:"register_count"`
	ByteOrder            string                   `json:"byte_order"`
	WordOrder            string                   `json:"word_order"`
	BitIndex             int                      `json:"bit_index"`
	EnumItems            []DeviceTemplateEnumItem `json:"enum_items"`
	EnumSummaryText      string                   `json:"enum_summary_text,omitempty"`
	Scale                float64                  `json:"scale"`
	Offset               float64                  `json:"offset"`
	Precision            uint32                   `json:"precision"`
	Summary              bool                     `json:"summary"`
	HistoryEnabled       bool                     `json:"history_enabled"`
	ShowInRealtime       bool                     `json:"show_in_realtime"`
	SummaryText          string                   `json:"summary_text,omitempty"`
	AlarmCapabilityText  string                   `json:"alarm_capability_text,omitempty"`
	AlarmCapabilityClass string                   `json:"alarm_capability_class,omitempty"`
	WritableText         string                   `json:"writable_text,omitempty"`
	WritableClass        string                   `json:"writable_class,omitempty"`
	DisplayOrder         int                      `json:"display_order"`
	InvalidRuleType      string                   `json:"invalid_rule_type"`
	InvalidRuleValue     float64                  `json:"invalid_rule_value"`
	InvalidRuleMin       float64                  `json:"invalid_rule_min"`
	InvalidRuleMax       float64                  `json:"invalid_rule_max"`
	RealtimeGroupID      string                   `json:"realtime_group_id"`
}

// MarshalJSON 同时保留页面使用的 offset 和配置包规范字段 value_offset；二者始终来自同一模型值。
func (field DeviceTemplateField) MarshalJSON() ([]byte, error) {
	type fieldAlias DeviceTemplateField
	enumItems := field.EnumItems
	if enumItems == nil {
		enumItems = make([]DeviceTemplateEnumItem, 0)
	}
	return json.Marshal(struct {
		fieldAlias
		ValueOffset float64                  `json:"value_offset"`
		EnumItems   []DeviceTemplateEnumItem `json:"enum_items"`
	}{
		fieldAlias:  fieldAlias(field),
		ValueOffset: field.Offset,
		EnumItems:   enumItems,
	})
}

// DeviceTemplateReadBlock 是设备类型内部稳定引用的连续读取区块。
// StartOffset 相对于单台设备基地址，字段偏移再相对于所属区块。
type DeviceTemplateReadBlock struct {
	BlockKey      string `json:"block_key"`
	DisplayName   string `json:"display_name"`
	FunctionCode  int    `json:"function_code"`
	StartOffset   int    `json:"start_offset"`
	RegisterCount int    `json:"register_count"`
	SortOrder     int    `json:"sort_order"`
}

type DeviceTemplateRealtimeGroup struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Order int    `json:"order"`
}

type DeviceTemplateWriteCommandOption struct {
	Label string `json:"label"`
	Value uint16 `json:"value"`
}

type DeviceTemplateWriteCommandField struct {
	Key             string                             `json:"key"`
	Label           string                             `json:"label"`
	Type            string                             `json:"type"`
	Min             uint16                             `json:"min"`
	Max             uint16                             `json:"max"`
	Unit            string                             `json:"unit"`
	HasDefaultValue bool                               `json:"has_default_value"`
	DefaultValue    uint16                             `json:"default_value"`
	Options         []DeviceTemplateWriteCommandOption `json:"options"`
}

type DeviceTemplateWriteCommand struct {
	Key            string                            `json:"key"`
	Name           string                            `json:"name"`
	Description    string                            `json:"description"`
	Group          string                            `json:"group"`
	FunctionCode   uint32                            `json:"function_code"`
	RegisterOffset uint16                            `json:"register_offset"`
	RegisterCount  uint16                            `json:"register_count"`
	HasAbsolute    bool                              `json:"has_absolute_register"`
	Absolute       uint16                            `json:"absolute_register"`
	FixedValues    []uint16                          `json:"fixed_values"`
	ValueFields    []DeviceTemplateWriteCommandField `json:"value_fields"`
	Warnings       []string                          `json:"warnings"`
	RequireConfirm bool                              `json:"require_confirm"`
	ConfirmText    string                            `json:"confirm_text"`
	SuccessHint    string                            `json:"success_hint"`
}

type DeviceTemplateDefinition struct {
	ID                      string                        `json:"id"`
	DisplayName             string                        `json:"display_name"`
	Description             string                        `json:"description"`
	DefaultStart            uint16                        `json:"default_start_register"`
	DeviceAddressStride     int                           `json:"device_address_stride"`
	ReadBlocks              []DeviceTemplateReadBlock     `json:"read_blocks"`
	Builtin                 bool                          `json:"builtin"`
	Fields                  []DeviceTemplateField         `json:"fields"`
	WriteCommands           []DeviceTemplateWriteCommand  `json:"write_commands"`
	RealtimeGroupingEnabled bool                          `json:"realtime_grouping_enabled"`
	RealtimeGroups          []DeviceTemplateRealtimeGroup `json:"realtime_groups"`
}

func FindDeviceTemplateIn(templates []DeviceTemplateDefinition, templateID string) (DeviceTemplateDefinition, bool) {
	if templateID == "" {
		return DeviceTemplateDefinition{}, false
	}
	for _, definition := range templates {
		if definition.ID == templateID {
			return definition, true
		}
	}
	return DeviceTemplateDefinition{}, false
}
