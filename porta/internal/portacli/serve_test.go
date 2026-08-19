// Copyright (c) 2026 Ekorau LLC

// internal/portacli/serve_test.go
package portacli

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/space"
)

// errSentinel is a recognizable listener error for awaitServeExit tests.
var errSentinel = errors.New("sentinel listener failure")

func TestDefaultAllowCIDRHasRFC1918LoopbackAndTailscale(t *testing.T) {
	got := defaultAllowCIDR()
	want := []string{
		"10.0.0.0/8",
		"172.16.0.0/12",
		"192.168.0.0/16",
		"100.64.0.0/10",
		"127.0.0.0/8",
		"::1/128",
	}
	if len(got) != len(want) {
		t.Fatalf("got %d entries, want %d", len(got), len(want))
	}
	for i, w := range want {
		if got[i] != w {
			t.Errorf("entry %d = %q, want %q", i, got[i], w)
		}
	}
}

func TestDefaultAllowCIDRReturnsFreshSlice(t *testing.T) {
	a := defaultAllowCIDR()
	b := defaultAllowCIDR()
	// Mutating one must not affect the other (cobra StringSliceVar
	// shares the backing slice across resets).
	a[0] = "mutated"
	if b[0] == "mutated" {
		t.Error("defaultAllowCIDR returned a shared slice; must return a fresh copy")
	}
}

func TestNewServeCmdRegistersFlags(t *testing.T) {
	cmd := newServeCmd()
	for _, name := range []string{"http-port", "http-bind", "http-allow-cidr"} {
		if cmd.Flags().Lookup(name) == nil {
			t.Errorf("flag --%s not registered", name)
		}
	}
}

// --port drove v1's TFTP listener, which was deleted with the bulk plane
// (design §7). It is absent rather than accepted-and-ignored: a flag that
// silently does nothing is worse than one that errors. The space listener
// reintroduces its own port flag when the wire task lands.
func TestServeHasNoStaleTFTPPortFlag(t *testing.T) {
	if f := newServeCmd().Flags().Lookup("port"); f != nil {
		t.Errorf("--port is still registered (%q) but nothing listens on it", f.Usage)
	}
}

// TestAwaitServeExitDrainsOnCtxCancel is the #7 guard: on SIGINT (ctx
// cancel) the serve must wait for every listener goroutine to finish its
// graceful shutdown — draining udpErr, blocksErr and httpErr — before
// returning, rather than racing the deferred conn/store Close.
func TestAwaitServeExitDrainsOnCtxCancel(t *testing.T) {
	udpErr := make(chan error, 1)
	blocksErr := make(chan error, 1)
	sweepErr := make(chan error, 1)
	httpErr := make(chan error, 1)
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // SIGINT analog: ctx.Done fires immediately

	done := make(chan error, 1)
	go func() { done <- awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr) }()

	// Must not return while any listener is still shutting down.
	select {
	case <-done:
		t.Fatal("returned before draining udpErr/blocksErr/httpErr")
	case <-time.After(50 * time.Millisecond):
	}

	// Listeners report clean shutdown; only now may awaitServeExit return.
	udpErr <- nil
	blocksErr <- nil
	sweepErr <- nil
	httpErr <- nil
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("got %v, want nil", err)
		}
	case <-time.After(time.Second):
		t.Fatal("did not return after all channels drained")
	}
}

// TestAwaitServeExitNilHTTPErr covers --http-port 0: httpErr is nil. The
// ctx-cancel drain must not deadlock waiting on a nil channel.
func TestAwaitServeExitNilHTTPErr(t *testing.T) {
	udpErr := make(chan error, 1)
	blocksErr := make(chan error, 1)
	sweepErr := make(chan error, 1)
	var httpErr chan error // nil, like --http-port 0
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	done := make(chan error, 1)
	go func() { done <- awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr) }()
	udpErr <- nil
	blocksErr <- nil
	sweepErr <- nil
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("got %v, want nil", err)
		}
	case <-time.After(time.Second):
		t.Fatal("deadlocked draining a nil httpErr")
	}
}

