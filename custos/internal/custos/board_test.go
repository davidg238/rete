// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseBoardsJoinsTheFlags(t *testing.T) {
	bs, err := ParseBoards([]string{"c6=/dev/ttyA"}, []string{"dev=/tmp/oevm_img -x"}, []string{"dev=/a.oimg.map.json"}, []string{"dev=/a.nsl"})
	if err != nil {
		t.Fatal(err)
	}
	if len(bs) != 2 || bs[0].Name != "c6" || bs[0].Port != "/dev/ttyA" || bs[1].Name != "dev" ||
		bs[1].Host != "/tmp/oevm_img -x" || bs[1].Map != "/a.oimg.map.json" || bs[1].Source != "/a.nsl" {
		t.Fatalf("%+v", bs)
	}
}

func TestParseBoardsRefuses(t *testing.T) {
	cases := map[string][4][]string{
		"<name>=<value>": {{"/dev/ttyA"}, nil, nil, nil},
		"a-z, 0-9":       {{"C6=/dev/ttyA"}, nil, nil, nil},
		"named twice":    {{"a=/dev/ttyA"}, {"a=/bin/true"}, nil, nil},
		"no --board":     {{"a=/dev/ttyA"}, nil, {"b=/x.map.json"}, nil},
		"one cable":      {{"a=/dev/ttyA", "b=/dev/ttyA"}, nil, nil, nil},
	}
	for want, c := range cases {
		if _, err := ParseBoards(c[0], c[1], c[2], c[3]); err == nil || !strings.Contains(err.Error(), want) {
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
	h, err := Handler(Config{Boards: bs})
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
