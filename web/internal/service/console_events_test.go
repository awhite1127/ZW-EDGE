package service

import (
	"context"
	"fmt"
	"testing"

	"edge-web/internal/model"
)

type eventHistoryBackendStub struct {
	backendAPI
	query          model.EventHistoryQuery
	queryResult    model.EventHistoryResult
	queryErr       error
	devices        []model.DeviceConfig
	devicesErr     error
	recentCallMade bool
}

func (stub *eventHistoryBackendStub) QueryServiceEvents(
	_ context.Context,
	query model.EventHistoryQuery,
) (model.EventHistoryResult, error) {
	stub.query = query
	return stub.queryResult, stub.queryErr
}

func (stub *eventHistoryBackendStub) ListDevices(context.Context) ([]model.DeviceConfig, error) {
	return stub.devices, stub.devicesErr
}

func (stub *eventHistoryBackendStub) ListRecentEvents(context.Context) ([]model.ServiceEvent, error) {
	stub.recentCallMade = true
	return nil, fmt.Errorf("history page must not use list_recent_events")
}

func TestLoadEventsQueriesBackendWithNormalizedFiltersAndUsesBackendTotals(t *testing.T) {
	stub := &eventHistoryBackendStub{
		queryResult: model.EventHistoryResult{
			Rows: []model.ServiceEvent{
				{
					EventID:  "evt-21",
					TargetID: "dev-1",
					Summary:  "system-dev-1 communication restored",
					Diagnosis: model.DiagnosisStatus{
						TargetID:   "dev-1",
						TargetName: "system-dev-1",
					},
				},
				{EventID: "evt-22", TargetID: "dev-2", Summary: "poll warning"},
			},
			Total:      22,
			LevelStats: model.EventLevelStats{Error: 3, Warning: 4, Info: 28},
			SourceStats: []model.EventSourceStat{
				{Source: "polling", Count: 12},
				{Source: "custom_source", Count: 1},
			},
		},
		devices: []model.DeviceConfig{
			{DeviceID: "dev-1", SystemName: "system-dev-1", DeviceName: "Main Meter"},
		},
	}

	page := NewConsoleService(stub).LoadEvents(context.Background(), model.EventsPageQuery{
		Tab:       "history",
		Level:     " WARN ",
		Source:    " POLLING ",
		Search:    " ground ",
		TimeRange: "24H",
		Page:      5,
		PageSize:  20,
	})

	wantQuery := model.EventHistoryQuery{
		Level: "warning", Source: "polling", Search: "ground", TimeRange: "24h", Page: 5, PageSize: 20,
	}
	if stub.query != wantQuery {
		t.Fatalf("backend query mismatch: got %+v, want %+v", stub.query, wantQuery)
	}
	if stub.recentCallMade {
		t.Fatal("history page called ListRecentEvents")
	}
	if !page.EventsState.Available || !page.BackendReachable {
		t.Fatalf("history section should be available: %+v", page.EventsState)
	}
	if page.FilteredEventCount != 22 || page.TotalPages != 2 || page.Page != 2 {
		t.Fatalf("pagination must use backend total: total=%d pages=%d page=%d", page.FilteredEventCount, page.TotalPages, page.Page)
	}
	if page.PageStart != 21 || page.PageEnd != 22 || !page.HasPrevPage || page.HasNextPage {
		t.Fatalf("page bounds mismatch: start=%d end=%d prev=%t next=%t", page.PageStart, page.PageEnd, page.HasPrevPage, page.HasNextPage)
	}
	if page.TotalEventCount != 35 || page.ErrorCount != 3 || page.WarningCount != 4 || page.InfoCount != 28 {
		t.Fatalf("level stats mismatch: total=%d error=%d warning=%d info=%d", page.TotalEventCount, page.ErrorCount, page.WarningCount, page.InfoCount)
	}
	if len(page.Events) != 2 || page.Events[0].Summary != "Main Meter communication restored" || page.Events[0].Diagnosis.TargetName != "Main Meter" {
		t.Fatalf("paged rows should retain device display-name hydration: %+v", page.Events)
	}
	if !hasEventSourceOption(page.SourceOptions, "custom_source") {
		t.Fatalf("dynamic source stats must feed source options: %+v", page.SourceOptions)
	}
}

func TestLoadEventsReportsQueryFailureWithoutFallingBackToRecentHundred(t *testing.T) {
	stub := &eventHistoryBackendStub{queryErr: fmt.Errorf("event query unavailable")}
	page := NewConsoleService(stub).LoadEvents(context.Background(), model.EventsPageQuery{Tab: "history"})

	if page.EventsState.Available || page.EventsState.ErrorMessage != "event query unavailable" {
		t.Fatalf("query failure not surfaced: %+v", page.EventsState)
	}
	if stub.recentCallMade {
		t.Fatal("history page must not silently fall back to the truncated recent-event endpoint")
	}
}

func hasEventSourceOption(options []model.EventFilterOption, value string) bool {
	for _, option := range options {
		if option.Value == value {
			return true
		}
	}
	return false
}
