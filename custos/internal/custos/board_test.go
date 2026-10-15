// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestParseBoardsJoinsTheFlags(t *testing.T) {
	bs, err := ParseBoards([]string{"c6=/dev/ttyA"}, []string{"dev=/tmp/oevm_img -x"}, nil, nil, []string{"dev=/a.oimg.map.json"}, []string{"dev=/a.nsl"})
	if err != nil {
		t.Fatal(err)
	}
	if len(bs) != 2 || bs[0].Name != "c6" || bs[0].Port != "/dev/ttyA" || bs[1].Name != "dev" ||
		bs[1].Host != "/tmp/oevm_img -x" || bs[1].Map != "/a.oimg.map.json" || bs[1].Source != "/a.nsl" {
		t.Fatalf("%+v", bs)
	}
}

// A bare --board reads its cable from the bench file, as the old collector unit did.
func TestABareBoardReadsItsBenchFile(t *testing.T) {
	old := BenchDir
	BenchDir = t.TempDir()
	defer func() { BenchDir = old }()
	os.WriteFile(filepath.Join(BenchDir, "console-c6spare.env"), []byte("# the spare\nRETE_CONSOLE_PORT=/dev/serial/by-id/x\n"), 0o644)
	os.WriteFile(filepath.Join(BenchDir, "console-empty.env"), []byte("# nothing\n"), 0o644)
	bs, err := ParseBoards([]string{"c6spare"}, nil, nil, nil, nil, nil)
	if err != nil || len(bs) != 1 || bs[0].Port != "/dev/serial/by-id/x" {
		t.Fatalf("%+v %v", bs, err)
	}
	for name, want := range map[string]string{"nope": "no bench file", "empty": "no RETE_CONSOLE_PORT="} {
		if _, err := ParseBoards([]string{name}, nil, nil, nil, nil, nil); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: %v, want %q", name, err, want)
		}
	}
}

func TestParseBoardsRefuses(t *testing.T) {
	cases := map[string][6][]string{
		"<name>=<value>":          {{"/dev/ttyA"}, nil, nil, nil, nil, nil},
		"a-z, 0-9":                {{"C6=/dev/ttyA"}, nil, nil, nil, nil, nil},
		"named twice":             {{"a=/dev/ttyA"}, {"a=/bin/true"}, nil, nil, nil, nil},
		"no --board":              {{"a=/dev/ttyA"}, nil, nil, nil, {"b=/x.map.json"}, nil},
		"one cable":               {{"a=/dev/ttyA", "b=/dev/ttyA"}, nil, nil, nil, nil, nil},
		"<name>=<udp port>":       {nil, nil, {"c6=x"}, nil, nil, nil},
		"serves its console only": {nil, nil, {"c6=54330"}, nil, {"c6=/x.map.json"}, nil},
		"has no debug page":       {nil, nil, nil, {"bell=206ef1195438"}, {"bell=/x.map.json"}, {"bell=/x.nsl"}},
		"bell is named twice":     {nil, {"bell=/bin/true"}, nil, {"bell=206ef1195438"}, nil, nil},
		"twice":                   {{"a=/dev/ttyA"}, nil, {"a=54330"}, nil, nil, nil},
	}
	for want, c := range cases {
		if _, err := ParseBoards(c[0], c[1], c[2], c[3], c[4], c[5]); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%v: %v, want an error naming %q", c, err, want)
		}
	}
}

func get(t *testing.T, url string) (int, string) {
	t.Helper()
	r, err := http.Get(url)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Body.Close()
	b, _ := io.ReadAll(r.Body)
	return r.StatusCode, string(b)
}

func serveBoards(t *testing.T, bs ...Board) *httptest.Server {
	t.Helper()
	h, err := Handler(Config{Boards: bs, ConsoleRoot: t.TempDir()})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	t.Cleanup(srv.Close)
	return srv
}

// A host board runs the image beside its map; with one board the board-less
// paths are its shorthand, and /boards names what it serves.
func TestOneHostBoardServesEveryVerbTwice(t *testing.T) {
	srv := serveBoards(t, Board{Name: "dev", Host: "/bin/true", Map: fixtureMap(t)})
	for _, p := range []string{"/board/dev/debug", "/board/dev/profile", "/board/dev/load", "/debug", "/profile", "/load", "/fleet.css"} {
		if code, _ := get(t, srv.URL+p); code != 200 {
			t.Errorf("%s: %d", p, code)
		}
	}
	_, body := get(t, srv.URL+"/boards")
	var infos []BoardInfo
	if err := json.Unmarshal([]byte(body), &infos); err != nil {
		t.Fatal(err)
	}
	if len(infos) != 1 || infos[0].Name != "dev" || infos[0].Target != "host" || strings.Join(infos[0].Verbs, ",") != "debug,profile,load" {
		t.Fatalf("%s", body)
	}
}

// Two boards: each under its name, and no shorthand, since it could mean either.
func TestTwoBoardsHaveNoShorthand(t *testing.T) {
	srv := serveBoards(t, Board{Name: "a", Host: "/bin/true", Map: fixtureMap(t)}, Board{Name: "b", Host: "/bin/true"})
	for p, want := range map[string]int{"/board/a/debug": 200, "/board/a/load": 200, "/board/b/load": 200, "/board/b/debug": 404, "/load": 404, "/debug": 404} {
		if code, _ := get(t, srv.URL+p); code != want {
			t.Errorf("%s: %d, want %d", p, code, want)
		}
	}
}

