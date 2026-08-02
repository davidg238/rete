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
			st, err := space.Open(dbPath, nowMs)
			if err != nil {
				return err
			}
			defer st.Close()
			rows, err := st.Tree(args[0], prefix)
			if err != nil {
				return err
			}
			for _, e := range rows {
				v, err := cval.Decode(e.Value)
				if err != nil {
					// A stored value that will not decode is a real fault, not
					// a display problem — say so rather than printing nothing.
					fmt.Fprintf(c.OutOrStdout(), "%-32s seq=%-6d <undecodable: %v>\n", e.Path, e.Seq, err)
					continue
				}
				fmt.Fprintf(c.OutOrStdout(), "%-32s seq=%-6d %s\n", e.Path, e.Seq, cval.Format(v))
			}
			return nil
		},
	}
	return cmd
}
