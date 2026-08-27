// Copyright (c) 2026 Ekorau LLC

package space

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

// A snapshot is the porta↔gatekeeper interface (first-cut design §2): it must be
// a consistent copy that opens immutable=1 (writing nothing), and it must not
// disturb the live source.
func TestSnapshotIntoOpensImmutableAndLeavesSourceIntact(t *testing.T) {
	st := testStore(t)
	// Some real content to copy: a cell and an event.
	if _, err := st.Write("n1", "goal/name", cval.KindCell, enc(t, "bathroom-1"), "op", "commission"); err != nil {
		t.Fatalf("Write: %v", err)
	}
	if _, _, err := st.Ingest("n1", 41, []Entry{{Seq: 1, Path: "obs/name", Kind: cval.KindCell, Value: enc(t, "bathroom-1")}}); err != nil {
		t.Fatalf("Ingest: %v", err)
	}

	snap := filepath.Join(t.TempDir(), "snap.db")
	if err := st.SnapshotInto(snap); err != nil {
		t.Fatalf("SnapshotInto: %v", err)
	}

	// The source must still be writable and consistent after the snapshot.
	if _, err := st.Write("n1", "goal/mode", cval.KindCell, enc(t, "run"), "op", "post-snap"); err != nil {
		t.Fatalf("source write after snapshot failed: %v", err)
	}

	// The snapshot opens under a GENUINE OS-level immutable boundary: chmod a-w
	// on the file AND its directory, then immutable=1. A live WAL store cannot be
	// read this way (a WAL reader must write -shm); a true snapshot can. This is
	// the design §2.1 boundary, exercised.
	dir := filepath.Dir(snap)
	if err := os.Chmod(snap, 0o444); err != nil {
		t.Fatalf("chmod file: %v", err)
	}
	if err := os.Chmod(dir, 0o555); err != nil {
		t.Fatalf("chmod dir: %v", err)
	}
	t.Cleanup(func() { os.Chmod(dir, 0o755) })

	ro, err := sql.Open("sqlite3", "file:"+snap+"?immutable=1&mode=ro")
	if err != nil {
		t.Fatalf("open snapshot immutable: %v", err)
	}
	defer ro.Close()

	var cells int
	if err := ro.QueryRow("SELECT count(*) FROM cells").Scan(&cells); err != nil {
		t.Fatalf("read snapshot: %v", err)
	}
	if cells == 0 {
		t.Fatalf("snapshot has no cells — content did not copy")
	}

	// The snapshot froze BEFORE goal/mode was written; it must not carry it.
	var hasMode int
	if err := ro.QueryRow("SELECT count(*) FROM cells WHERE path = 'goal/mode'").Scan(&hasMode); err != nil {
		t.Fatalf("read snapshot mode: %v", err)
	}
	if hasMode != 0 {
		t.Fatalf("snapshot is not a point-in-time copy: it carries a cell written after it")
	}

	// No -wal / -shm sidecar was created beside the immutable snapshot.
	for _, suffix := range []string{"-wal", "-shm"} {
		if _, err := os.Stat(snap + suffix); err == nil {
			t.Fatalf("immutable snapshot created a %s sidecar — the boundary is not real", suffix)
		}
	}
}

func TestSnapshotIntoRefusesExistingFile(t *testing.T) {
	st := testStore(t)
	dest := filepath.Join(t.TempDir(), "exists.db")
	if err := os.WriteFile(dest, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := st.SnapshotInto(dest); err == nil {
		t.Fatalf("SnapshotInto overwrote an existing file; it must refuse")
	}
}
