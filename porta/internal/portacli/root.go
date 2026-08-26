// Copyright (c) 2026 Ekorau LLC

// Package portacli is the porta gateway's cobra command tree.
//
// The tree is serve, get, put, log, key and blob — the last two added by rete#25
// for the image-push path (`key gen` once, `blob put` storing and signing an
// image, whose output is a `put` line: still no verb the store knows about).
//
// v1's verbs (scan, ping, device, container, monitor, debug, profile) were
// deleted with the command queue they wrote to; put and get are the general
// forms they become sugar over, since verbs live at the CLI and nowhere else
// (design D2). The design record's §4.2 capability audit names where each v1
// verb lands. Nothing was dropped silently.
package portacli

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
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

// nowMs is the store's clock. Everything in the space model is milliseconds
// (design D4.2); nowSec survives only for serverstat, which predates it.
func nowMs() int64 { return time.Now().UnixMilli() }

// versionString is what `porta --version` prints and what the status surface
// reports: the version, with the short commit appended when linked in.
func versionString() string {
	if commit != "" {
		return version + " (" + commit + ")"
	}
	return version
}

// announceStore names the store a read answered from, on stderr where it
// cannot pollute output a gate parses (rete#221): a correct-looking empty
// answer must still say what it read, because "no rows yet" and "wrong
// database" print identically on stdout.
func announceStore(c *cobra.Command) {
	path := dbPath
	if abs, err := filepath.Abs(path); err == nil {
		path = abs
	}
	fmt.Fprintf(c.ErrOrStderr(), "# store %s\n", path)
}

// NewRootCmd builds the porta command tree.
func NewRootCmd() *cobra.Command {
	root := &cobra.Command{
		Use:     "porta",
		Short:   "porta — northbound gateway for nodus-style nodes",
		Version: versionString(),
	}
	// --db is persistent so every verb reads the same store without each
	// redeclaring the flag (which would shadow this one and diverge).
	root.PersistentFlags().StringVar(&dbPath, "db", "porta.db", "SQLite database path")
	root.AddCommand(
		newServeCmd(),
		newGetCmd(),
		newPutCmd(),
		newLogCmd(),
		newKeyCmd(),
		newBlobCmd(),
	)
	return root
}

// Execute runs the porta CLI. The root context is cancelled on
// SIGINT/SIGTERM so long-running subcommands (serve) can exit cleanly.
// Closes the gap from https://github.com/davidg238/porta/issues/2 — until this change, the --follow cancel
// path was unreachable in production because cmd.Context() was
// context.Background().
func Execute() error {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	return NewRootCmd().ExecuteContext(ctx)
}
