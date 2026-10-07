// Copyright (c) 2026 Ekorau LLC

package custos

// board.go — the boards custos serves (peer design D15, rete#314): each one a
// board on its console cable or a host VM, named, with every verb under
// /board/<name>/. With one board, the board-less paths are its shorthand.
//
//	GET /boards   every board, as JSON: its name, its target, and the verbs it serves

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os/exec"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/profile"
)

// Board is one target. Exactly one of Port and Host.
type Board struct {
	Name string
	// Port is a board's console cable, held from start-up when the board has a
	// debug page, and opened for each load or capture otherwise.
	Port string
	// Host is a host VM binary (with any flags, split on white space). It runs
	// the image beside Map for debug and profile, and each uploaded image once
	// for load (D14).
	Host string
	// Map is the program's <name>.oimg.map.json: without it the board serves
	// load only. Source is its .nsl ("" = beside the map).
	Map    string
	Source string
}

// BoardInfo is one row of GET /boards.
type BoardInfo struct {
	Name   string
	Target string // "port" or "host"
	Port   string `json:",omitempty"`
	Host   string `json:",omitempty"`
	Map    string `json:",omitempty"`
	Verbs  []string
}

var boardName = regexp.MustCompile(`^[a-z0-9][a-z0-9-]*$`)

// ParseBoards joins the repeated --board, --host, --map and --source flags
// (each name=value) into boards, sorted by name, or says what is wrong.
func ParseBoards(ports, hosts, maps, sources []string) ([]Board, error) {
	byName := map[string]*Board{}
	split := func(flag, kv string) (string, string, error) {
		name, v, ok := strings.Cut(kv, "=")
		if !ok || v == "" || !boardName.MatchString(name) {
			return "", "", fmt.Errorf("custos: --%s takes <name>=<value>, a name of a-z, 0-9 and -: %q", flag, kv)
		}
		return name, v, nil
	}
	for _, t := range []struct {
		flag string
		kvs  []string
	}{{"board", ports}, {"host", hosts}} {
		for _, kv := range t.kvs {
			name, v, err := split(t.flag, kv)
			if err != nil {
				return nil, err
			}
			if byName[name] != nil {
				return nil, fmt.Errorf("custos: board %s is named twice", name)
			}
			b := &Board{Name: name}
			if t.flag == "board" {
				b.Port = v
			} else {
				b.Host = v
			}
			byName[name] = b
		}
	}
	for _, t := range []struct {
		flag string
		kvs  []string
		set  func(*Board, string)
	}{{"map", maps, func(b *Board, v string) { b.Map = v }}, {"source", sources, func(b *Board, v string) { b.Source = v }}} {
		for _, kv := range t.kvs {
			name, v, err := split(t.flag, kv)
			if err != nil {
				return nil, err
			}
			b := byName[name]
			if b == nil {
				return nil, fmt.Errorf("custos: --%s names board %s, which no --board or --host gives", t.flag, name)
			}
			t.set(b, v)
		}
	}
	seen := map[string]string{}
	var out []Board
	for _, b := range byName {
		if b.Port != "" {
			if other, ok := seen[b.Port]; ok {
				return nil, fmt.Errorf("custos: boards %s and %s are one cable, and a cable has one holder", other, b.Name)
			}
			seen[b.Port] = b.Name
		}
		out = append(out, *b)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// mountBoards serves every board under /board/<name>, and the only one also
// at the board-less paths.
func mountBoards(mux *http.ServeMux, boards []Board, esptool string) error {
	var infos []BoardInfo
	for _, b := range boards {
		prefixes := []string{"/board/" + b.Name}
		if len(boards) == 1 {
			prefixes = append(prefixes, "")
		}
		verbs, err := mountBoard(mux, prefixes, b, esptool)
		if err != nil {
			return fmt.Errorf("custos: board %s: %w", b.Name, err)
		}
		info := BoardInfo{Name: b.Name, Target: "port", Port: b.Port, Host: b.Host, Map: b.Map, Verbs: verbs}
		if b.Host != "" {
			info.Target = "host"
		}
		infos = append(infos, info)
	}
	mux.HandleFunc("GET /boards", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(infos)
	})
	return nil
}

// mountBoard mounts one board's verbs under each prefix and answers their names.
// The verbs on one board take turns through one lock; a held debug cable is
// lent to a load (which restarts the session on the new program) and to a
// capture (which resumes it).
func mountBoard(mux *http.ServeMux, prefixes []string, b Board, esptool string) ([]string, error) {
	busy := &sync.Mutex{}
	var verbs []string
	var held *cable
	if b.Map != "" {
		sm, err := debug.LoadSourceMap(b.Map, b.Source)
		if err != nil {
			return nil, err
		}
		s := debug.NewSession(sm, io.Discard)
		s.AutoFetch = true
		var line debug.Line
		if b.Port != "" {
			open := heldCable(b.Port, s)
			if line, err = open(); err != nil {
				return nil, err
			}
			held = newDebugCable(s, line, open, func() (*debug.SourceMap, error) {
				return debug.LoadSourceMap(b.Map, b.Source)
			})
		} else {
			argv := append(strings.Fields(b.Host), strings.TrimSuffix(b.Map, ".map.json"))
			if err := checkHost(argv, b.Map); err != nil {
				return nil, err
			}
			if line, err = debug.StartHost(argv, true, s.OnLine, s.OnEnd); err != nil {
				return nil, err
			}
		}
		s.Attach(line)
		mountDebug(mux, prefixes, s, busy)

		open := func(onLine func(string), onEnd func()) (profile.Sender, error) {
			return debug.OpenPort(b.Port, 115200, onLine, onEnd)
		}
		if b.Host != "" {
			argv := append(strings.Fields(b.Host), strings.TrimSuffix(b.Map, ".map.json"))
			open = func(onLine func(string), onEnd func()) (profile.Sender, error) {
				return debug.StartHost(argv, false, onLine, onEnd)
			}
		}
		mountProfile(mux, prefixes, b.Map, open, held, busy)
		verbs = append(verbs, "debug", "profile")
	}

	var t load.Target
	if b.Host != "" {
		argv := strings.Fields(b.Host)
		if len(argv) == 0 {
			return nil, fmt.Errorf("--host: an empty command line")
		}
		if _, err := exec.LookPath(argv[0]); err != nil {
			return nil, err
		}
		t = load.Host{Argv: argv, Timeout: 15 * time.Second}
	} else {
		t = load.Loader{
			Region:  load.ESP32C6,
			Esptool: esptool,
			Port:    b.Port,
			Board:   load.SerialBoard{Port: b.Port, Baud: 115200, Timeout: 15 * time.Second},
			Flash: func(argv []string) error {
				out, err := exec.Command(argv[0], argv[1:]...).CombinedOutput()
				if err != nil {
					return fmt.Errorf("%w: %s", err, lastLines(string(out), 3))
				}
				return nil
			},
		}
	}
	if err := mountLoad(mux, prefixes, t, held, busy); err != nil {
		return nil, err
	}
	return append(verbs, "load"), nil
}

// handle registers h at pattern ("METHOD /path") under each prefix.
func handle(mux *http.ServeMux, prefixes []string, pattern string, h http.HandlerFunc) {
	method, path, _ := strings.Cut(pattern, " ")
	for _, p := range prefixes {
		mux.HandleFunc(method+" "+p+path, h)
	}
}
