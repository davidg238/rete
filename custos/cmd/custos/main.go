// Copyright (c) 2026 Ekorau LLC

// custos — the system entry point (design D1, 2026-08-11; rete#198).
// The shell of the exposed agent-facing surface: /dev serves the dev status
// page and the repo docs tree; /health and /manifest are proxied to porta's
// sheltered operator listener. The query surface is the 2026-08-22 first-cut
// design's build (D3 ratified as (b); stdio-MCP-first, internal/gate) — it
// lands here, never in porta.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/profile"
	"github.com/davidg238/rete/protocol/allowlist"
)

func main() {
	// A subcommand is the shell surface: debug (the in-image debugger, over a
	// cable or a host VM), profile (a profile dump ranked by name), and sql, tables, describe over porta's store. Without
	// one, custos serves.
	if len(os.Args) > 1 && os.Args[1] == "debug" {
		os.Exit(debug.Run(os.Args[2:], os.Stdin, os.Stdout, os.Stderr))
	}
	if len(os.Args) > 1 && os.Args[1] == "profile" {
		os.Exit(profile.Run(os.Args[2:], os.Stdin, os.Stdout, os.Stderr))
	}
	if len(os.Args) > 1 && !strings.HasPrefix(os.Args[1], "-") {
		if err := custos.RunCLI(os.Args[1:], os.Stdout); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	var cfg custos.Config
	var allow string
	flag.StringVar(&cfg.Bind, "bind", "0.0.0.0", "listen address")
	flag.IntVar(&cfg.Port, "port", 6971, "listen port")
	flag.StringVar(&cfg.Docs, "docs", "", "path to the repo docs/ tree served at /dev (empty = /dev disabled)")
	flag.StringVar(&cfg.PortaHTTP, "porta-http", "http://127.0.0.1:6970",
		"base URL of porta's operator HTTP listener, fronted at /health and /manifest")
	flag.StringVar(&cfg.Store, "store", "", "porta's sqlite store, read-only, for the fleet page at /fleet (empty = no fleet page)")
	flag.StringVar(&cfg.Descriptors, "descriptors", "", "protocol/descriptors/records.yaml, required with --store")
	flag.StringVar(&allow, "allow-cidr", strings.Join(allowlist.Default(), ","),
		"comma-separated CIDR allowlist (empty = serve any peer)")
	flag.Parse()
	cfg.AllowCIDR = strings.Split(allow, ",")

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := custos.Run(ctx, cfg, log.Printf); err != nil {
		log.Fatal(err)
	}
}
