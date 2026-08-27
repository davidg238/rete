// Copyright (c) 2026 Ekorau LLC

// internal/portacli/snapshot.go
package portacli

import (
	"fmt"

	"github.com/davidg238/porta/internal/space"
	"github.com/spf13/cobra"
)

// newSnapshotCmd produces a consistent, self-contained point-in-time copy of the
// store — the porta↔gatekeeper interface (gatekeeper first-cut design §2). porta
// owns the store, so producing the snapshot is porta's job and an OPERATIONS job
// (D1). `VACUUM INTO` yields a compacted single file off a live WAL database
// WITHOUT blocking the writer, and the result is genuinely immutable: gatekeeper
// opens it with immutable=1 (true of a snapshot, false of a live store), so the
// boundary is real rather than a promise SQLite keeps. See the design §2.1 for
// why a read-only handle on the live store is not a substitute.
func newSnapshotCmd() *cobra.Command {
	var out string
	cmd := &cobra.Command{
		Use:   "snapshot",
		Short: "Write a consistent, immutable-openable copy of the store (VACUUM INTO)",
		Args:  cobra.NoArgs,
		RunE: func(c *cobra.Command, args []string) error {
			if out == "" {
				return fmt.Errorf("snapshot: --out is required")
			}
			st, err := space.OpenExisting(dbPath, nowMs)
			if err != nil {
				return err
			}
			defer st.Close()
			announceStore(c)
			if err := st.SnapshotInto(out); err != nil {
				return err
			}
			fmt.Fprintf(c.OutOrStdout(), "snapshot written: %s\n", out)
			return nil
		},
	}
	cmd.Flags().StringVar(&out, "out", "", "destination snapshot file (must not exist)")
	return cmd
}
