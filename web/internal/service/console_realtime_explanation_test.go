package service

import (
	"edge-web/internal/model"
	"testing"
)

func TestRealtimeExplanation(t *testing.T) {
	cases := []struct {
		name string
		row  model.RealtimeRow
		want string
	}{
		{"timeout", model.RealtimeRow{ErrorMessage: "读取超时：从站 1 未响应"}, "设备未响应，请检查连接"},
		{"alarm precedence", model.RealtimeRow{ErrorMessage: "数据告警：超时计数超限", Diagnosis: model.DiagnosisStatus{ErrorCode: "data_alarm"}}, "数据超限，请检查设备"},
		{"unknown retains generic advice", model.RealtimeRow{ErrorMessage: "unexpected register response"}, "采集异常，请检查设备"},
		{"not collected", model.RealtimeRow{}, "尚未采集，等待数据"},
		{"offline", model.RealtimeRow{HasStatus: true}, "设备离线，请检查连接"},
		{"online", model.RealtimeRow{HasStatus: true, Online: true}, "—"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := RealtimeExplanation(tc.row); got != tc.want {
				t.Fatalf("got %q want %q", got, tc.want)
			}
		})
	}
}
