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
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/profile"
	"github.com/davidg238/rete/custos/internal/tui"
	"github.com/davidg238/rete/protocol/allowlist"
)

func main() {
	// Without a subcommand, custos serves: HTTP is the only agent surface (peer
	// design D12, rete#365). The subcommands are for a person at a shell: tui (a
	// terminal front over a serving custos, rete#341), load (an .oimg into a
	// board's program region, rete#310), and sql, tables, describe over
	// porta's store. debug and profile are kept, and gated, but not advertised: the
	// pages at /debug and /profile and their agent paths are what the docs teach.
	if len(os.Args) > 1 && os.Args[1] == "debug" {
		os.Exit(debug.Run(os.Args[2:], os.Stdin, os.Stdout, os.Stderr))
	}
	if len(os.Args) > 1 && os.Args[1] == "profile" {
		os.Exit(profile.Run(os.Args[2:], os.Stdin, os.Stdout, os.Stderr))
	}
	if len(os.Args) > 1 && os.Args[1] == "tui" {
		os.Exit(tui.Run(os.Args[2:], os.Stderr))
	}
	if len(os.Args) > 1 && os.Args[1] == "load" {
		os.Exit(load.Run(os.Args[2:], os.Stdout, os.Stderr))
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
	var ports, hosts, udps, nodes, maps, sources []string
	each := func(dst *[]string) func(string) error {
		return func(v string) error { *dst = append(*dst, v); return nil }
	}
	flag.Func("board", "a board on its console cable, `name=tty` (a bare name reads ~/.config/rete/console-<name>.env), repeatable: every verb at /board/<name>/ (peer design D15)", each(&ports))
	flag.Func("host", "a host VM instead of a board, `name=binary`: \"dev=/tmp/oevm_img\"; it runs the image beside the board's --map", each(&hosts))
	flag.Func("udp-log", "a board that logs over the mesh, `name=port`: its console datagrams are collected, and its console verbs served", each(&udps))
	flag.Func("porta-node", "a node porta knows, `name=node-id`: load pushes it a program through --porta-http, and is answered at the goal write (rete#398); with a --map, debug and patch through its goal/debug/main cell and dbg/req, each answered Pending (rete#432)", each(&nodes))
	flag.StringVar(&cfg.Advertise, "advertise", "", "this host's IPv6 address as the nodes reach it (a literal: the mesh has no names): each --porta-node is told by goal/console to tee its console here, and serves its console verbs and, with a --map, profile (rete#415)")
	flag.Func("map", "a board's program map, `name=path`.oimg.map.json: without one the board serves load only", each(&maps))
	flag.Func("source", "a board's .nsl, `name=path` (default: beside its --map)", each(&sources))
	flag.StringVar(&cfg.Esptool, "esptool", "esptool.py", "the esptool a board's load runs")
	flag.StringVar(&allow, "allow-cidr", strings.Join(allowlist.Default(), ","),
		"comma-separated CIDR allowlist (empty = serve any peer)")
	flag.Parse()
	cfg.AllowCIDR = strings.Split(allow, ",")
	boards, err := custos.ParseBoards(ports, hosts, udps, nodes, maps, sources)
	if err != nil {
		log.Fatal(err)
	}
	cfg.Boards = boards

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := custos.Run(ctx, cfg, log.Printf); err != nil {
		log.Fatal(err)
	}
}
