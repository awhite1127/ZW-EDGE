package service

import (
	"archive/zip"
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"testing"
	"time"

	"edge-web/internal/ipc"
	"edge-web/internal/model"
)

// controller 不可连接时，诊断导出仍必须携带安装版本、发布信息、升级状态和升级日志。
func TestDiagnosticExportKeepsStaticProductFactsWhenControllerUnavailable(t *testing.T) {
	root := t.TempDir()
	logDir := filepath.Join(root, "log")
	if err := os.MkdirAll(filepath.Join(root, "update"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(logDir, 0o755); err != nil {
		t.Fatal(err)
	}
	files := map[string]string{
		"VERSION":            "0.9.0\n",
		"package-info.json":  "{\"version\":\"0.9.0\"}\n",
		"update/status.json": "{\"state\":\"ROLLED_BACK\"}\n",
		"log/update.log":     "upgrade failed and rollback completed\n",
		"log/update.log.1":   "previous upgrade log\n",
	}
	for name, content := range files {
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.WriteFile(path, []byte(content), 0o640); err != nil {
			t.Fatal(err)
		}
	}

	previousRoot := diagnosticProductRoot
	diagnosticProductRoot = root
	t.Cleanup(func() { diagnosticProductRoot = previousRoot })
	t.Setenv("EDGE_CONTROLLER_LOG_DIR", logDir)

	backend := NewBackendServiceWithTimeout(
		ipc.NewClient(filepath.Join(root, "controller-unavailable.sock"), 20*time.Millisecond),
		20*time.Millisecond,
	)
	console := NewConsoleService(backend)
	var output bytes.Buffer
	if err := console.WriteDiagnosticExport(
		context.Background(), &output, model.EventExportQuery{}, time.Unix(1_700_000_000, 0)); err != nil {
		t.Fatalf("WriteDiagnosticExport failed: %v", err)
	}

	archive, err := zip.NewReader(bytes.NewReader(output.Bytes()), int64(output.Len()))
	if err != nil {
		t.Fatalf("open diagnostic zip: %v", err)
	}
	want := map[string]string{
		"product/VERSION":            files["VERSION"],
		"product/package-info.json":  files["package-info.json"],
		"product/update-status.json": files["update/status.json"],
		"logs/update.log":            files["log/update.log"],
		"logs/update.log.1":          files["log/update.log.1"],
	}
	for _, entry := range archive.File {
		expected, ok := want[entry.Name]
		if !ok {
			continue
		}
		reader, openErr := entry.Open()
		if openErr != nil {
			t.Fatal(openErr)
		}
		content, readErr := io.ReadAll(reader)
		_ = reader.Close()
		if readErr != nil {
			t.Fatal(readErr)
		}
		if string(content) != expected {
			t.Errorf("%s content mismatch: %q", entry.Name, content)
		}
		delete(want, entry.Name)
	}
	for missing := range want {
		t.Errorf("diagnostic entry missing: %s", missing)
	}
}
