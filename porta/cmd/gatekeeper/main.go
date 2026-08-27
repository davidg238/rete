// Copyright (c) 2026 Ekorau LLC

// gatekeeper — the system entry point (design D1, 2026-08-11; rete#198).
// The shell of the exposed agent-facing surface: /dev serves the dev status
// page and the repo docs tree; /health and /manifest are proxied to porta's
// sheltered operator listener. The query surface is the 2026-08-22 first-cut
// design's build (D3 ratified as (b); stdio-MCP-first, internal/gate) — it
// lands here, never in porta.
package main

import (
	"context"
	"flag"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"github.com/davidg238/porta/internal/gatekeeper"
	"github.com/davidg238/porta/internal/httpsrv"
)

func main() {
	var cfg gatekeeper.Config
	var allow string
	flag.StringVar(&cfg.Bind, "bind", "0.0.0.0", "listen address")
	flag.IntVar(&cfg.Port, "port", 6971, "listen port")
	flag.StringVar(&cfg.Docs, "docs", "", "path to the repo docs/ tree served at /dev (empty = /dev disabled)")
	flag.StringVar(&cfg.PortaHTTP, "porta-http", "http://127.0.0.1:6970",
		"base URL of porta's operator HTTP listener, fronted at /health and /manifest")
	flag.StringVar(&allow, "allow-cidr", strings.Join(httpsrv.DefaultAllowCIDR(), ","),
		"comma-separated CIDR allowlist (empty = serve any peer)")
	flag.Parse()
	cfg.AllowCIDR = strings.Split(allow, ",")

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := gatekeeper.Run(ctx, cfg, log.Printf); err != nil {
		log.Fatal(err)
	}
}
