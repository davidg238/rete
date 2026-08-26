// Copyright (c) 2026 Ekorau LLC

// internal/httpsrv/health_test.go
package httpsrv

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/davidg238/porta/internal/serverstat"
)

// fakeNodes stands in for whatever store is current. httpsrv depends on the
// question (how many nodes?) and not on the answerer, so its tests need no
// database at all — which is the point of the NodeCounter seam.
type fakeNodes struct {
	n   int
	err error
}

func (f fakeNodes) CountNodes() (int, error) { return f.n, f.err }

func testNodes(t *testing.T) NodeCounter {
	t.Helper()
	return fakeNodes{}
}

func decodeHealth(t *testing.T, h http.HandlerFunc) (int, map[string]any) {
	t.Helper()
	w := httptest.NewRecorder()
	h.ServeHTTP(w, httptest.NewRequest("GET", "/health", nil))
	var body map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("body not valid JSON: %v (body=%s)", err, w.Body.String())
	}
	return w.Code, body
}

func TestHealthHandlerReturnsOK(t *testing.T) {
	w := httptest.NewRecorder()
	healthHandler(testNodes(t), nil).ServeHTTP(w, httptest.NewRequest("GET", "/health", nil))
	if w.Code != http.StatusOK {
		t.Errorf("got %d, want 200", w.Code)
	}
	if ct := w.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("Content-Type = %q, want application/json", ct)
	}
	var body struct {
		Status string `json:"status"`
		Nodes  int    `json:"nodes"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("body not valid JSON: %v (body=%s)", err, w.Body.String())
	}
	if body.Status != "ok" {
		t.Errorf("status=%q, want ok", body.Status)
	}
	if body.Nodes != 0 {
		t.Errorf("nodes=%d, want 0 (fresh store)", body.Nodes)
	}
}

func TestHealthHandlerCountsNodes(t *testing.T) {
	code, body := decodeHealth(t, healthHandler(fakeNodes{n: 3}, nil))
	if code != http.StatusOK {
		t.Errorf("got %d, want 200", code)
	}
	if body["nodes"] != float64(3) {
		t.Errorf("nodes=%v, want 3", body["nodes"])
	}
}

// /health is a liveness probe for the LISTENER. A store that cannot answer
// must not turn it red, or an ops dashboard reports the gateway down for what
// is a database blip.
func TestHealthHandlerStaysOKWhenCountFails(t *testing.T) {
	code, body := decodeHealth(t, healthHandler(fakeNodes{n: 9, err: errors.New("db is busy")}, nil))
	if code != http.StatusOK {
		t.Errorf("got %d, want 200 despite the count failing", code)
	}
	if body["status"] != "ok" {
		t.Errorf("status=%v, want ok", body["status"])
	}
	if body["nodes"] != float64(-1) {
		t.Errorf("nodes=%v, want -1 (unknown, not the stale 9)", body["nodes"])
	}
}

// During the space-sync rewrite serve runs with no store at all, so a nil
// counter is a supported state and not a panic.
func TestHealthHandlerToleratesNilCounter(t *testing.T) {
	code, body := decodeHealth(t, healthHandler(nil, nil))
	if code != http.StatusOK {
		t.Errorf("got %d, want 200", code)
	}
	if body["nodes"] != float64(-1) {
		t.Errorf("nodes=%v, want -1", body["nodes"])
	}
}

// rete#176: when the bulk plane could not bind, /health carries the reason.
// The status stays "ok" — the exchange (what /health probes) IS serving; the
// field is the loud part. Absent when the plane is up, so probes keying on
// body shape see nothing new in the healthy case.
func TestHealthHandlerReportsBlocksPlaneDown(t *testing.T) {
	s := serverstat.New("1.0.0", "abc", nil)
	statsFn := func() *serverstat.Stats { return s }

	_, body := decodeHealth(t, healthHandler(testNodes(t), statsFn))
	if _, present := body["blocks_plane_down"]; present {
		t.Fatalf("plane up: blocks_plane_down should be absent, body=%v", body)
	}

	s.BlocksPlaneDown("address already in use")
	code, body := decodeHealth(t, healthHandler(testNodes(t), statsFn))
	if code != http.StatusOK {
		t.Fatalf("degraded /health = %d, want 200 (the exchange is serving)", code)
	}
	if got, _ := body["blocks_plane_down"].(string); got != "address already in use" {
		t.Fatalf("blocks_plane_down=%q, want the bind error, body=%v", got, body)
	}
}
