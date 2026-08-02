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
			rows, err := st.Events(args[0], limit)
			if err != nil {
				return err
			}
			for _, e := range rows {
				dir := "→"
				if e.Direction == "in" {
					dir = "←"
				}
				v, err := cval.Decode(e.Value)
				text := "<undecodable>"
				if err == nil {
					text = cval.Format(v)
				}
				fmt.Fprintf(c.OutOrStdout(), "%s %s %-28s seq=%-6d %-12s %s\n",
					time.UnixMilli(e.TsMs).Format(time.RFC3339), dir, e.Path, e.Seq, e.Actor, text)
			}
			return nil
		},
	}
	cmd.Flags().IntVar(&limit, "limit", 50, "maximum rows")
	return cmd
}
