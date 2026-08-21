package ipc

// 本文件实现基于 Unix Domain Socket 的单次请求/响应 IPC 客户端，负责超时、帧读写和错误解码。

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"edge-web/internal/model"
)

// Client 封装 Go Web 到后端 UDS IPC 的一次完整请求响应调用。
// HTTP handler 不直接处理帧读写，统一通过这里和后端通信。
// nextID 原子递增以避免重复请求号，sem 限制并发 UDS 连接数并为后端 worker 池提供反压。
type Client struct {
	socketPath string
	timeout    time.Duration
	nextID     uint64
	sem        chan struct{}
	// dialContext 仅由包内测试替换；生产路径始终使用带 context 的 UDS dialer。
	dialContext func(context.Context) (net.Conn, error)
}

type slowCallLogState struct {
	lastLoggedAt time.Time
	suppressed   uint64
}

var slowCallLogs = struct {
	sync.Mutex
	byMethod map[string]slowCallLogState
}{
	byMethod: make(map[string]slowCallLogState),
}

// CallError 表示后端已正常返回，但业务结果为失败。
type CallError struct {
	Code    string
	Message string
}

// Error 判断或转换当前错误信息。
func (e *CallError) Error() string {
	if e == nil {
		return ""
	}
	return fmt.Sprintf("%s: %s", e.Code, e.Message)
}

// NewClient 创建并初始化对应服务对象。
func NewClient(socketPath string, timeout time.Duration) *Client {
	return &Client{
		socketPath: socketPath,
		timeout:    timeout,
		sem:        make(chan struct{}, maxConcurrentCalls),
	}
}

// Call 负责完成一次完整的 IPC 调用。
// 它会编码请求、建立 UDS 连接、写入帧、读取响应并反序列化结果。
func (c *Client) Call(ctx context.Context, method string, params interface{}, result interface{}) (err error) {
	return c.call(ctx, method, params, result, c.timeout)
}

// CallWithTimeout 仅用于已知会长时间阻塞的后端操作，例如 ntpd -gq。
func (c *Client) CallWithTimeout(ctx context.Context, method string, params interface{}, result interface{}, timeout time.Duration) error {
	if timeout <= 0 {
		timeout = c.timeout
	}
	return c.call(ctx, method, params, result, timeout)
}

// call 执行一次完整的 IPC 请求与响应。
func (c *Client) call(ctx context.Context, method string, params interface{}, result interface{}, operationTimeout time.Duration) (err error) {
	started := time.Now()
	defer func() {
		logSlowCall(method, time.Since(started), err)
	}()

	if ctx == nil {
		ctx = context.Background()
	}
	overallCtx, cancelOverall := context.WithTimeout(ctx, operationTimeout)
	defer cancelOverall()

	request := model.IPCRequest{
		ID:     strconv.FormatUint(atomic.AddUint64(&c.nextID, 1), 10),
		Method: method,
		Params: params,
	}

	payload, err := json.Marshal(request)
	if err != nil {
		return fmt.Errorf("编码 IPC 请求失败: %w", err)
	}

	// 编码不占用后端并发名额；排队中的 HTTP 请求仍可随客户端取消及时退出。
	if err := c.acquire(overallCtx); err != nil {
		return err
	}
	defer c.release()

	var conn net.Conn
	if c.dialContext != nil {
		conn, err = c.dialContext(overallCtx)
	} else {
		dialer := net.Dialer{}
		conn, err = dialer.DialContext(overallCtx, "unix", c.socketPath)
	}
	if err != nil {
		if contextErr := overallCtx.Err(); contextErr != nil {
			return fmt.Errorf("连接后端 IPC 已取消或超时: %w", contextErr)
		}
		return fmt.Errorf("连接后端 IPC 失败: %w", err)
	}
	defer conn.Close()

	deadline, ok := overallCtx.Deadline()
	if !ok {
		return fmt.Errorf("IPC 调用缺少截止时间")
	}
	if err := conn.SetDeadline(deadline); err != nil {
		return fmt.Errorf("设置 IPC 超时失败: %w", err)
	}
	// socket deadline 只反映创建连接时的截止时间；调用方提前取消时主动唤醒正在阻塞的读写。
	stopCancellationWakeup := context.AfterFunc(overallCtx, func() {
		_ = conn.SetDeadline(time.Now())
	})
	defer stopCancellationWakeup()

	if err := writeFrame(conn, payload); err != nil {
		if contextErr := overallCtx.Err(); contextErr != nil {
			return fmt.Errorf("写入后端 IPC 已取消或超时: %w", contextErr)
		}
		return err
	}

	responsePayload, err := readFrame(conn)
	if err != nil {
		if contextErr := overallCtx.Err(); contextErr != nil {
			return fmt.Errorf("读取后端 IPC 已取消或超时: %w", contextErr)
		}
		return err
	}

	return decodeResponse(request.ID, responsePayload, result)
}

