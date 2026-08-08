// Copyright (c) 2026 Ekorau LLC

// internal/portacli/blobcmd.go
package portacli

import (
	"encoding/hex"
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/cval"
)

// appRecord builds the goal/apps/<name> App record (RECORDS §4 + design §3):
// the image as a BlobRef, the push-time signature over its hash (D3).
func appRecord(hash []byte, size int64, sig []byte) ([]byte, error) {
	return cval.Encode(cval.Record{
		Tag: "App",
		Fields: cval.Map{
			{K: cval.Symbol("image"), V: cval.Record{
				Tag: "BlobRef",
				Fields: cval.Map{
					{K: cval.Symbol("hash"), V: cval.Bytes(hash)},
					{K: cval.Symbol("size"), V: size},
				},
			}},
			{K: cval.Symbol("sig"), V: cval.Bytes(sig)},
		},
	})
}

func newBlobCmd() *cobra.Command {
	cmd := &cobra.Command{Use: "blob", Short: "Bulk-plane image blobs"}
	put := &cobra.Command{
		Use:   "put <file.oimg>",
		Short: "Store an image blob, sign its hash, print the goal/apps write",
		Args:  cobra.ExactArgs(1),
		RunE: func(c *cobra.Command, args []string) error {
			f, err := os.Open(args[0])
			if err != nil {
				return err
			}
			defer f.Close()
			st, err := blob.Open(blobDir(dbPath))
			if err != nil {
				return err
			}
			hash, size, err := st.Put(f)
			if err != nil {
				return err
			}
			priv, _ := keyPaths(dbPath)
			sig, err := blob.SignHash(priv, hash[:])
			if err != nil {
				return err
			}
			raw, err := appRecord(hash[:], size, sig)
			if err != nil {
				return err
			}
			fmt.Fprintf(c.OutOrStdout(), "hash %s\nsig %s\nporta put <node> goal/apps/main --raw %s\n",
				hex.EncodeToString(hash[:]), hex.EncodeToString(sig), hex.EncodeToString(raw))
			return nil
		},
	}
	cmd.AddCommand(put)
	return cmd
}
