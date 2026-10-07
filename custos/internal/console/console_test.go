// Copyright (c) 2026 Ekorau LLC

package console

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"
)

// stampRE is the deleted reader's line pattern (logread.py's LINE_RE): the
// files must still parse by it, since the harness reads them the same way.
var stampRE = regexp.MustCompile(`^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}[+-]\d\d:\d\d) (.*)$`)

func TestAppendWritesTheCollectorsFormat(t *testing.T) {
	s := &Store{Root: t.TempDir(), Board: "c6"}
	when := time.Date(2026, 8, 3, 20, 19, 38, 87e6, time.FixedZone("MST", -7*3600))
	if err := s.Append("ESP-ROM:esp32c6-20220919", when); err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(s.Path(when.Local()))
	if err != nil {
		t.Fatal(err)
	}
	want := Stamp("ESP-ROM:esp32c6-20220919", when.Local())
	if string(b) != want || !stampRE.MatchString(strings.TrimSuffix(string(b), "\n")) {
		t.Fatalf("%q, want %q", b, want)
	}
	// The fixture's own first line, as the Python collector wrote it.
	if got := Stamp("ESP-ROM:esp32c6-20220919", when); got != "2026-08-03T20:19:38.087-07:00 ESP-ROM:esp32c6-20220919\n" {
		t.Fatalf("%q", got)
	}
	if filepath.Base(s.Path(when)) != "console-c6-2026-08-03.log" {
		t.Fatal(s.Path(when))
	}
}

func TestPruneKeepsTheNewestDay(t *testing.T) {
	s := &Store{Root: t.TempDir(), Board: "c6", Budget: 10}
	for _, d := range []int{1, 2, 3} {
		s.Append(strings.Repeat("x", 20), time.Date(2026, 10, d, 12, 0, 0, 0, time.Local))
	}
	os.WriteFile(filepath.Join(s.Root, "console-c6-notadate.log"), []byte("not ours"), 0o644)
	deleted, err := s.Prune()
	if err != nil || len(deleted) != 2 {
		t.Fatalf("%v %v", deleted, err)
	}
	days, _ := s.Days()
	if len(days) != 1 || days[0].Day() != 3 {
		t.Fatalf("%v", days)
	}
	if _, err := os.Stat(filepath.Join(s.Root, "console-c6-notadate.log")); err != nil {
		t.Fatal("a file that is not the store's was touched")
	}
}

func TestLockIsOneReader(t *testing.T) {
	root := t.TempDir()
	f, err := Lock(root, "c6")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if _, err := Lock(root, "c6"); err == nil || !strings.Contains(err.Error(), "held (pid=") {
		t.Fatalf("a second reader: %v", err)
	}
	if g, err := Lock(root, "c6spare"); err != nil {
		t.Fatalf("another board: %v", err)
	} else {
		g.Close()
	}
}

// fakePort is one open of a cable: the test writes what the board says into w.
type fakePort struct {
	r      *io.PipeReader
	w      *io.PipeWriter
	mu     sync.Mutex
	sent   []string
	closed bool
}

func newFakePort() *fakePort { r, w := io.Pipe(); return &fakePort{r: r, w: w} }

func (p *fakePort) Read(b []byte) (int, error) { return p.r.Read(b) }
func (p *fakePort) Write(b []byte) (int, error) {
	p.mu.Lock()
	p.sent = append(p.sent, string(b))
	p.mu.Unlock()
	return len(b), nil
}
func (p *fakePort) Close() error {
	p.mu.Lock()
	p.closed = true
	p.mu.Unlock()
	p.r.Close()
	return nil
}

// bench runs a Cable whose opens come from ports, in order, with failures
// where ports holds nil.
type bench struct {
	c      *Cable
	store  *Store
	mu     sync.Mutex
	ports  []*fakePort
	opened chan *fakePort
	cancel context.CancelFunc
	done   chan struct{}
}

func newBench(t *testing.T, ports ...*fakePort) *bench {
	b := &bench{store: &Store{Root: t.TempDir(), Board: "c6"}, ports: ports, opened: make(chan *fakePort, 16), done: make(chan struct{})}
	b.c = &Cable{Store: b.store, Port: "/dev/fake", Heartbeat: time.Hour, Open: func() (io.ReadWriteCloser, error) {
		b.mu.Lock()
		defer b.mu.Unlock()
		if len(b.ports) == 0 {
			return nil, errors.New("no such device")
		}
		p := b.ports[0]
		b.ports = b.ports[1:]
		if p == nil {
			return nil, errors.New("no such device")
		}
		b.opened <- p
		return p, nil
	}}
	ctx, cancel := context.WithCancel(context.Background())
	b.cancel = cancel
	go func() { b.c.Run(ctx); close(b.done) }()
	t.Cleanup(b.stop)
	return b
}

func (b *bench) stop() {
	b.cancel()
	<-b.done
}

func (b *bench) log(t *testing.T) []string {
	t.Helper()
	days, _ := b.store.Days()
	var out []string
	for _, d := range days {
		data, _ := os.ReadFile(b.store.Path(d))
		for _, l := range strings.Split(strings.TrimSpace(string(data)), "\n") {
			m := stampRE.FindStringSubmatch(l)
			if m == nil {
				t.Fatalf("unstamped line %q", l)
			}
			out = append(out, m[2])
		}
	}
	return out
}

func waitFor(t *testing.T, what string, ok func() bool) {
	t.Helper()
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(5 * time.Millisecond) {
		if ok() {
			return
		}
	}
	t.Fatalf("timed out waiting for %s", what)
}

