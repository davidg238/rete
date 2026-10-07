// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/davidg238/rete/custos/internal/console"
)

// cableBoard serves one collected board, c6, whose cable is a fresh pipe at
// every open; it answers the server and a writer to the board's current port.
func cableBoard(t *testing.T) (*httptest.Server, func() *io.PipeWriter) {
	t.Helper()
	ports := make(chan *pipePort, 16)
	openCable = func(string) func() (io.ReadWriteCloser, error) {
		return func() (io.ReadWriteCloser, error) {
			r, w := io.Pipe()
			p := &pipePort{r, w, make(chan string, 16)}
			ports <- p
			return p, nil
		}
	}
	t.Cleanup(func() { openCable = openSerial })
	ctx, cancel := context.WithCancel(context.Background())
	root := t.TempDir()
	h, err := Handler(Config{Boards: []Board{{Name: "c6", Port: "/dev/fake"}}, ConsoleRoot: root, Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	t.Cleanup(func() { srv.Close(); cancel(); time.Sleep(50 * time.Millisecond) })
	var cur *pipePort
	return srv, func() *io.PipeWriter {
		for {
			select {
			case p := <-ports:
				cur = p
			default:
				if cur != nil {
					return cur.w
				}
				time.Sleep(5 * time.Millisecond)
			}
		}
	}
}

func getJSON(t *testing.T, url string, v any) int {
	t.Helper()
	r, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Body.Close()
	if err := json.NewDecoder(r.Body).Decode(v); err != nil {
		t.Fatal(err)
	}
	return r.StatusCode
}

// D16: the console's reader is custos's, over HTTP — the lines, the runs, the
// panics, and the cable's health.
func TestACollectedBoardAnswersItsConsole(t *testing.T) {
	srv, board := cableBoard(t)
	base := srv.URL + "/board/c6"
	board().Write([]byte("rst:0xc (SW_CPU)\n*** Booting Zephyr OS build v4.2.0 ***\noevm: build-id B1\n" +
		"E: >>> ZEPHYR FATAL ERROR 0: CPU exception on CPU 0\nE: #CD:BEGIN#\nE: #CD:5a4501\nE: #CD:END#\n" +
		"*** Booting Zephyr OS build v4.2.0 ***\noevm: build-id B2\nhello\n"))
	var lines []console.Entry
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		if getJSON(t, base+"/log?tail=2", &lines); len(lines) == 2 && lines[1].Text == "hello" {
			break
		}
	}
	if len(lines) != 2 || lines[0].Text != "oevm: build-id B2" || lines[1].Text != "hello" {
		t.Fatalf("tail: %+v", lines)
	}
	getJSON(t, base+"/log?since=-1m", &lines)
	if len(lines) < 10 || !strings.HasPrefix(lines[0].Text, console.Marker+" collector started") {
		t.Fatalf("since: %+v", lines)
	}
	var runs []RunRow
	getJSON(t, base+"/runs", &runs)
	if len(runs) != 2 || runs[0].BuildID != "B1" || runs[1].BuildID != "B2" {
		t.Fatalf("runs: %+v", runs)
	}
	var e map[string]string
	var ps []PanicRow
	getJSON(t, base+"/panics", &ps)
	if len(ps) != 1 || ps[0].Index != 1 || ps[0].Run != 1 || ps[0].BuildID != "B1" || ps[0].Coredump != 1 {
		t.Fatalf("panics: %+v", ps)
	}
	t.Setenv("RETE_ELF_ROOT", t.TempDir())
	if code := getJSON(t, base+"/panics/1/bt", &e); code != 422 || !strings.Contains(e["Error"], "no archived ELF for build id B1") {
		t.Fatalf("bt without the ELF: %d %v", code, e)
	}
	if code := getJSON(t, base+"/panics/2/bt", &e); code != 404 {
		t.Fatalf("bt of a panic that is not there: %d %v", code, e)
	}
	var h console.Health
	getJSON(t, base+"/health", &h)
	if h.State != "up" || h.LastLine.IsZero() {
		t.Fatalf("health: %+v", h)
	}
	if code := getJSON(t, base+"/log?since=yesterday", &e); code != 400 || e["Error"] == "" {
		t.Fatalf("a bad since: %d %v", code, e)
	}
}

// D16 (David, 2026-10-06): a flash borrows the cable with /lend, and custos
// takes it back on /return or when the time runs out, whichever comes first.
func TestLendFreesTheCableUntilReturnOrTheTimeRunsOut(t *testing.T) {
	srv, board := cableBoard(t)
	base := srv.URL + "/board/c6"
	board()
	if code, body := post(t, base+"/lend", nil); code != 400 {
		t.Fatalf("a lend without s: %d %s", code, body)
	}
	if code, body := post(t, base+"/lend?s=60", nil); code != 200 || !strings.Contains(body, `"State":"lent"`) {
		t.Fatalf("lend: %d %s", code, body)
	}
	if code, body := post(t, base+"/lend?s=60", nil); code != 409 {
		t.Fatalf("a second lend: %d %s", code, body)
	}
	if code, body := post(t, base+"/return", nil); code != 200 || strings.Contains(body, `"State":"lent"`) {
		t.Fatalf("return: %d %s", code, body)
	}
	if code, _ := post(t, base+"/return", nil); code != 409 {
		t.Fatalf("a return with nothing lent: %d", code)
	}
	var h console.Health
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		if getJSON(t, base+"/health", &h); h.State == "up" {
			break
		}
	}
	if h.State != "up" {
		t.Fatalf("not up after the return: %+v", h)
	}
	post(t, base+"/lend?s=1", nil)
	time.Sleep(1200 * time.Millisecond)
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		if getJSON(t, base+"/health", &h); h.State == "up" {
			return
		}
	}
	t.Fatalf("not taken back when the time ran out: %+v", h)
}

// A board that logs over the mesh is collected from its UDP port, and serves
// its console verbs only.
func TestAUDPBoardServesItsConsole(t *testing.T) {
	l, err := net.ListenPacket("udp", "[::1]:0")
	if err != nil {
		t.Fatal(err)
	}
	port := l.LocalAddr().(*net.UDPAddr).Port
	l.Close()
	ctx, cancel := context.WithCancel(context.Background())
	h, err := Handler(Config{Boards: []Board{{Name: "bell", UDP: port}, {Name: "dev", Host: "/bin/true"}}, ConsoleRoot: t.TempDir(), Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	t.Cleanup(func() { srv.Close(); cancel(); time.Sleep(50 * time.Millisecond) })
	to, err := net.Dial("udp", net.JoinHostPort("::1", strconv.Itoa(port)))
	if err != nil {
		t.Fatal(err)
	}
	var lines []console.Entry
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(20 * time.Millisecond) {
		to.Write([]byte("ding"))
		if getJSON(t, srv.URL+"/board/bell/log?tail=1", &lines); len(lines) == 1 && lines[0].Text == "ding" {
			break
		}
	}
	if len(lines) != 1 || lines[0].Text != "ding" {
		t.Fatalf("%+v", lines)
	}
	var infos []BoardInfo
	getJSON(t, srv.URL+"/boards", &infos)
	if infos[0].Target != "udp" || strings.Join(infos[0].Verbs, ",") != "log,runs,panics,health" {
		t.Fatalf("%+v", infos[0])
	}
	if code, _ := post(t, srv.URL+"/board/bell/lend?s=5", nil); code != 404 && code != 405 {
		t.Fatalf("a UDP board lent its cable: %d", code)
	}
}
