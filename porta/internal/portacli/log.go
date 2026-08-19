// Copyright (c) 2026 Ekorau LLC

// internal/portacli/log.go
package portacli

import (
	"fmt"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
	"github.com/spf13/cobra"
)

// newLogCmd is v1's command audit, generalised: the write log for a node, in
// both directions, with the actor that caused each write. It is strictly more
// than v1 could offer — a drained command queue lost superseded intent, and
// this log keeps it.
func newLogCmd() *cobra.Command {
	var limit int
	var boot int64
	cmd := &cobra.Command{
		Use:   "log <node>",
		Short: "Show the write log for a node, newest first",
		Args:  cobra.ExactArgs(1),
		RunE: func(c *cobra.Command, args []string) error {
			st, err := space.Open(dbPath, nowMs)
			if err != nil {
				return err
			}
			defer st.Close()
			rows, err := st.Events(args[0], limit, boot)
			if err != nil {
				return err
			}
			for _, e := range rows {
				// The arrow is an authority claim, so a gateway verdict gets
				// neither: gw/** is written ABOUT the node and crosses no
				// boundary (design 2026-08-10 §6.1).
				dir := "→"
				switch e.Direction {
				case "in":
					dir = "←"
				case space.DirectionGateway:
					dir = "◆"
				}
				v, err := cval.Decode(e.Value)
				text := "<undecodable>"
				if err == nil {
					text = cval.Format(v)
				}
				fmt.Fprintf(c.OutOrStdout(), "%s %s %-28s seq=%-6d %-9s %-12s %s\n",
					time.UnixMilli(e.TsMs).Format(time.RFC3339), dir, e.Path, e.Seq,
					eventBootLabel(e), e.Actor, text)
			}
			return nil
		},
	}
	cmd.Flags().IntVar(&limit, "limit", 50, "maximum rows")
	// Answerable on the log and nowhere else: it is append-only, so one boot is
	// a filter over rows that each record their own. The cell tree keeps only
	// the latest value per path and could never reconstruct a past boot, which
	// is why `get` has no such flag (rete#210).
	cmd.Flags().Int64Var(&boot, "boot", 0, "show only rows the node published during boot N")
	return cmd
}

func eventBootLabel(e space.Event) string {
	switch {
	case e.Direction == "out":
		return "boot=-"
	case e.BootID == 0:
		return "boot=?"
	default:
		return fmt.Sprintf("boot=%d", e.BootID)
	}
}
