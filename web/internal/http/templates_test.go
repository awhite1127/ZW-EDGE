package httpserver

import "testing"

func TestEmbeddedPageTemplatesParse(t *testing.T) {
	templates, err := loadPageTemplates("")
	if err != nil {
		t.Fatalf("load embedded templates: %v", err)
	}
	if templates["overview"] == nil || templates["operations"] == nil {
		t.Fatal("overview or operations template missing")
	}
}
