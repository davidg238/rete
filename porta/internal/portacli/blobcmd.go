// Copyright (c) 2026 Ekorau LLC

// internal/portacli/blobcmd.go
package portacli

import (
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/spf13/cobra"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/space"
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
	var node string
	put := &cobra.Command{
		Use:   "put <file.oimg> [--node <id>]",
		Short: "Store an image blob and sign its hash; with --node, also deploy it as that node's goal/apps/main",
		Args:  cobra.ExactArgs(1),
		RunE: func(c *cobra.Command, args []string) error {
			// rete#480: the deploy is done here, against the same --db, rather than
			// printed as a line for the operator to paste. The pasted line named the
			// wrong binary, had no --db and left the node blank. The store opens first,
			// so a wrong --db fails before a blob is stored beside it.
			var st *space.Store
			if node != "" {
				var err error
				if st, err = space.OpenExisting(dbPath, nowMs); err != nil {
					return err
				}
				defer st.Close()
			}
			f, err := os.Open(args[0])
			if err != nil {
				return err
			}
			defer f.Close()
			s, err := storeAndSign(dbPath, f)
			if err != nil {
				return err
			}
			out := c.OutOrStdout()
			fmt.Fprintf(out, "hash %s\nsig %s\nrecord %s\n",
				hex.EncodeToString(s.Hash), hex.EncodeToString(s.Sig), hex.EncodeToString(s.Record))
			if st == nil {
				return nil
			}
			seq, err := st.Write(node, "goal/apps/main", cval.KindCell, s.Record, currentActor(), "blob put "+filepath.Base(args[0]))
			if err != nil {
				return err
			}
			fmt.Fprintf(out, "wrote %s goal/apps/main seq=%d\n", node, seq)
			return nil
		},
	}
	put.Flags().StringVar(&node, "node", "", "also write the image as this node's goal/apps/main")
	cmd.AddCommand(put)
	return cmd
}
