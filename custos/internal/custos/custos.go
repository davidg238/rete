// Copyright (c) 2026 Ekorau LLC

// Package custos is the system entry point — the exposed, agent-facing
// surface over porta (design D1, 2026-08-11: porta is OPERATIONS, sheltered
// and schema-blind; custos is the query and authority surface). This is
// the SHELL of that surface: it serves the dev status page and the repo docs
// tree at /dev, and fronts porta's operator routes (/health, /manifest) by
// proxy. The query surface itself is the first-cut design's build
// (2026-08-22 note: D3 ratified as (b), stdio-MCP-first, internal/gate) —
// nothing here decodes a value.
//
// A separate process on purpose: custos can be restarted and iterated on
// freely, while a porta restart costs every node an outage. Refreshing the
// status page is an edit to the docs directory — never a restart of anything.
package custos

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os/exec"
	"strings"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/imagemap"
	"github.com/davidg238/rete/custos/internal/profile"
	"github.com/davidg238/rete/protocol/allowlist"
)

type Config struct {
	Bind      string   // listen address, e.g. "0.0.0.0"
	Port      int      // listen port
	Docs      string   // path to the repo docs/ tree ("" = /dev disabled)
	PortaHTTP string   // base URL of porta's operator HTTP listener ("" = proxy answers 502)
	AllowCIDR []string // peer allowlist, same semantics as porta's --http-allow-cidr
	// Store is porta's sqlite store, read through the gate's `live` binding for
	// the fleet page ("" = no fleet page). Descriptors is the reading-rule table
	// the gate needs to open it.
	Store       string
	Descriptors string
	// ProfilePort is a board's console cable for the profile page, ranked
	// through ProfileMap ("" = no profile page). ProfileHost is a host VM
	// command line instead, started afresh for each capture: the agent's
	// board-free path (rete#364). Exactly one of ProfilePort and ProfileHost.
	ProfilePort string
	ProfileHost string
	ProfileMap  string
	// DebugPort is a board's console cable for the debug page, held from
	// start-up; DebugMap is its image's map and DebugSource the .nsl ("" =
	// beside the map). "" = no debug page.
	DebugPort   string
	DebugMap    string
	DebugSource string
	// DebugHost is a host VM command line (split on white space) the debug page
	// runs instead of holding a board's cable: the agent's board-free path
	// (rete#362). Exactly one of DebugPort and DebugHost.
	DebugHost string
}

