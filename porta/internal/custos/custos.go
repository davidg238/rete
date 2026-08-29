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
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"time"

	"github.com/davidg238/porta/internal/httpsrv"
)

type Config struct {
	Bind      string   // listen address, e.g. "0.0.0.0"
	Port      int      // listen port
	Docs      string   // path to the repo docs/ tree ("" = /dev disabled)
	PortaHTTP string   // base URL of porta's operator HTTP listener ("" = proxy answers 502)
	AllowCIDR []string // peer allowlist, same semantics as porta's --http-allow-cidr
}

// Handler builds the custos mux: / and /dev land on the status page,
// /dev/ serves the docs tree read-only (markdown as text/plain so a browser
// displays the documents of record), and /health + /manifest proxy to porta.
func Handler(cfg Config) (http.Handler, error) {
	mux := http.NewServeMux()
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
		mux.HandleFunc("/{$}", statusPage)
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
	allowed, err := httpsrv.AllowCIDR(cfg.AllowCIDR)
	if err != nil {
		return err
	}
	srv := &http.Server{
		Addr:    net.JoinHostPort(cfg.Bind, fmt.Sprint(cfg.Port)),
		Handler: httpsrv.AllowlistMiddleware(allowed)(h),
	}
	errc := make(chan error, 1)
	go func() { errc <- srv.ListenAndServe() }()
	logf("custos: serving on %s (docs=%s, porta=%s)", srv.Addr, cfg.Docs, cfg.PortaHTTP)
	select {
	case <-ctx.Done():
		shctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		return srv.Shutdown(shctx)
	case err := <-errc:
		return err
	}
}
