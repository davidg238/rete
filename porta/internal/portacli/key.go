// Copyright (c) 2026 Ekorau LLC
package portacli

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/spf13/cobra"

	"github.com/davidg238/porta/internal/blob"
)

func keyPaths(db string) (priv, pub string) {
	d := filepath.Dir(db)
	return filepath.Join(d, "porta.key"), filepath.Join(d, "porta.pub")
}

func blobDir(db string) string { return filepath.Join(filepath.Dir(db), "blobs") }

func newKeyCmd() *cobra.Command {
	cmd := &cobra.Command{Use: "key", Short: "Signing keypair for image push"}
	gen := &cobra.Command{
		Use:   "gen",
		Short: "Generate the porta signing keypair beside the db",
		Args:  cobra.NoArgs,
		RunE: func(c *cobra.Command, args []string) error {
			priv, pub := keyPaths(dbPath)
			if err := blob.GenerateKeys(priv, pub); err != nil {
				return err
			}
			raw, _ := os.ReadFile(pub)
			fmt.Fprintf(c.OutOrStdout(), "pub %s (%s)\n", string(raw), pub)
			return nil
		},
	}
	cmd.AddCommand(gen)
	return cmd
}
