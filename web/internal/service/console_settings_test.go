package service

import (
	"context"
	"sync"
	"testing"

	"edge-web/internal/model"
)

type settingsBackendSpy struct {
	backendAPI

	mu        sync.Mutex
	calls     map[string]int
	settings  model.SystemSettings
	templates model.DeviceTemplateManagementView
	mqtt      model.MqttSettings
	runtime   model.MqttRuntimeStatus
}

func newSettingsBackendSpy() *settingsBackendSpy {
	return &settingsBackendSpy{calls: make(map[string]int)}
}

func (s *settingsBackendSpy) record(method string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.calls[method]++
}

func (s *settingsBackendSpy) GetSystemSettings(context.Context) (model.SystemSettings, error) {
	s.record("system")
	return s.settings, nil
}

func (s *settingsBackendSpy) GetDeviceTemplateManagement(context.Context) (model.DeviceTemplateManagementView, error) {
	s.record("templates")
	return s.templates, nil
}

func (s *settingsBackendSpy) GetMqttSettings(context.Context) (model.MqttSettings, error) {
	s.record("mqtt")
	return s.mqtt, nil
}

func (s *settingsBackendSpy) GetMqttRuntimeStatus(context.Context) (model.MqttRuntimeStatus, error) {
	s.record("mqtt-runtime")
	return s.runtime, nil
}

func (s *settingsBackendSpy) assertCalls(t *testing.T, want map[string]int) {
	t.Helper()
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.calls) != len(want) {
		t.Fatalf("calls = %#v, want %#v", s.calls, want)
	}
	for method, count := range want {
		if s.calls[method] != count {
			t.Errorf("%s calls = %d, want %d", method, s.calls[method], count)
		}
	}
}

func TestLoadDeviceTemplateSettingsUsesPageSpecificQueries(t *testing.T) {
	backend := newSettingsBackendSpy()
	backend.settings.DisplayName = "Test Console"
	for index := 0; index < 10; index++ {
		backend.templates.Templates = append(backend.templates.Templates, model.DeviceTemplateManagementItem{
			TemplateID:   "template-" + string(rune('a'+index)),
			TemplateName: "Template",
		})
	}

	page := NewConsoleService(backend).LoadDeviceTemplateSettings(context.Background(), 2)
	backend.assertCalls(t, map[string]int{"system": 1, "templates": 1})
	if page.DeviceTemplatePagination.Page != 2 || len(page.DeviceTemplateManagement.Templates) != 2 {
		t.Fatalf("unexpected pagination: %#v, items=%d", page.DeviceTemplatePagination, len(page.DeviceTemplateManagement.Templates))
	}
	if page.Settings.DisplayName != "Test Console" {
		t.Fatalf("display name = %q", page.Settings.DisplayName)
	}
}

func TestLoadDeviceTemplateEditorReusesTemplateQuery(t *testing.T) {
	backend := newSettingsBackendSpy()
	backend.templates.Templates = []model.DeviceTemplateManagementItem{{
		TemplateID:   "custom-meter",
		TemplateName: "Custom Meter",
		Editable:     true,
	}}

	page, definition, found, err := NewConsoleService(backend).LoadDeviceTemplateEditorSettings(
		context.Background(), 1, "custom-meter",
	)
	if err != nil {
		t.Fatal(err)
	}
	backend.assertCalls(t, map[string]int{"system": 1, "templates": 1})
	if !found || definition.ID != "custom-meter" || definition.DisplayName != "Custom Meter" {
		t.Fatalf("editable template = (%#v, %t)", definition, found)
	}
	if page.DeviceTemplatePagination.TotalItems != 1 {
		t.Fatalf("template total = %d", page.DeviceTemplatePagination.TotalItems)
	}
}

func TestLoadMqttSettingsPageUsesOnlyMqttQueries(t *testing.T) {
	backend := newSettingsBackendSpy()
	backend.settings.DisplayName = "Test Console"
	backend.mqtt.BrokerHost = "broker.example"
	backend.runtime.Connected = true

	page := NewConsoleService(backend).LoadMqttSettingsPage(context.Background())
	backend.assertCalls(t, map[string]int{"system": 1, "mqtt": 1, "mqtt-runtime": 1})
	if page.Mqtt.BrokerHost != "broker.example" || !page.MqttRuntime.Connected {
		t.Fatalf("unexpected MQTT page data: %#v %#v", page.Mqtt, page.MqttRuntime)
	}
}
