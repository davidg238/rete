// Copyright (c) 2026 Ekorau LLC

// internal/portacli/put.go
package portacli

import (
	"encoding/hex"
	"fmt"
	"os"
	"os/user"
	"strconv"

	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/rete/protocol/cval"
	"github.com/spf13/cobra"
)

// newPutCmd is the general path write. The operator-friendly verbs (porta app
// run, porta mode, …) are sugar over this — verbs live at the CLI and
// nowhere else (design D2).
func newPutCmd() *cobra.Command {
	var kind, reason, actor string
	var raw bool
	cmd := &cobra.Command{
		Use:   "put <node> <path> <value>",
		Short: "Write a value to a node's goal tree",
		Args:  cobra.ExactArgs(3),
		RunE: func(c *cobra.Command, args []string) error {
			k := cval.KindCell
			if kind == "stream" {
				k = cval.KindStream
			} else if kind != "cell" {
				return fmt.Errorf("--kind must be cell or stream, got %q", kind)
			}
			var enc []byte
			var err error
			if raw {
				enc, err = hex.DecodeString(args[2])
				if err != nil {
					return fmt.Errorf("--raw value is not hex: %w", err)
				}
				if err := cval.Validate(enc); err != nil {
					return fmt.Errorf("--raw value is not a closed cval encoding: %w", err)
				}
			} else {
				enc, err = cval.Encode(parseScalar(args[2]))
				if err != nil {
					return err
				}
			}
			who := actor
			if who == "" {
				who = currentActor()
			}
			st, err := space.OpenExisting(dbPath, nowMs)
			if err != nil {
				return err
			}
			defer st.Close()
			seq, err := st.Write(args[0], args[1], k, enc, who, reason)
			if err != nil {
				return err
			}
			fmt.Fprintf(c.OutOrStdout(), "wrote %s seq=%d\n", args[1], seq)
			return nil
		},
	}
	cmd.Flags().StringVar(&kind, "kind", "cell", "cell or stream")
	cmd.Flags().StringVar(&reason, "reason", "", "why this write is being made (recorded in the log)")
	cmd.Flags().StringVar(&actor, "actor", "", "who is making this write (defaults to the OS user)")
	cmd.Flags().BoolVar(&raw, "raw", false, "value is hex-encoded raw deterministic CBOR")
	return cmd
}

// parseScalar covers the scalar cases an operator types. Records and nested
// values are not typeable on a command line and arrive through the HTTP
// surface (the operator-surface plan); a string is the honest fallback.
func parseScalar(s string) any {
	if s == "true" {
		return true
	}
	if s == "false" {
		return false
	}
	if s == "nil" {
		return nil
	}
	if len(s) > 1 && s[0] == '#' {
		return cval.Symbol(s[1:])
	}
	if n, err := strconv.ParseInt(s, 10, 64); err == nil {
		return n
	}
	if f, err := strconv.ParseFloat(s, 64); err == nil {
		return f
	}
	return s
}

func currentActor() string {
	if u, err := user.Current(); err == nil && u.Username != "" {
		return u.Username
	}
	if h, err := os.Hostname(); err == nil {
		return h
	}
	return "unknown"
}
