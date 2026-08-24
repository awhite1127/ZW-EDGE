package httpserver

import (
	"compress/gzip"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	webassets "edge-web"
)

func embeddedStaticTestHandler(t *testing.T) http.Handler {
	t.Helper()
	t.Setenv("EDGE_WEB_STATIC_DIR", "")
	return staticAssetCacheMiddleware(http.StripPrefix("/static/", staticFileServer("")))
}

func TestEmbeddedStaticAssetsServePrecompressedGzip(t *testing.T) {
	handler := embeddedStaticTestHandler(t)
	raw, err := webassets.StaticFS.ReadFile("static/app.css")
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodGet, "/static/app.css?v=test-version", nil)
	request.Header.Set("Accept-Encoding", "br, gzip")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d", response.Code)
	}
	if got := response.Header().Get("Content-Encoding"); got != "gzip" {
		t.Fatalf("Content-Encoding = %q", got)
	}
	if got := response.Header().Get("Content-Type"); got != "text/css; charset=utf-8" {
		t.Fatalf("Content-Type = %q", got)
	}
	if !strings.Contains(response.Header().Get("Vary"), "Accept-Encoding") {
		t.Fatalf("Vary = %q", response.Header().Get("Vary"))
	}
	if got := response.Header().Get("Cache-Control"); got != "public, max-age=31536000, immutable" {
		t.Fatalf("Cache-Control = %q", got)
	}
	reader, err := gzip.NewReader(response.Body)
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := io.ReadAll(reader)
	if err != nil {
		t.Fatal(err)
	}
	if err := reader.Close(); err != nil {
		t.Fatal(err)
	}
	if string(decoded) != string(raw) {
		t.Fatal("decoded gzip body differs from embedded app.css")
	}
}

func TestEmbeddedStaticAssetsRespectIdentityAndGzipQuality(t *testing.T) {
	handler := embeddedStaticTestHandler(t)
	raw, err := webassets.StaticFS.ReadFile("static/app.css")
	if err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodGet, "/static/app.css", nil)
	request.Header.Set("Accept-Encoding", "gzip;q=0, *;q=1")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d", response.Code)
	}
	if got := response.Header().Get("Content-Encoding"); got != "" {
		t.Fatalf("Content-Encoding = %q", got)
	}
	if !strings.Contains(response.Header().Get("Vary"), "Accept-Encoding") {
		t.Fatalf("Vary = %q", response.Header().Get("Vary"))
	}
	if got := response.Header().Get("Cache-Control"); got != "public, max-age=300, must-revalidate" {
		t.Fatalf("Cache-Control = %q", got)
	}
	if response.Body.String() != string(raw) {
		t.Fatal("identity body differs from embedded app.css")
	}
}

func TestEmbeddedStaticAssetsServeGzipHEADWithoutBody(t *testing.T) {
	handler := embeddedStaticTestHandler(t)

	getRequest := httptest.NewRequest(http.MethodGet, "/static/app-core.js?v=test-version", nil)
	getRequest.Header.Set("Accept-Encoding", "gzip")
	getResponse := httptest.NewRecorder()
	handler.ServeHTTP(getResponse, getRequest)

	headRequest := httptest.NewRequest(http.MethodHead, "/static/app-core.js?v=test-version", nil)
	headRequest.Header.Set("Accept-Encoding", "gzip")
	headResponse := httptest.NewRecorder()
	handler.ServeHTTP(headResponse, headRequest)

	if headResponse.Code != http.StatusOK {
		t.Fatalf("status = %d", headResponse.Code)
	}
	if headResponse.Body.Len() != 0 {
		t.Fatalf("HEAD body length = %d", headResponse.Body.Len())
	}
	if got := headResponse.Header().Get("Content-Encoding"); got != "gzip" {
		t.Fatalf("Content-Encoding = %q", got)
	}
	if got := headResponse.Header().Get("Content-Type"); got != "text/javascript; charset=utf-8" {
		t.Fatalf("Content-Type = %q", got)
	}
	wantLength := strconv.Itoa(getResponse.Body.Len())
	if got := headResponse.Header().Get("Content-Length"); got != wantLength {
		t.Fatalf("Content-Length = %q, want %q", got, wantLength)
	}
}

func TestStaticDirectoryOverrideRemainsUncompressed(t *testing.T) {
	directory := t.TempDir()
	const content = "console.log('development override');\n"
	if err := os.WriteFile(filepath.Join(directory, "app-core.js"), []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("EDGE_WEB_STATIC_DIR", directory)
	handler := staticAssetCacheMiddleware(http.StripPrefix("/static/", staticFileServer("")))
	request := httptest.NewRequest(http.MethodGet, "/static/app-core.js?v=dev", nil)
	request.Header.Set("Accept-Encoding", "gzip")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d", response.Code)
	}
	if got := response.Header().Get("Content-Encoding"); got != "" {
		t.Fatalf("development override was unexpectedly compressed: %q", got)
	}
	if response.Body.String() != content {
		t.Fatalf("body = %q", response.Body.String())
	}
}
