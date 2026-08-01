// Copyright (c) 2026 Ekorau LLC

// Package portacli is the porta gateway's cobra command tree.
//
// During the space-sync rewrite the tree is `serve` alone. v1's verbs (scan,
// ping, device, container, log, monitor, debug, profile) were deleted with the
// command queue they wrote to; they return as sugar over path writes and tree
// reads, and the design record's §4.2 capability audit names where each one
// lands. Nothing was dropped silently.
package portacli

import (
	"context"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/spf13/cobra"
)

var (
	dbPath string

	// Build identity, injected at link time via -ldflags -X (see
	// deploy/build-deb.sh). Defaults make `go run`/tests report a dev build.
	version = "dev"
	commit  = ""
)

func nowSec() int64 { return time.Now().Unix() }

// versionString is what `porta --version` prints and what the status surface
// reports: the version, with the short commit appended when linked in.
func versionString() string {
	if commit != "" {
		return version + " (" + commit + ")"
	}
	return version
}

// NewRootCmd builds the porta command tree.
func NewRootCmd() *cobra.Command {
	root := &cobra.Command{
		Use:     "porta",
		Short:   "porta — northbound gateway for nodus-style nodes",
		Version: versionString(),
	}
	// --db survives the rewrite unused: the space store lands in Task 3 and
	// serve opens it in Task 5, and deploy/ already passes the flag.
	root.PersistentFlags().StringVar(&dbPath, "db", "porta.db", "SQLite database path")
	root.AddCommand(newServeCmd())
	return root
}

// Execute runs the porta CLI. The root context is cancelled on
// SIGINT/SIGTERM so long-running subcommands (serve) can exit cleanly.
// Closes the gap from porta/porta#2 — until this change, the --follow cancel
// path was unreachable in production because cmd.Context() was
// context.Background().
func Execute() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return NewRootCmd().ExecuteContext(ctx)
}
