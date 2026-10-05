// Copyright (c) 2026 Ekorau LLC

// internal/portacli/get.go
package portacli

import (
	"fmt"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/spf13/cobra"
)

// newGetCmd dumps a node's cell tree. goal/X against obs/X is the convergence
// view AND the self-heal query — the same diff, which is why self-heal is no
// longer a mechanism.
func newGetCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "get <node> [prefix]",
		Short: "Dump a node's cell tree",
		Args:  cobra.RangeArgs(1, 2),
		RunE: func(c *cobra.Command, args []string) error {
			prefix := ""
			if len(args) == 2 {
				prefix = args[1]
			}
			st, err := space.OpenExisting(dbPath, nowMs)
			if err != nil {
				return err
			}
			defer st.Close()
			announceStore(c)
			rows, err := st.Tree(args[0], prefix)
			if err != nil {
				return err
			}
			boot, err := st.CurrentBoot(args[0])
			if err != nil {
				return err
			}

			// THE BANNER GOES FIRST AND IT GOES TO STDOUT. A rebooted node
			// rewrites its whole tree over several seconds, so a read taken
			// inside that window mixes this boot's values with the last boot's
			// and nothing in a row says which is which (rete#210 — it nearly
			// inverted a hardware finding on 2026-08-14). stdout rather than
			// stderr because a warning a pipe eats is not a guard: the same
			// mistake made a `tail -F | grep` monitor report three missed wakes
			// as nothing happening.
			stale := 0
			for _, e := range rows {
				if isStale(e, boot) {
					stale++
				}
			}
			if stale > 0 {
				fmt.Fprintf(c.OutOrStdout(),
					"! MIXED READ: %d of %d rows predate boot %d — see porta log --boot %d\n",
					stale, len(rows), boot, boot)
			}

			for _, e := range rows {
				mark, note := " ", ""
				if isStale(e, boot) {
					mark, note = "~", "  STALE"
				}
				v, err := cval.Decode(e.Value)
				if err != nil {
					// A stored value that will not decode is a real fault, not
					// a display problem — say so rather than printing nothing.
					fmt.Fprintf(c.OutOrStdout(), "%s %-32s seq=%-6d %-9s <undecodable: %v>\n",
						mark, e.Path, e.Seq, bootLabel(e), err)
					continue
				}
				fmt.Fprintf(c.OutOrStdout(), "%s %-32s seq=%-6d %-9s %s%s\n",
					mark, e.Path, e.Seq, bootLabel(e), cval.Format(v), note)
			}
			return nil
		},
	}
	return cmd
}

// isStale is space.IsStale over one cell, the rule custos's cells.stale shares.
func isStale(e space.Cell, currentBoot int64) bool {
	return space.IsStale(e.Direction, e.BootID, currentBoot)
}

func bootLabel(e space.Cell) string {
	switch {
	case e.Direction == "out":
		return "boot=-"
	case e.BootID == 0:
		return "boot=?"
	default:
		return fmt.Sprintf("boot=%d", e.BootID)
	}
}
