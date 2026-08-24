package httpserver

import (
	"html/template"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"testing"

	webassets "edge-web"
)

func TestEmbeddedPageTemplatesParse(t *testing.T) {
	templates, err := loadPageTemplates("")
	if err != nil {
		t.Fatalf("load embedded templates: %v", err)
	}
	assertPageTemplateIsolation(t, templates)
}

func TestDiskPageTemplatesParse(t *testing.T) {
	directory := t.TempDir()
	err := fs.WalkDir(webassets.TemplateFS, "templates", func(name string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() || !strings.HasSuffix(name, ".html") {
			return nil
		}
		content, readErr := fs.ReadFile(webassets.TemplateFS, name)
		if readErr != nil {
			return readErr
		}
		target := filepath.Join(directory, filepath.FromSlash(strings.TrimPrefix(name, "templates/")))
		if mkdirErr := os.MkdirAll(filepath.Dir(target), 0o755); mkdirErr != nil {
			return mkdirErr
		}
		return os.WriteFile(target, content, 0o600)
	})
	if err != nil {
		t.Fatal(err)
	}
	templates, err := loadPageTemplates(directory)
	if err != nil {
		t.Fatalf("load disk templates: %v", err)
	}
	assertPageTemplateIsolation(t, templates)
}

func assertPageTemplateIsolation(t *testing.T, templates map[string]*template.Template) {
	t.Helper()
	if templates["overview"] == nil || templates["operations"] == nil {
		t.Fatal("overview or operations template missing")
	}
	for _, partial := range []string{"channels_panel", "masters_panel", "devices_panel"} {
		if templates["collection"].Lookup(partial) == nil {
			t.Errorf("collection partial %q missing", partial)
		}
		if templates["overview"].Lookup(partial) != nil {
			t.Errorf("unrelated overview template parsed collection partial %q", partial)
		}
	}
}