// A board with no map serves load only, and opens no cable at start-up.
func TestAPortBoardWithoutAMapServesLoadOnly(t *testing.T) {
	srv := serveBoards(t, Board{Name: "c6", Port: "/dev/nonexistent-tty"})
	if code, _ := get(t, srv.URL+"/board/c6/load"); code != 200 {
		t.Fatalf("load: %d", code)
	}
	if code, _ := get(t, srv.URL+"/board/c6/debug"); code != 404 {
		t.Fatalf("debug: %d", code)
	}
}

func TestAHostBoardIsCheckedAtStartUp(t *testing.T) {
	m := fixtureMap(t)
	if _, err := Handler(Config{Boards: []Board{{Name: "dev", Host: "/nonexistent/oevm_img", Map: m}}}); err == nil || !strings.Contains(err.Error(), "board dev") {
		t.Errorf("a missing VM: %v", err)
	}
	// A map whose image beside it is another build.
	dir := t.TempDir()
	b, _ := os.ReadFile(m)
	other := filepath.Join(dir, "Fixture.oimg.map.json")
	os.WriteFile(other, b, 0o644)
	os.WriteFile(filepath.Join(dir, "Fixture.oimg"), []byte("not the image the map describes"), 0o644)
	if _, err := Handler(Config{Boards: []Board{{Name: "dev", Host: "/bin/true", Map: other}}}); err == nil {
		t.Errorf("an image the map does not describe was accepted")
	}
}

// A host program that ends before its window closes answers 502 at once.
func TestAProfileOnAHostThatEndsEarlyAnswers502(t *testing.T) {
	srv := serveBoards(t, Board{Name: "dev", Host: "/bin/true", Map: fixtureMap(t)})
	r, err := http.Post(srv.URL+"/board/dev/profile/capture?ms=50", "", nil)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := io.ReadAll(r.Body)
	if r.StatusCode != http.StatusBadGateway || !strings.Contains(string(b), "program ended before the window closed") {
		t.Fatalf("%d %s", r.StatusCode, b)
	}
}

// pipePort is a board's cable in a test: the board's output is written to w.
type pipePort struct {
	r    *io.PipeReader
	w    *io.PipeWriter
	sent chan string
}

func (p *pipePort) Read(b []byte) (int, error)  { return p.r.Read(b) }
func (p *pipePort) Write(b []byte) (int, error) { p.sent <- string(b); return len(b), nil }
func (p *pipePort) Close() error                { return p.r.Close() }

// D16: a board on a cable is collected — every line to its day log — and its
// debug session and a capture are subscribers of the one reader.
func TestABoardOnACableIsCollectedAndItsVerbsSubscribe(t *testing.T) {
	r, w := io.Pipe()
	port := &pipePort{r, w, make(chan string, 16)}
	openCable = func(string) func() (io.ReadWriteCloser, error) {
		return func() (io.ReadWriteCloser, error) { return port, nil }
	}
	t.Cleanup(func() { openCable = openSerial })
	root := t.TempDir()
	ctx, cancel := context.WithCancel(context.Background())
	day := filepath.Join(root, "console-c6-"+time.Now().Format("2006-01-02")+".log")
	t.Cleanup(func() { // the collector's last line, before the temp dir goes
		cancel()
		for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
			if data, _ := os.ReadFile(day); strings.Contains(string(data), "collector stopped") {
				return
			}
		}
		t.Error("the collector never wrote that it stopped")
	})
	h, err := Handler(Config{Boards: []Board{{Name: "c6", Port: "/dev/fake", Map: fixtureMap(t)}}, ConsoleRoot: root, Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	srv := httptest.NewServer(h)
	defer srv.Close()
	if _, err := Handler(Config{Boards: []Board{{Name: "c6", Port: "/dev/fake2"}}, ConsoleRoot: root}); err == nil || !strings.Contains(err.Error(), "held") {
		t.Fatalf("a second collector of c6: %v", err)
	}

	w.Write([]byte("oevm: build-id B1\ndbg:ready\n"))
	waitState(t, srv.URL+"/board/c6/debug/state", "ready")

	// A capture subscribes, sends its request down the cable, and reads the window.
	done := make(chan int)
	go func() {
		resp, err := http.Post(srv.URL+"/board/c6/profile/capture?ms=10", "", nil)
		if err != nil {
			done <- 0
			return
		}
		done <- resp.StatusCode
	}()
	if got := <-port.sent; got != "profile 10\n" {
		t.Fatalf("sent %q", got)
	}
	w.Write([]byte("mprof[window]: methods=1 activations=5 overflow=0 cap=256\nmprof: 4096 5\nmprof[window]: end\n"))
	if code := <-done; code != 200 {
		t.Fatalf("capture: %d", code)
	}

	data, _ := os.ReadFile(day)
	for _, want := range []string{"@@rete-console@@ collector started", " oevm: build-id B1\n", " dbg:ready\n", " mprof: 4096 5\n"} {
		if !strings.Contains(string(data), want) {
			t.Errorf("the day log lacks %q:\n%s", want, data)
		}
	}
}

func waitState(t *testing.T, url, run string) {
	t.Helper()
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		_, body := get(t, url)
		if strings.Contains(body, `"Run":"`+run+`"`) {
			return
		}
	}
	t.Fatalf("%s never reached %s", url, run)
}
