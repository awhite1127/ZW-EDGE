package service

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"path/filepath"
	"testing"
	"time"

	"edge-web/internal/ipc"
	"edge-web/internal/model"
)

type eventHistoryIPCRequest struct {
	ID     string                  `json:"id"`
	Method string                  `json:"method"`
	Params model.EventHistoryQuery `json:"params"`
}

func TestQueryServiceEventsIPCContract(t *testing.T) {
	socketPath := filepath.Join(t.TempDir(), "events.sock")
	listener, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Skipf("unix sockets unavailable: %v", err)
	}
	defer listener.Close()

	requestCh := make(chan eventHistoryIPCRequest, 1)
	serverErrCh := make(chan error, 1)
	go serveEventHistoryContract(listener, requestCh, serverErrCh)

	query := model.EventHistoryQuery{
		Level: "warning", Source: "polling", TimeRange: "24h", Search: "ground", Page: 3, PageSize: 20,
	}
	client := ipc.NewClient(socketPath, 2*time.Second)
	result, err := NewBackendServiceWithTimeout(client, 2*time.Second).QueryServiceEvents(context.Background(), query)
	if err != nil {
		t.Fatalf("QueryServiceEvents failed: %v", err)
	}

	request := <-requestCh
	if request.Method != "query_service_events" {
		t.Fatalf("IPC method mismatch: got %q", request.Method)
	}
	if request.Params != query {
		t.Fatalf("IPC params mismatch: got %+v, want %+v", request.Params, query)
	}
	if result.Total != 41 || len(result.Rows) != 1 || result.Rows[0].EventID != "evt-contract" {
		t.Fatalf("IPC result rows/total mismatch: %+v", result)
	}
	if result.LevelStats != (model.EventLevelStats{Error: 7, Warning: 11, Info: 23}) {
		t.Fatalf("IPC level stats mismatch: %+v", result.LevelStats)
	}
	if len(result.SourceStats) != 1 || result.SourceStats[0] != (model.EventSourceStat{Source: "polling", Count: 11}) {
		t.Fatalf("IPC source stats mismatch: %+v", result.SourceStats)
	}
	if serverErr := <-serverErrCh; serverErr != nil {
		t.Fatal(serverErr)
	}
}

func serveEventHistoryContract(
	listener net.Listener,
	requestCh chan<- eventHistoryIPCRequest,
	serverErrCh chan<- error,
) {
	conn, err := listener.Accept()
	if err != nil {
		serverErrCh <- fmt.Errorf("accept IPC connection: %w", err)
		return
	}
	defer conn.Close()

	payload, err := readEventHistoryTestFrame(conn)
	if err != nil {
		serverErrCh <- err
		return
	}
	var request eventHistoryIPCRequest
	if err := json.Unmarshal(payload, &request); err != nil {
		serverErrCh <- fmt.Errorf("decode IPC request: %w", err)
		return
	}
	requestCh <- request

	response := struct {
		ID      string                   `json:"id"`
		Success bool                     `json:"success"`
		Result  model.EventHistoryResult `json:"result"`
	}{
		ID:      request.ID,
		Success: true,
		Result: model.EventHistoryResult{
			Rows:        []model.ServiceEvent{{EventID: "evt-contract", Level: "warning", Source: "polling"}},
			Total:       41,
			LevelStats:  model.EventLevelStats{Error: 7, Warning: 11, Info: 23},
			SourceStats: []model.EventSourceStat{{Source: "polling", Count: 11}},
		},
	}
	responsePayload, err := json.Marshal(response)
	if err != nil {
		serverErrCh <- fmt.Errorf("encode IPC response: %w", err)
		return
	}
	if err := writeEventHistoryTestFrame(conn, responsePayload); err != nil {
		serverErrCh <- err
		return
	}
	serverErrCh <- nil
}

func readEventHistoryTestFrame(reader io.Reader) ([]byte, error) {
	var header [4]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return nil, fmt.Errorf("read IPC request header: %w", err)
	}
	payload := make([]byte, binary.BigEndian.Uint32(header[:]))
	if _, err := io.ReadFull(reader, payload); err != nil {
		return nil, fmt.Errorf("read IPC request payload: %w", err)
	}
	return payload, nil
}

func writeEventHistoryTestFrame(writer io.Writer, payload []byte) error {
	var header [4]byte
	binary.BigEndian.PutUint32(header[:], uint32(len(payload)))
	if _, err := writer.Write(header[:]); err != nil {
		return fmt.Errorf("write IPC response header: %w", err)
	}
	if _, err := writer.Write(payload); err != nil {
		return fmt.Errorf("write IPC response payload: %w", err)
	}
	return nil
}
