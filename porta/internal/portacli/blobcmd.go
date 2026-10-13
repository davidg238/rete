// Copyright (c) 2026 Ekorau LLC

// internal/portacli/blobcmd.go
package portacli

import (
	"encoding/hex"
	"fmt"
	"io"
	"os"

	"github.com/spf13/cobra"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/rete/protocol/cval"
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

// Signed is one stored, signed blob: what `porta blob put` prints and POST
// /blob answers. Record is the goal/apps/<name> value, ready to write.
type Signed struct {
	Hash, Sig, Record []byte
	Size              int64
}

// storeAndSign is the signing seat (D3), shared by the CLI and POST /blob:
// store the bytes content-addressed beside db, sign their SHA-256 with porta's
// key, and build the App record that names them.
func storeAndSign(db string, r io.Reader) (Signed, error) {
	st, err := blob.Open(blobDir(db))
	if err != nil {
		return Signed{}, err
	}
	hash, size, err := st.Put(r)
	if err != nil {
		return Signed{}, err
	}
	priv, _ := keyPaths(db)
	sig, err := blob.SignHash(priv, hash[:])
	if err != nil {
		return Signed{}, err
	}
	raw, err := appRecord(hash[:], size, sig)
	if err != nil {
		return Signed{}, err
	}
	return Signed{Hash: hash[:], Sig: sig, Record: raw, Size: size}, nil
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
			s, err := storeAndSign(dbPath, f)
			if err != nil {
				return err
			}
			fmt.Fprintf(c.OutOrStdout(), "hash %s\nsig %s\nporta put <node> goal/apps/main --raw %s\n",
				hex.EncodeToString(s.Hash), hex.EncodeToString(s.Sig), hex.EncodeToString(s.Record))
			return nil
		},
	}
	cmd.AddCommand(put)
	return cmd
}