// TestAwaitServeExitPropagatesListenerErrors confirms a listener failure (no
// ctx cancel) is returned verbatim, and a clean HTTP exit falls through to
// wait on the remaining listeners.
func TestAwaitServeExitPropagatesListenerErrors(t *testing.T) {
	ctx := context.Background()

	// UDP listener fails → its error propagates.
	udpErr := make(chan error, 1)
	blocksErr := make(chan error, 1)
	udpErr <- errSentinel
	if got := awaitServeExit(ctx, udpErr, blocksErr, make(chan error, 1), nil); got != errSentinel {
		t.Errorf("udp error: got %v, want errSentinel", got)
	}

	// Blocks listener fails → its error propagates.
	udpErr = make(chan error, 1)
	blocksErr = make(chan error, 1)
	blocksErr <- errSentinel
	if got := awaitServeExit(ctx, udpErr, blocksErr, make(chan error, 1), nil); got != errSentinel {
		t.Errorf("blocks error: got %v, want errSentinel", got)
	}

	// The sweep fails → its error propagates like any other listener's.
	udpErr = make(chan error, 1)
	blocksErr = make(chan error, 1)
	sweepErr := make(chan error, 1)
	sweepErr <- errSentinel
	if got := awaitServeExit(ctx, udpErr, blocksErr, sweepErr, nil); got != errSentinel {
		t.Errorf("sweep error: got %v, want errSentinel", got)
	}

	// HTTP fails → its error propagates.
	udpErr = make(chan error, 1)
	blocksErr = make(chan error, 1)
	httpErr := make(chan error, 1)
	httpErr <- errSentinel
	if got := awaitServeExit(ctx, udpErr, blocksErr, make(chan error, 1), httpErr); got != errSentinel {
		t.Errorf("http error: got %v, want errSentinel", got)
	}

	// HTTP exits cleanly while UDP still running → fall through to UDP.
	udpErr = make(chan error, 1)
	blocksErr = make(chan error, 1)
	httpErr = make(chan error, 1)
	httpErr <- nil
	udpErr <- errSentinel
	if got := awaitServeExit(ctx, udpErr, blocksErr, make(chan error, 1), httpErr); got != errSentinel {
		t.Errorf("clean http then udp error: got %v, want errSentinel", got)
	}
}

// The injector must be off unless asked for: a gateway that silently drops one
// ack in three is a gateway that manufactures the very bug the flag exists to
// study.
func TestServeRegistersTestDropAckFlagDefaultingToOff(t *testing.T) {
	f := newServeCmd().Flags().Lookup("test-drop-ack")
	if f == nil {
		t.Fatal("--test-drop-ack is not registered")
	}
	if f.DefValue != "0" {
		t.Fatalf("default = %q, want \"0\" (off)", f.DefValue)
	}
}

// sequencedDoneCtx is a context.Context whose Done() answers an OPEN
// (never-closed) channel for its first two calls and a CLOSED channel from
// the third call on. awaitServeExit's inner path calls ctx.Done() exactly
// three times before it can reach the bug this reproduces: once in the
// outer pre-check, once as a case operand in the main select, and once in
// the inner pre-check reached only after the main select has already
// consumed httpErr. Sequencing Done() this way drives that exact
// interleaving deterministically, without a real ctx cancel racing the
// channel sends.
type sequencedDoneCtx struct {
	context.Context
	mu     sync.Mutex
	calls  int
	open   chan struct{} // never closed: Done() "not ready"
	closed chan struct{} // closed at construction: Done() "ready"
}

func newSequencedDoneCtx() *sequencedDoneCtx {
	c := &sequencedDoneCtx{
		Context: context.Background(),
		open:    make(chan struct{}),
		closed:  make(chan struct{}),
	}
	close(c.closed)
	return c
}

func (c *sequencedDoneCtx) Done() <-chan struct{} {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.calls++
	if c.calls <= 2 {
		return c.open
	}
	return c.closed
}

