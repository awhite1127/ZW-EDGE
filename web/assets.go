package webassets

// 模板和静态资源嵌入二进制，生产部署不依赖外部资源目录；开发模式仍可显式使用磁盘目录。

import "embed"

// StaticFS 嵌入 Web 静态资源，板端部署时不再依赖外部 static 目录。
//
//go:embed static/*
var StaticFS embed.FS

// TemplateFS 将页面模板与静态资源一起固化进同一个 edge-web 版本，
// 避免只替换二进制时继续加载板端旧模板和旧静态资源版本号。
//
//go:embed templates/*.html templates/partials/*.html
var TemplateFS embed.FS
