package ipc

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"edge-web/internal/model"
)

func TestFrameRoundTripAndLimits(t *testing.T) {
	payload := []byte(`{"method":"ping"}`)
	var framed bytes.Buffer
	if err := writeFrame(&framed, payload); err != nil {
		t.Fatal(err)
	}
	decoded, err := readFrame(&framed)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(decoded, payload) {
		t.Fatalf("decoded payload = %q", decoded)
	}

	if err := writeFrame(io.Discard, nil); err == nil {
		t.Fatal("empty frame was accepted")
	}
	if err := writeFrame(io.Discard, make([]byte, maxPayloadBytes+1)); err == nil {
		t.Fatal("oversized frame was accepted")
	}

	var zeroHeader [frameHeaderBytes]byte
	if _, err := readFrame(bytes.NewReader(zeroHeader[:])); err == nil {
		t.Fatal("zero-length response frame was accepted")
	}
	var oversizedHeader [frameHeaderBytes]byte
	binary.BigEndian.PutUint32(oversizedHeader[:], maxPayloadBytes+1)
	if _, err := readFrame(bytes.NewReader(oversizedHeader[:])); err == nil {
		t.Fatal("oversized response frame was accepted")
	}
	var truncated bytes.Buffer
	binary.Write(&truncated, binary.BigEndian, uint32(4))
	truncated.WriteString("ab")
	if _, err := readFrame(&truncated); !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatalf("truncated frame error = %v", err)
	}
}

type zeroWriter struct{}

func (zeroWriter) Write([]byte) (int, error) { return 0, nil }

func TestWriteAllRejectsWriterWithoutProgress(t *testing.T) {
	if err := writeAll(zeroWriter{}, []byte("payload")); !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Fatalf("writeAll error = %v", err)
	}
}

func TestDecodeResponseContract(t *testing.T) {
	var result struct {
		Value int `json:"value"`
	}
	if err := decodeResponse("7", []byte(`{"id":"7","success":true,"result":{"value":42}}`), &result); err != nil {
		t.Fatal(err)
	}
	if result.Value != 42 {
		t.Fatalf("result value = %d", result.Value)
	}

	tests := []struct {
		name    string
		payload string
		want    string
	}{
		{name: "mismatched id", payload: `{"id":"8","success":true,"result":{}}`, want: "ID 不匹配"},
		{name: "missing id", payload: `{"success":true,"result":{}}`, want: "缺少有效请求 ID"},
		{name: "missing result", payload: `{"id":"7","success":true}`, want: "缺少业务结果"},
		{name: "business error", payload: `{"id":"7","success":false,"error":{"code":"bad","message":"failed"}}`, want: "bad: failed"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			err := decodeResponse("7", []byte(test.payload), &struct{}{})
			if err == nil || !strings.Contains(err.Error(), test.want) {
				t.Fatalf("error = %v, want substring %q", err, test.want)
			}
		})
	}

	err := decodeResponse(
		"7",
		[]byte(`{"success":false,"error":{"code":"server_busy","message":"busy"}}`),
		nil,
	)
	var callError *CallError
	if !errors.As(err, &callError) || callError.Code != "server_busy" {
		t.Fatalf("server busy error = %#v", err)
	}
}

func TestClientCallRoundTrip(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	defer serverConn.Close()
	client := NewClient("", time.Second)
	client.dialContext = func(context.Context) (net.Conn, error) {
		return clientConn, nil
	}

	serverErr := make(chan error, 1)
	go func() {
		payload, err := readFrame(serverConn)
		if err != nil {
			serverErr <- err
			return
		}
		var request model.IPCRequest
		if err := json.Unmarshal(payload, &request); err != nil {
			serverErr <- err
			return
		}
		if request.Method != "ping" || request.ID == "" {
			serverErr <- errors.New("unexpected IPC request")
			return
		}
		response, err := json.Marshal(map[string]interface{}{
			"id":      request.ID,
			"success": true,
			"result":  map[string]int{"value": 9},
		})
		if err == nil {
			err = writeFrame(serverConn, response)
		}
		serverErr <- err
	}()

	var result struct {
		Value int `json:"value"`
	}
	if err := client.Call(context.Background(), "ping", map[string]bool{"ready": true}, &result); err != nil {
		t.Fatal(err)
	}
	if result.Value != 9 {
		t.Fatalf("result value = %d", result.Value)
	}
	if err := <-serverErr; err != nil {
		t.Fatal(err)
	}
}

func TestClientCallCancellationWakesBlockedRead(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	client := NewClient("", time.Second)
	client.dialContext = func(context.Context) (net.Conn, error) {
		return clientConn, nil
	}
	releaseServer := make(chan struct{})
	go func() {
		_, _ = readFrame(serverConn)
		<-releaseServer
		_ = serverConn.Close()
	}()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Millisecond)
	defer cancel()
	started := time.Now()
	err := client.Call(ctx, "blocked", nil, &struct{}{})
	close(releaseServer)
	if err == nil || !strings.Contains(err.Error(), "取消或超时") {
		t.Fatalf("cancellation error = %v", err)
	}
	if elapsed := time.Since(started); elapsed > 500*time.Millisecond {
		t.Fatalf("cancellation took %s", elapsed)
	}
}

func TestAcquireHonorsContextCancellation(t *testing.T) {
	client := NewClient("", time.Second)
	for range maxConcurrentCalls {
		client.sem <- struct{}{}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := client.acquire(ctx); err == nil {
		t.Fatal("acquire ignored canceled context")
	}
}