// Handler builds the custos mux: / lands on the fleet page when a store is
// configured and on the dev status page otherwise, /dev always on the status
// page, /dev/ serves the docs tree read-only (markdown as text/plain so a
// browser displays the documents of record), /health + /manifest proxy to
// porta, /fleet + /q/{name} serve the fleet (fleet.go), /profile a board's
// method profile (profile.go), and /debug a debugging session on a board or a
// host VM (debug.go).
func Handler(cfg Config) (http.Handler, error) {
	mux := http.NewServeMux()
	// Every page's stylesheet, whichever pages are mounted.
	mux.HandleFunc("GET /fleet.css", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/css; charset=utf-8")
		w.Write(fleetCSS)
	})
	if cfg.ProfilePort != "" || cfg.ProfileHost != "" {
		if cfg.ProfilePort != "" && cfg.ProfileHost != "" {
			return nil, fmt.Errorf("custos: give one of --profile-port or --profile-host")
		}
		if cfg.ProfileMap == "" {
			return nil, fmt.Errorf("custos: the profile page needs --profile-map (<name>.oimg.map.json)")
		}
		open := func(onLine func(string), onEnd func()) (profile.Sender, error) {
			return debug.OpenPort(cfg.ProfilePort, 115200, onLine, onEnd)
		}
		if cfg.ProfileHost != "" {
			argv := strings.Fields(cfg.ProfileHost)
			if err := checkHost(argv, cfg.ProfileMap); err != nil {
				return nil, fmt.Errorf("custos: --profile-host: %w", err)
			}
			open = func(onLine func(string), onEnd func()) (profile.Sender, error) {
				return debug.StartHost(argv, false, onLine, onEnd)
			}
		}
		mountProfile(mux, cfg.ProfileMap, open)
	}
	if cfg.DebugPort != "" || cfg.DebugHost != "" {
		if cfg.DebugPort != "" && cfg.DebugHost != "" {
			return nil, fmt.Errorf("custos: give one of --debug-port or --debug-host")
		}
		if cfg.DebugMap == "" {
			return nil, fmt.Errorf("custos: the debug page needs --debug-map (<name>.oimg.map.json)")
		}
		if cfg.DebugPort != "" && cfg.DebugPort == cfg.ProfilePort {
			return nil, fmt.Errorf("custos: --debug-port and --profile-port are one cable, and a cable has one holder")
		}
		sm, err := debug.LoadSourceMap(cfg.DebugMap, cfg.DebugSource)
		if err != nil {
			return nil, err
		}
		s := debug.NewSession(sm, io.Discard)
		s.AutoFetch = true
		var line debug.Line
		if cfg.DebugPort != "" {
			if line, err = debug.OpenPort(cfg.DebugPort, 115200, s.OnLine, s.OnLost); err != nil {
				return nil, fmt.Errorf("custos: --debug-port: %w", err)
			}
		} else {
			argv := strings.Fields(cfg.DebugHost)
			if err := checkHost(argv, cfg.DebugMap); err != nil {
				return nil, fmt.Errorf("custos: --debug-host: %w", err)
			}
			if line, err = debug.StartHost(argv, true, s.OnLine, s.OnEnd); err != nil {
				return nil, fmt.Errorf("custos: --debug-host: %w", err)
			}
		}
		s.Attach(line)
		mountDebug(mux, s, sm)
	}
	if cfg.Store != "" {
		g, err := openFleet(cfg.Store, cfg.Descriptors)
		if err != nil {
			return nil, err
		}
		mountFleet(mux, g)
		mux.HandleFunc("/{$}", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/fleet", http.StatusFound)
		})
	}
	if cfg.Docs != "" {
		fs := http.FileServer(http.Dir(cfg.Docs))
		mux.Handle("/dev/", http.StripPrefix("/dev/",
			http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if strings.HasSuffix(r.URL.Path, ".md") {
					w.Header().Set("Content-Type", "text/plain; charset=utf-8")
				}
				fs.ServeHTTP(w, r)
			})))
		statusPage := func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/dev/status/dev-status.html", http.StatusFound)
		}
		mux.HandleFunc("/dev", statusPage)
		if cfg.Store == "" {
			mux.HandleFunc("/{$}", statusPage)
		}
	}
	proxy, err := portaProxy(cfg.PortaHTTP)
	if err != nil {
		return nil, err
	}
	mux.Handle("/health", proxy)
	mux.Handle("/manifest", proxy)
	return mux, nil
}

// portaProxy fronts the sheltered porta listener. An empty base answers 502 —
// the route exists and names its missing backend rather than 404ing, so a
// misconfiguration is loud instead of reading as "no such surface".
func portaProxy(base string) (http.Handler, error) {
	if base == "" {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "custos: no porta backend configured (--porta-http)", http.StatusBadGateway)
		}), nil
	}
	u, err := url.Parse(base)
	if err != nil {
		return nil, fmt.Errorf("porta-http %q: %w", base, err)
	}
	return httputil.NewSingleHostReverseProxy(u), nil
}

// Run serves until ctx is cancelled, then shuts down gracefully.
func Run(ctx context.Context, cfg Config, logf func(string, ...any)) error {
	h, err := Handler(cfg)
	if err != nil {
		return err
	}
	allowed, err := allowlist.Parse(cfg.AllowCIDR)
	if err != nil {
		return err
	}
	srv := &http.Server{
		Addr:    net.JoinHostPort(cfg.Bind, fmt.Sprint(cfg.Port)),
		Handler: allowlist.Middleware(allowed)(h),
	}
	errc := make(chan error, 1)
	go func() { errc <- srv.ListenAndServe() }()
	logf("custos: serving on %s (docs=%s, porta=%s, store=%s)", srv.Addr, cfg.Docs, cfg.PortaHTTP, cfg.Store)
	select {
	case <-ctx.Done():
		shctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		return srv.Shutdown(shctx)
	case err := <-errc:
		return err
	}
}

// checkHost refuses a host VM command line custos cannot run, or whose .oimg
// is not the image the map describes.
func checkHost(argv []string, mapPath string) error {
	if len(argv) == 0 {
		return fmt.Errorf("an empty command line")
	}
	if _, err := exec.LookPath(argv[0]); err != nil {
		return err
	}
	m, err := imagemap.Load(mapPath)
	if err != nil {
		return err
	}
	for _, a := range argv {
		if strings.HasSuffix(a, ".oimg") {
			if err := m.CheckImage(a); err != nil {
				return err
			}
		}
	}
	return nil
}
