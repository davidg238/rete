// Copyright (c) 2026 Ekorau LLC

// Package storefixture opens the committed stores under
// protocol/fixtures/store/ for custos's tests. porta's own writer writes them
// (porta/internal/space/fixtures_test.go, drift-guarded there); custos reads
// porta's store and may not import porta, so it loads the text instead
// (rete#338).
package storefixture

import (
	"database/sql"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	_ "github.com/mattn/go-sqlite3"
)

// Path is the committed SQL text of the named fixture.
func Path(name string) string {
	_, here, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(here), "..", "..", "..", "protocol", "fixtures", "store", name+".sql")
}

// Live loads the named fixture into a fresh store and holds it open in WAL
// mode until the test ends, as porta holds its live store: a -wal file sits
// beside it, and custos's live binding reads it as it would the gateway's.
func Live(t testing.TB, name string) string {
	t.Helper()
	text, err := os.ReadFile(Path(name))
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "store.db")
	db, err := sql.Open("sqlite3", "file:"+path+"?_journal_mode=WAL")
	if err != nil {
		t.Fatal(err)
	}
	db.SetMaxOpenConns(1)
	t.Cleanup(func() { db.Close() })
	if _, err := db.Exec(string(text)); err != nil {
		t.Fatalf("load %s: %v", name, err)
	}
	return path
}

// Snap is the named fixture as `porta snapshot` writes it.
func Snap(t testing.TB, name string) string {
	t.Helper()
	return Snapshot(t, Live(t, name))
}

// Snapshot copies the store at path the way `porta snapshot` does, with
// VACUUM INTO: one self-contained file, no -wal, safe to open immutable.
func Snapshot(t testing.TB, path string) string {
	t.Helper()
	dest := filepath.Join(t.TempDir(), "snap.db")
	db, err := sql.Open("sqlite3", "file:"+path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec("VACUUM INTO ?", dest); err != nil {
		t.Fatal(err)
	}
	return dest
}
