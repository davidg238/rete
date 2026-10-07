// Copyright (c) 2026 Ekorau LLC

package custos

// board.go — the boards custos serves (peer design D15, rete#314): each one a
// board on its console cable or a host VM, named, with every verb under
// /board/<name>/. With one board, the board-less paths are its shorthand.
//
//	GET /boards   every board, as JSON: its name, its target, and the verbs it serves

import (
	"context"
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

	"github.com/davidg238/rete/custos/internal/console"
	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/profile"
	"go.bug.st/serial"
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
// at the board-less paths. A board on a cable is also collected: custos reads
// its console until ctx ends, into day logs under root (D16).
func mountBoards(ctx context.Context, mux *http.ServeMux, boards []Board, esptool, root string) error {
	var infos []BoardInfo
	for _, b := range boards {
		prefixes := []string{"/board/" + b.Name}
		if len(boards) == 1 {
			prefixes = append(prefixes, "")
		}
		verbs, err := mountBoard(ctx, mux, prefixes, b, esptool, root)
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

// openSerial opens a board's console cable without a reset pulse: DTR and RTS
// held low from the open on (rete#311; the CP2102N devkit wires them to EN and
// BOOT). A read timeout is an empty read, so the collector notices a lend.
func openSerial(port string) func() (io.ReadWriteCloser, error) {
	return func() (io.ReadWriteCloser, error) {
		p, err := serial.Open(port, &serial.Mode{BaudRate: 115200, InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false}})
		if err != nil {
			return nil, err
		}
		if err := p.SetReadTimeout(200 * time.Millisecond); err != nil {
			p.Close()
			return nil, err
		}
		return p, nil
	}
}

// openCable is how a board's cable is opened; a test replaces it.
var openCable = openSerial

// subscription is a capture's view of a collected cable: lines come from the
// collector, and Close stops them.
type subscription struct {
	c      *console.Cable
	cancel func()
}

func (s subscription) Send(line string) error { return s.c.Send(line) }
func (s subscription) Close() error           { s.cancel(); return nil }

// cableLine is the debug session's line on a collected cable.
type cableLine struct{ c *console.Cable }

func (l cableLine) Send(line string) error { return l.c.Send(line) }

// mountBoard mounts one board's verbs under each prefix and answers their names.
// The verbs on one board take turns through one lock. On a cable, the collector
// owns the port: the debug session and a capture subscribe to its lines, and a
// load borrows the port from it (esptool and the reset need it to themselves),
// after which the debug session follows the board's new program.
func mountBoard(ctx context.Context, mux *http.ServeMux, prefixes []string, b Board, esptool, root string) ([]string, error) {
	busy := &sync.Mutex{}
	var verbs []string
	var c *console.Cable
	if b.Port != "" {
		lock, err := console.Lock(root, b.Name)
		if err != nil {
			return nil, err
		}
		c = &console.Cable{Store: &console.Store{Root: root, Board: b.Name, Budget: console.DefaultBudget},
			Open: openCable(b.Port), Port: b.Port, Lock: lock}
		go c.Run(ctx)
		mountConsole(mux, prefixes, c, time.Now)
		verbs = append(verbs, "log", "runs", "panics", "health", "lend")
	}
	held := &around{}
	if c != nil {
		held.lend = c.Lend
	}
	if b.Map != "" {
		sm, err := debug.LoadSourceMap(b.Map, b.Source)
		if err != nil {
			return nil, err
		}
		s := debug.NewSession(sm, io.Discard)
		s.AutoFetch = true
		open := func(onLine func(string), onEnd func()) (profile.Sender, error) {
			return subscription{c, c.Subscribe(onLine)}, nil
		}
		if c != nil {
			c.Subscribe(s.OnLine)
			s.Attach(cableLine{c})
			held.follow = follow(s, cableLine{c}, func() (*debug.SourceMap, error) { return debug.LoadSourceMap(b.Map, b.Source) })
		} else {
			argv := append(strings.Fields(b.Host), strings.TrimSuffix(b.Map, ".map.json"))
			if err := checkHost(argv, b.Map); err != nil {
				return nil, err
			}
			line, err := debug.StartHost(argv, true, s.OnLine, s.OnEnd)
			if err != nil {
				return nil, err
			}
			s.Attach(line)
			open = func(onLine func(string), onEnd func()) (profile.Sender, error) {
				return debug.StartHost(argv, false, onLine, onEnd)
			}
		}
		mountDebug(mux, prefixes, s, busy)
		mountProfile(mux, prefixes, b.Map, open, busy)
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

// follow is what a load tells the debug page (peer design D14): it re-reads the
// map and restarts the session on it, on line; or, when the map is not the
// program the board now runs, ends the session as stale until a load it fits.
func follow(s *debug.Session, line debug.Line, reread func() (*debug.SourceMap, error)) func(sha string) string {
	return func(sha string) string {
		sm, err := reread()
		if err == nil && sha != "" && sm.SHA256() != sha {
			err = fmt.Errorf("the board now runs sha256 %.12s, and the debug map describes %.12s: rebuild, then load again",
				sha, sm.SHA256())
		}
		if err != nil {
			s.Stale(err.Error())
			return err.Error()
		}
		s.Restart(sm)
		s.Attach(line)
		return "restarted"
	}
}

// handle registers h at pattern ("METHOD /path") under each prefix.
func handle(mux *http.ServeMux, prefixes []string, pattern string, h http.HandlerFunc) {
	method, path, _ := strings.Cut(pattern, " ")
	for _, p := range prefixes {
		mux.HandleFunc(method+" "+p+path, h)
	}
}