// TestAwaitServeExitDoesNotDeadlockOnAlreadyConsumedHTTPErr is the regression
// guard for the deadlock this fix closes: on the inner ctx-cancel path
// (reached after a clean HTTP exit, httpErr delivering nil, has already been
// consumed by the main select), draining must NOT touch httpErr again — it
// is a cap-1 channel the HTTP goroutine writes to exactly once and never
// closes, so a second receive blocks forever.
//
// We send httpErr's one value and start awaitServeExit BEFORE sending
// anything on udpErr/blocksErr/sweepErr. Until we send those, httpErr is the
// only channel ready (ctx.Done() is "not ready" for its first two calls, per
// sequencedDoneCtx), so the main select deterministically picks the httpErr
// case — no other case can race it. Only once we've confirmed (the 50ms
// window used elsewhere in this file) that awaitServeExit has NOT returned
// early do we send the other three, which moves it into the inner
// ctx.Done()-ready branch (the third Done() call). The buggy code calls
// drain() there, which drains udpErr/blocksErr/sweepErr fine and then blocks
// forever on the already-consumed httpErr — so this test times out and fails
// against the pre-fix code, and returns promptly against the fix.
func TestAwaitServeExitDoesNotDeadlockOnAlreadyConsumedHTTPErr(t *testing.T) {
	udpErr := make(chan error, 1)
	blocksErr := make(chan error, 1)
	sweepErr := make(chan error, 1)
	httpErr := make(chan error, 1)
	httpErr <- nil // clean HTTP exit

	ctx := newSequencedDoneCtx()

	done := make(chan error, 1)
	go func() { done <- awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr) }()

	// Must not return before the other three listeners have anything to
	// report — proves we are past the main select (httpErr consumed) and
	// sitting in the inner branch, not that awaitServeExit short-circuited.
	select {
	case <-done:
		t.Fatal("returned before udpErr/blocksErr/sweepErr were ever sent")
	case <-time.After(50 * time.Millisecond):
	}

	udpErr <- nil
	blocksErr <- nil
	sweepErr <- nil

	select {
	case err := <-done:
		if err != nil {
			t.Errorf("got %v, want nil", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("awaitServeExit deadlocked draining an already-consumed httpErr (regression)")
	}
}

// The sweep is a listener in the same class as udp and blocks: it returns only
// on ctx cancel, and the drain is what keeps an in-flight Sweep from racing the
// store's deferred Close.
func TestAwaitServeExitDrainsTheSweep(t *testing.T) {
	udpErr := make(chan error, 1)
	blocksErr := make(chan error, 1)
	sweepErr := make(chan error, 1)
	httpErr := make(chan error, 1)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	done := make(chan error, 1)
	go func() { done <- awaitServeExit(ctx, udpErr, blocksErr, sweepErr, httpErr) }()

	udpErr <- nil
	blocksErr <- nil
	httpErr <- nil
	select {
	case <-done:
		t.Fatal("returned before draining sweepErr")
	case <-time.After(50 * time.Millisecond):
	}
	sweepErr <- nil
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("got %v, want nil", err)
		}
	case <-time.After(time.Second):
		t.Fatal("did not return after the sweep drained")
	}
}

// A sweep that reports a transition logs it, and one that reports nothing says
// nothing — a line every 30 s per node would bury the transitions.
func TestSweepLivenessLogsOnlyTransitions(t *testing.T) {
	dir := t.TempDir()
	var clock int64 = 10_000
	st, err := space.Open(dir+"/sweep.db", func() int64 { return clock })
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "", "pull")
	st.RecordExchange("n1", 41, 0, 0, 0, 0, "", "push")
	for i := 0; i < 4; i++ {
		clock += 2_000
		st.RecordExchange("n1", 41, 0, 0, 0, 0, "", "pull")
		st.RecordExchange("n1", 41, 0, 0, 0, 0, "", "push")
	}
	clock += 20_000

	var mu sync.Mutex
	var lines []string
	logf := func(format string, args ...any) {
		mu.Lock()
		defer mu.Unlock()
		lines = append(lines, fmt.Sprintf(format, args...))
	}

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- sweepLiveness(ctx, st, time.Millisecond, logf) }()

	deadline := time.After(2 * time.Second)
	for {
		mu.Lock()
		n := len(lines)
		mu.Unlock()
		if n > 0 {
			break
		}
		select {
		case <-deadline:
			cancel()
			t.Fatal("no transition logged within 2s")
		case <-time.After(5 * time.Millisecond):
		}
	}
	// Many ticks have now run; only the ONE transition may have been logged.
	time.Sleep(50 * time.Millisecond)
	cancel()
	if err := <-done; err != nil {
		t.Fatalf("sweepLiveness returned %v, want nil on ctx cancel", err)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(lines) != 1 {
		t.Fatalf("logged %d lines across many ticks, want 1:\n%v", len(lines), lines)
	}
	if !strings.Contains(lines[0], "n1") || !strings.Contains(lines[0], "silent") {
		t.Fatalf("line = %q, want it to name the node and the verdict", lines[0])
	}
}
