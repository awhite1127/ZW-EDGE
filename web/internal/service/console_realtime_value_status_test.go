package service

import (
	"testing"

	"edge-web/internal/model"
)

func TestBuildRealtimePointRowsPreservesInvalidValueStatusText(t *testing.T) {
	snapshot := model.DeviceRealtimeSnapshot{
		TemplateID: "RD100",
		Points: []model.PointValue{
			{
				Key:         "resistance",
				Name:        "接地电阻",
				Quality:     "bad",
				Valid:       false,
				RawValue:    65535,
				DisplayText: "设备故障",
				Message:     "设备故障（原始值 65535）",
			},
		},
	}
	fields := []model.DeviceTemplateField{
		{Key: "resistance", ShowInRealtime: true},
	}

	rows := buildRealtimePointRows(snapshot, fields, false)
	if len(rows) != 1 {
		t.Fatalf("expected one realtime row, got %d", len(rows))
	}
	if rows[0].Text != "设备故障" || rows[0].ValueText != "设备故障" {
		t.Fatalf("invalid value status text was lost: %#v", rows[0])
	}
	if rows[0].Valid || rows[0].StateClass != "status-warn" {
		t.Fatalf("fault point quality state is incorrect: %#v", rows[0])
	}
}
