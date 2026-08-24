package service

import (
	"context"
	"sync"
	"testing"
	"time"

	"edge-web/internal/model"
)

type displayNameBackendSpy struct {
	backendAPI

	mu           sync.Mutex
	calls        map[string]int
	settings     model.SystemSettings
	updateResult model.SystemSettingsUpdateResult
	importResult model.ConfigImportResult
	resetResult  model.FactoryResetResult
	getStarted   chan struct{}
	getRelease   chan struct{}
	startOnce    sync.Once
}

func newDisplayNameBackendSpy() *displayNameBackendSpy {
	return &displayNameBackendSpy{calls: make(map[string]int)}
}

func (s *displayNameBackendSpy) GetSystemSettings(context.Context) (model.SystemSettings, error) {
	s.mu.Lock()
	s.calls["get"]++
	settings := s.settings
	started := s.getStarted
	release := s.getRelease
	s.mu.Unlock()
	if started != nil {
		s.startOnce.Do(func() { close(started) })
	}
	if release != nil {
		<-release
	}
	return settings, nil
}

func (s *displayNameBackendSpy) UpdateSystemSettings(context.Context, model.SystemSettingsUpdateRequest) (model.SystemSettingsUpdateResult, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.calls["update"]++
	return s.updateResult, nil
}

func (s *displayNameBackendSpy) ImportSystemConfig(context.Context, model.ConfigExportBundle) (model.ConfigImportResult, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.calls["import"]++
	return s.importResult, nil
}

func (s *displayNameBackendSpy) RequestFactoryReset(context.Context) (model.FactoryResetResult, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.calls["reset"]++
	return s.resetResult, nil
}

func (s *displayNameBackendSpy) setSettings(settings model.SystemSettings) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.settings = settings
}

func (s *displayNameBackendSpy) callCount(method string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.calls[method]
}

func TestSystemDisplayNameCacheUsesTTL(t *testing.T) {
	backend := newDisplayNameBackendSpy()
	backend.setSettings(model.SystemSettings{DisplayName: "Console A"})
	console := NewConsoleService(backend)

	for index := 0; index < 2; index++ {
		value, err := console.GetSystemDisplayName(context.Background())
		if err != nil || value != "Console A" {
			t.Fatalf("GetSystemDisplayName = %q, %v", value, err)
		}
	}
	if got := backend.callCount("get"); got != 1 {
		t.Fatalf("backend reads = %d, want 1", got)
	}

	console.systemDisplayNameMu.Lock()
	console.systemDisplayNameExpiresAt = time.Now().Add(-time.Second)
	console.systemDisplayNameMu.Unlock()
	backend.setSettings(model.SystemSettings{DisplayName: "Console B"})
	value, err := console.GetSystemDisplayName(context.Background())
	if err != nil || value != "Console B" {
		t.Fatalf("expired GetSystemDisplayName = %q, %v", value, err)
	}
	if got := backend.callCount("get"); got != 2 {
		t.Fatalf("backend reads after expiry = %d, want 2", got)
	}
}

func TestSystemDisplayNameUpdateWinsOverInflightRead(t *testing.T) {
	backend := newDisplayNameBackendSpy()
	backend.settings = model.SystemSettings{DisplayName: "Old Console"}
	backend.updateResult.Settings.DisplayName = "New Console"
	backend.getStarted = make(chan struct{})
	backend.getRelease = make(chan struct{})
	console := NewConsoleService(backend)

	result := make(chan string, 1)
	go func() {
		value, _ := console.GetSystemDisplayName(context.Background())
		result <- value
	}()
	<-backend.getStarted
	feedback := console.UpdateSystemSettings(context.Background(), model.SystemSettingsUpdateRequest{DisplayName: "New Console"})
	if !feedback.Success {
		t.Fatalf("update failed: %s", feedback.Message)
	}
	close(backend.getRelease)
	if value := <-result; value != "New Console" {
		t.Fatalf("inflight read returned %q", value)
	}
	value, err := console.GetSystemDisplayName(context.Background())
	if err != nil || value != "New Console" {
		t.Fatalf("cached value = %q, %v", value, err)
	}
	if got := backend.callCount("get"); got != 1 {
		t.Fatalf("backend reads = %d, want 1", got)
	}
}

func TestSystemDisplayNameCacheRefreshesAfterImportAndFactoryReset(t *testing.T) {
	backend := newDisplayNameBackendSpy()
	backend.setSettings(model.SystemSettings{DisplayName: "Old Console"})
	backend.importResult.Imported = true
	backend.resetResult.ResetCompleted = true
	console := NewConsoleService(backend)
	if _, err := console.GetSystemDisplayName(context.Background()); err != nil {
		t.Fatal(err)
	}

	feedback := console.ImportSystemConfig(context.Background(), model.ConfigExportBundle{
		SystemSettings: model.SystemSettings{DisplayName: "Imported Console"},
	})
	if !feedback.Success {
		t.Fatalf("import failed: %s", feedback.Message)
	}
	value, err := console.GetSystemDisplayName(context.Background())
	if err != nil || value != "Imported Console" {
		t.Fatalf("imported value = %q, %v", value, err)
	}

	feedback = console.RequestFactoryReset(context.Background())
	if !feedback.Success {
		t.Fatalf("factory reset failed: %s", feedback.Message)
	}
	value, err = console.GetSystemDisplayName(context.Background())
	if err != nil || value != "" {
		t.Fatalf("factory-reset value = %q, %v", value, err)
	}
	if got := backend.callCount("get"); got != 1 {
		t.Fatalf("backend reads = %d, want 1", got)
	}
}
