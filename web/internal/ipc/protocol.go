package ipc

// IPC 帧采用固定长度前缀限制消息边界，最大尺寸用于阻止异常后端响应耗尽 Web 进程内存。

const (
	// frameHeaderBytes 是 IPC 协议固定使用的 4 字节大端长度头。
	frameHeaderBytes = 4
	// maxPayloadBytes 需要和 backend 保持一致，避免两端对边界判断不同。
	maxPayloadBytes = 8 * 1024 * 1024
	// maxConcurrentCalls 限制 Web 进程同时打到后端 UDS 的 IPC 请求数，避免刷新风暴压垮后端。
	// 与后端固定 worker 数一致，让额外请求在 Go 侧可取消排队，避免占用
	// UDS 连接和后端队列内存。
	maxConcurrentCalls = 4
)
