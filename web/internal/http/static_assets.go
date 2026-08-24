package httpserver

import (
	"bytes"
	"compress/gzip"
	"io/fs"
	"log"
	"net/http"
	"os"
	"path"
	"strconv"
	"strings"
	"sync"
	"time"

	webassets "edge-web"
)

type compressedStaticAsset struct {
	contentType string
	gzipBytes   []byte
}

type embeddedStaticHandler struct {
	raw    http.Handler
	assets map[string]compressedStaticAsset
}

var embeddedStaticServer struct {
	sync.Once
	handler http.Handler
	err     error
}

func staticFileServer(staticDir string) http.Handler {
	// 开发覆盖目录必须保持所见即所得，不在内存中缓存或压缩磁盘文件。
	if override := strings.TrimSpace(os.Getenv("EDGE_WEB_STATIC_DIR")); override != "" {
		return http.FileServer(http.Dir(override))
	}

	handler, err := loadEmbeddedStaticHandler()
	if err != nil {
		log.Printf("加载或预压缩内嵌静态资源失败：%v", err)
	}
	if handler != nil {
		return handler
	}
	if strings.TrimSpace(staticDir) != "" {
		return http.FileServer(http.Dir(staticDir))
	}
	return http.NotFoundHandler()
}

func loadEmbeddedStaticHandler() (http.Handler, error) {
	embeddedStaticServer.Do(func() {
		staticFS, err := fs.Sub(webassets.StaticFS, "static")
		if err != nil {
			embeddedStaticServer.err = err
			return
		}
		embeddedStaticServer.handler, err = newEmbeddedStaticHandler(staticFS)
		if err != nil {
			// 压缩失败不应阻断 Web 启动，仍可安全回退到原始内嵌资源。
			embeddedStaticServer.handler = http.FileServer(http.FS(staticFS))
			embeddedStaticServer.err = err
		}
	})
	return embeddedStaticServer.handler, embeddedStaticServer.err
}

// newEmbeddedStaticHandler 在服务装配阶段一次性压缩 CSS/JS；请求阶段不再执行压缩或复制资源正文。
func newEmbeddedStaticHandler(staticFS fs.FS) (http.Handler, error) {
	handler := &embeddedStaticHandler{
		raw:    http.FileServer(http.FS(staticFS)),
		assets: make(map[string]compressedStaticAsset),
	}
	err := fs.WalkDir(staticFS, ".", func(name string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		extension := strings.ToLower(path.Ext(name))
		if extension != ".css" && extension != ".js" {
			return nil
		}
		content, err := fs.ReadFile(staticFS, name)
		if err != nil {
			return err
		}
		var compressed bytes.Buffer
		writer, err := gzip.NewWriterLevel(&compressed, gzip.BestCompression)
		if err != nil {
			return err
		}
		if _, err = writer.Write(content); err != nil {
			_ = writer.Close()
			return err
		}
		if err = writer.Close(); err != nil {
			return err
		}
		contentType := "text/javascript; charset=utf-8"
		if extension == ".css" {
			contentType = "text/css; charset=utf-8"
		}
		assetPath := "/" + strings.TrimPrefix(name, "./")
		handler.assets[assetPath] = compressedStaticAsset{
			contentType: contentType,
			gzipBytes:   append([]byte(nil), compressed.Bytes()...),
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	return handler, nil
}

func (h *embeddedStaticHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	assetPath := path.Clean("/" + strings.TrimPrefix(r.URL.Path, "/"))
	asset, compressible := h.assets[assetPath]
	if !compressible {
		h.raw.ServeHTTP(w, r)
		return
	}
	addVaryHeader(w.Header(), "Accept-Encoding")
	if !requestAcceptsGzip(r.Header.Get("Accept-Encoding")) {
		h.raw.ServeHTTP(w, r)
		return
	}

	w.Header().Set("Content-Encoding", "gzip")
	w.Header().Set("Content-Type", asset.contentType)
	// ServeContent 会把 Content-Encoding 视为外层动态压缩并省略长度；这里的正文已经固定，
	// 普通 GET/HEAD 可安全声明长度。Range 交回 ServeContent，避免把完整长度带到部分响应。
	if strings.TrimSpace(r.Header.Get("Range")) == "" {
		w.Header().Set("Content-Length", strconv.Itoa(len(asset.gzipBytes)))
	}
	http.ServeContent(w, r, path.Base(assetPath), time.Time{}, bytes.NewReader(asset.gzipBytes))
}

func requestAcceptsGzip(header string) bool {
	explicitQuality := -1.0
	wildcardQuality := -1.0
	for _, item := range strings.Split(header, ",") {
		parts := strings.Split(item, ";")
		coding := strings.ToLower(strings.TrimSpace(parts[0]))
		if coding == "" {
			continue
		}
		quality := 1.0
		for _, parameter := range parts[1:] {
			name, value, found := strings.Cut(strings.TrimSpace(parameter), "=")
			if !found || !strings.EqualFold(strings.TrimSpace(name), "q") {
				continue
			}
			parsed, err := strconv.ParseFloat(strings.TrimSpace(value), 64)
			if err != nil || parsed < 0 || parsed > 1 {
				quality = 0
			} else {
				quality = parsed
			}
		}
		switch coding {
		case "gzip":
			explicitQuality = quality
		case "*":
			wildcardQuality = quality
		}
	}
	if explicitQuality >= 0 {
		return explicitQuality > 0
	}
	return wildcardQuality > 0
}

func addVaryHeader(header http.Header, value string) {
	for _, current := range header.Values("Vary") {
		for _, item := range strings.Split(current, ",") {
			if strings.EqualFold(strings.TrimSpace(item), value) {
				return
			}
		}
	}
	header.Add("Vary", value)
}

// staticAssetCacheMiddleware 根据资源版本参数设置缓存策略。
// 页面模板为所有可执行静态资源携带版本号；未带版本号的资源只做短缓存并强制重新验证，
// 避免升级后浏览器长期沿用旧文件。
func staticAssetCacheMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.TrimSpace(r.URL.Query().Get("v")) != "" {
			w.Header().Set("Cache-Control", "public, max-age=31536000, immutable")
		} else {
			w.Header().Set("Cache-Control", "public, max-age=300, must-revalidate")
		}
		next.ServeHTTP(w, r)
	})
}
