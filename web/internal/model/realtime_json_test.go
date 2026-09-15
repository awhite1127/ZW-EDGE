package model

import (
	"encoding/json"
	"testing"
)

func TestRealtimeJSONContainsDisplayDataWithoutTemplateDefinitions(t *testing.T) {
	point := RealtimePointRow{Key: "r", Name: "绝缘电阻", Text: "125 kΩ", Valid: true}
	row := RealtimeRow{DeviceID: "device", SummaryPoints: []RealtimePointRow{point}, RealtimeGroups: []RealtimePointGroup{{ID: "group", Points: []RealtimePointRow{point}}}}
	payload, err := json.Marshal(row)
	if err != nil {
		t.Fatal(err)
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(payload, &fields); err != nil {
		t.Fatal(err)
	}
	if _, exists := fields["template_fields"]; exists {
		t.Fatal("realtime payload includes unused template definitions")
	}
	for _, key := range []string{"summary_points", "realtime_groups"} {
		if len(fields[key]) == 0 || string(fields[key]) == "null" {
			t.Fatalf("missing display data: %s", key)
		}
	}
}