func TestTheCableStampsEveryLineAndHandsItOn(t *testing.T) {
	p := newFakePort()
	b := newBench(t, p)
	var mu sync.Mutex
	var got []string
	cancel := b.c.Subscribe(func(l string) { mu.Lock(); got = append(got, l); mu.Unlock() })
	<-b.opened
	p.w.Write([]byte("oevm: build-id B1\r\nhel\x00lo\n"))
	waitFor(t, "two lines", func() bool { mu.Lock(); defer mu.Unlock(); return len(got) == 2 })
	cancel()
	p.w.Write([]byte("after\n"))
	waitFor(t, "the third line in the log", func() bool { l := b.log(t); return l[len(l)-1] == "after" })
	if strings.Join(got, "|") != "oevm: build-id B1|hello" {
		t.Fatalf("subscriber %q", got)
	}
	if err := b.c.Send("dbg:pause"); err != nil || strings.Join(p.sent, "") != "dbg:pause\n" {
		t.Fatalf("send: %v %q", err, p.sent)
	}
	b.stop()
	l := b.log(t)
	if !strings.HasPrefix(l[0], Marker+" collector started pid=") || l[1] != "oevm: build-id B1" || l[2] != "hello" ||
		l[3] != "after" || l[len(l)-1] != Marker+" collector stopped" {
		t.Fatalf("%q", l)
	}
}

func TestALostPortIsNotedOnceAndReacquired(t *testing.T) {
	p := newFakePort()
	b := newBench(t, nil, nil, p) // two failed opens, then the board is back
	<-b.opened
	waitFor(t, "reacquired", func() bool {
		l := b.log(t)
		return len(l) > 0 && l[len(l)-1] == Marker+" port reacquired"
	})
	lost := 0
	for _, l := range b.log(t) {
		if strings.HasPrefix(l, Marker+" port lost") {
			lost++
		}
	}
	if lost != 1 {
		t.Fatalf("port lost noted %d times: %q", lost, b.log(t))
	}
	if !b.c.Up() {
		t.Fatal("not up after reacquiring")
	}
}

func TestLendClosesThePortAndTakesItBack(t *testing.T) {
	first, second := newFakePort(), newFakePort()
	b := newBench(t, first, second)
	<-b.opened
	ran := false
	err := b.c.Lend(func() error {
		if !first.closed || b.c.Up() {
			t.Error("the port was still open while lent")
		}
		ran = true
		return nil
	})
	if err != nil || !ran {
		t.Fatalf("%v %v", err, ran)
	}
	<-b.opened
	waitFor(t, "up again", b.c.Up)
	second.w.Write([]byte("back\n"))
	waitFor(t, "a line after the lend", func() bool { l := b.log(t); return l[len(l)-1] == "back" })
	for _, l := range b.log(t) {
		if strings.Contains(l, "port lost") {
			t.Fatalf("a lend was logged as a loss: %q", b.log(t))
		}
	}
	l := strings.Join(b.log(t), "|")
	if !strings.Contains(l, Marker+" cable lent|"+Marker+" cable back") {
		t.Fatalf("%q", l)
	}
}

func TestTheHeartbeatSaysWhetherThePortIsUp(t *testing.T) {
	store := &Store{Root: t.TempDir(), Board: "c6"}
	c := &Cable{Store: store, Heartbeat: 20 * time.Millisecond, Open: func() (io.ReadWriteCloser, error) {
		return nil, errors.New("no such device")
	}}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { c.Run(ctx); close(done) }()
	time.Sleep(80 * time.Millisecond)
	cancel()
	<-done
	data, _ := os.ReadFile(store.Path(time.Now()))
	if !strings.Contains(string(data), Marker+" heartbeat port=down") {
		t.Fatalf("%s", data)
	}
}

// The health is answered from memory: up with the board's last line, down with
// why, lent while a load holds the cable.
func TestHealthIsTheCablesStateFromMemory(t *testing.T) {
	first, second := newFakePort(), newFakePort()
	b := newBench(t, nil, first, second)
	<-b.opened
	waitFor(t, "up", b.c.Up)
	if h := b.c.Health(); h.State != "up" || !h.LastLine.IsZero() || h.Since.IsZero() {
		t.Fatalf("%+v", h)
	}
	first.w.Write([]byte("hello\n"))
	waitFor(t, "the last line", func() bool { return !b.c.Health().LastLine.IsZero() })
	b.c.Lend(func() error {
		if h := b.c.Health(); h.State != "lent" {
			t.Errorf("while lent: %+v", h)
		}
		return nil
	})
	<-b.opened
	waitFor(t, "up again", b.c.Up)
	second.w.Close() // the board goes away, and no port follows
	waitFor(t, "down", func() bool { return b.c.Health().State == "down" })
	if h := b.c.Health(); h.Reason == "" {
		t.Fatalf("down without a reason: %+v", h)
	}
}

// A collector prunes when it starts, and the newest day is never pruned.
func TestTheCollectorPrunesAndSaysSo(t *testing.T) {
	store := &Store{Root: t.TempDir(), Board: "c6", Budget: 1}
	old := time.Now().AddDate(0, 0, -3)
	store.Append("an old day", old)
	c := &Cable{Store: store, Heartbeat: time.Hour, Open: func() (io.ReadWriteCloser, error) {
		return nil, errors.New("no such device")
	}}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { c.Run(ctx); close(done) }()
	waitFor(t, "the old day pruned", func() bool { _, err := os.Stat(store.Path(old)); return os.IsNotExist(err) })
	cancel()
	<-done
	data, _ := os.ReadFile(store.Path(time.Now()))
	if !strings.Contains(string(data), Marker+" pruned "+store.Path(old)) {
		t.Fatalf("%s", data)
	}
}