// decodeResponse 校验一次请求/响应的关联关系和统一结果契约。
func decodeResponse(requestID string, responsePayload []byte, result interface{}) error {
	var response model.IPCResponse
	if err := json.Unmarshal(responsePayload, &response); err != nil {
		return fmt.Errorf("解析 IPC 响应失败: %w", err)
	}

	var responseID string
	if len(response.ID) == 0 || json.Unmarshal(response.ID, &responseID) != nil || responseID == "" {
		// 后端在固定 worker 全部占满时会在读取请求帧之前拒绝连接，因此无法回显
		// 请求 ID。该连接级错误是唯一允许缺少 ID 的响应；其他响应仍严格关联。
		if !response.Success && response.Error != nil && response.Error.Code == "server_busy" {
			return &CallError{Code: response.Error.Code, Message: response.Error.Message}
		}
		return fmt.Errorf("IPC 响应缺少有效请求 ID")
	}
	if responseID != requestID {
		return fmt.Errorf("IPC 响应 ID 不匹配: got %q, want %q", responseID, requestID)
	}

	if !response.Success {
		if response.Error == nil {
			return &CallError{Code: "ipc_error", Message: "后端返回失败，但没有附带错误详情"}
		}
		return &CallError{Code: response.Error.Code, Message: response.Error.Message}
	}

	if result == nil {
		return nil
	}
	if len(response.Result) == 0 || string(response.Result) == "null" {
		return fmt.Errorf("IPC 成功响应缺少业务结果")
	}
	if err := json.Unmarshal(response.Result, result); err != nil {
		return fmt.Errorf("解析业务结果失败: %w", err)
	}
	return nil
}

// logSlowCall 记录超过阈值的 IPC 慢调用。
func logSlowCall(method string, elapsed time.Duration, err error) {
	if elapsed < 200*time.Millisecond {
		return
	}
	suppressed, ok := takeSlowCallLogSlot(method, time.Now())
	if !ok {
		return
	}
	suffix := ""
	if suppressed > 0 {
		suffix = fmt.Sprintf("，期间已抑制=%d", suppressed)
	}
	if err != nil {
		log.Printf("IPC 调用较慢：method=%s，耗时=%s，错误=%s%s", method, elapsed.Round(time.Millisecond), truncateLogText(err.Error()), suffix)
		return
	}
	log.Printf("IPC 调用较慢：method=%s，耗时=%s%s", method, elapsed.Round(time.Millisecond), suffix)
}

// takeSlowCallLogSlot 对同一 IPC 方法的慢调用日志做固定窗口限频。
func takeSlowCallLogSlot(method string, now time.Time) (uint64, bool) {
	const interval = 30 * time.Second
	slowCallLogs.Lock()
	defer slowCallLogs.Unlock()

	state := slowCallLogs.byMethod[method]
	if !state.lastLoggedAt.IsZero() && now.Sub(state.lastLoggedAt) < interval {
		state.suppressed++
		slowCallLogs.byMethod[method] = state
		return 0, false
	}
	suppressed := state.suppressed
	slowCallLogs.byMethod[method] = slowCallLogState{lastLoggedAt: now}
	return suppressed, true
}

// truncateLogText 截断过长的日志文本。
func truncateLogText(value string) string {
	const maxLength = 160
	value = strings.TrimSpace(value)
	if len(value) <= maxLength {
		return value
	}
	return value[:maxLength] + "..."
}

// acquire 获取 IPC 并发槽位。
func (c *Client) acquire(ctx context.Context) error {
	if c == nil || c.sem == nil {
		return nil
	}
	select {
	case c.sem <- struct{}{}:
		return nil
	case <-ctx.Done():
		return fmt.Errorf("后端 IPC 请求等待已取消或超时，请稍后重试: %w", ctx.Err())
	}
}

// release 释放 IPC 并发槽位。
func (c *Client) release() {
	if c == nil || c.sem == nil {
		return
	}
	select {
	case <-c.sem:
	default:
	}
}

// writeFrame 把 JSON payload 写成“长度头 + 消息体”的稳定 IPC 帧。
func writeFrame(writer io.Writer, payload []byte) error {
	if len(payload) == 0 {
		return fmt.Errorf("IPC 载荷长度不能为 0")
	}
	if len(payload) > maxPayloadBytes {
		return fmt.Errorf("IPC 载荷长度超过上限: %d > %d", len(payload), maxPayloadBytes)
	}

	var header [frameHeaderBytes]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(payload)))
	if err := writeAll(writer, header[:]); err != nil {
		return fmt.Errorf("写入 IPC 长度头失败: %w", err)
	}
	if err := writeAll(writer, payload); err != nil {
		return fmt.Errorf("写入 IPC 请求体失败: %w", err)
	}
	return nil
}

// readFrame 先读取长度头，再按长度读取消息体，并在分配内存前做边界校验。
func readFrame(reader io.Reader) ([]byte, error) {
	var header [frameHeaderBytes]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return nil, fmt.Errorf("读取 IPC 长度头失败: %w", err)
	}

	length := binary.BigEndian.Uint32(header[:])
	if length == 0 {
		return nil, fmt.Errorf("IPC 载荷长度不能为 0")
	}
	if length > maxPayloadBytes {
		return nil, fmt.Errorf("IPC 载荷长度超过上限: %d > %d", length, maxPayloadBytes)
	}

	payload := make([]byte, length)
	if _, err := io.ReadFull(reader, payload); err != nil {
		return nil, fmt.Errorf("读取 IPC 响应体失败: %w", err)
	}
	return payload, nil
}

// writeAll 显式处理短写，避免依赖底层 Writer 一次写完整个缓冲区。
func writeAll(writer io.Writer, payload []byte) error {
	written := 0
	for written < len(payload) {
		n, err := writer.Write(payload[written:])
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrUnexpectedEOF
		}
		written += n
	}
	return nil
}
