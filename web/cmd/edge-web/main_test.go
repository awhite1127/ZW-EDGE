package main

import (
	"context"
	"errors"
	"net/http"
	"sync"
	"testing"
	"time"
)

type fakeLifecycleServer struct {
	mu              sync.Mutex
	listenResult    chan error
	shutdownErr     error
	shutdownCalled  bool
	closeCalled     bool
	listenerStopped bool
}

func newFakeLifecycleServer() *fakeLifecycleServer {
	return &fakeLifecycleServer{listenResult: make(chan error, 1)}
}

func (s *fakeLifecycleServer) ListenAndServe() error {
	return <-s.listenResult
}

func (s *fakeLifecycleServer) Shutdown(context.Context) error {
	s.mu.Lock()
	s.shutdownCalled = true
	err := s.shutdownErr
	if err == nil && !s.listenerStopped {
		s.listenerStopped = true
		s.listenResult <- http.ErrServerClosed
	}
	s.mu.Unlock()
	return err
}

func (s *fakeLifecycleServer) Close() error {
	s.mu.Lock()
	s.closeCalled = true
	if !s.listenerStopped {
		s.listenerStopped = true
		s.listenResult <- http.ErrServerClosed
	}
	s.mu.Unlock()
	return nil
}

func TestTemplateDirFromEnvUsesEmbeddedTemplatesOnlyWhenUnset(t *testing.T) {
	t.Setenv("EDGE_WEB_TEMPLATE_DIR", "  ")
	if actual := templateDirFromEnv(); actual != "" {
		t.Fatalf("templateDirFromEnv() = %q, want embedded templates", actual)
	}
}

func TestTemplateDirFromEnvKeepsEveryExplicitOverride(t *testing.T) {
	for _, value := range []string{"templates", "./templates", "/tmp/edge-web-templates"} {
		t.Run(value, func(t *testing.T) {
			t.Setenv("EDGE_WEB_TEMPLATE_DIR", "  "+value+"  ")
			if actual := templateDirFromEnv(); actual != value {
				t.Fatalf("templateDirFromEnv() = %q, want %q", actual, value)
			}
		})
	}
}

func TestServeUntilDoneReturnsListenerFailureAfterCleanup(t *testing.T) {
	server := newFakeLifecycleServer()
	wantErr := errors.New("listen failed")
	server.listenResult <- wantErr

	err := serveUntilDone(context.Background(), server, time.Second)
	if !errors.Is(err, wantErr) {
		t.Fatalf("serveUntilDone() error = %v, want listener failure", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if !server.closeCalled {
		t.Fatal("listener failure must still close server resources")
	}
}

func TestServeUntilDoneGracefullyShutsDownOnCancellation(t *testing.T) {
	server := newFakeLifecycleServer()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	if err := serveUntilDone(ctx, server, time.Second); err != nil {
		t.Fatalf("serveUntilDone() = %v", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if !server.shutdownCalled || !server.closeCalled {
		t.Fatalf("shutdown=%t close=%t, want both lifecycle paths", server.shutdownCalled, server.closeCalled)
	}
}

func TestServeUntilDoneReturnsShutdownFailureWithoutFatalExit(t *testing.T) {
	server := newFakeLifecycleServer()
	wantErr := errors.New("shutdown failed")
	server.shutdownErr = wantErr
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	err := serveUntilDone(ctx, server, 10*time.Millisecond)
	if !errors.Is(err, wantErr) {
		t.Fatalf("serveUntilDone() error = %v, want shutdown failure", err)
	}
	server.mu.Lock()
	defer server.mu.Unlock()
	if !server.closeCalled {
		t.Fatal("shutdown failure must force-close server resources")
	}
}
