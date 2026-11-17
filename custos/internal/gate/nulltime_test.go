// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"context"
	"database/sql"
	"testing"

	"github.com/davidg238/rete/custos/internal/storefixture"
)

// TestDecodedTableCarriesANullTime: since #443 porta stores a journal row from a
// boot it never heard from with events.ts_ms NULL. The decoded copy must carry
// that row with a NULL ts_ms, not fail the whole snapshot on it.
func TestDecodedTableCarriesANullTime(t *testing.T) {
	live := storefixture.Live(t, "metrics")
	db, err := sql.Open("sqlite3", "file:"+live)
	if err != nil {
		t.Fatal(err)
	}
	res, err := db.Exec(`UPDATE events SET ts_ms = NULL
	                      WHERE id = (SELECT MIN(id) FROM events WHERE path = 'sys/metrics')`)
	if err != nil {
		t.Fatal(err)
	}
	if n, _ := res.RowsAffected(); n != 1 {
		t.Fatalf("fixture has no sys/metrics event to unstamp (%d rows)", n)
	}
	db.Close()

	g, err := Open(storefixture.Snapshot(t, live), descriptors(t))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer g.Close()
	ans, err := g.Query(context.Background(), Query{
		SQL: "SELECT COUNT(*) FROM " + TableName("sys/metrics") + " WHERE ts_ms IS NULL"})
	if err != nil {
		t.Fatalf("Query: %v", err)
	}
	if len(ans.Rows) != 1 || ans.Rows[0][0] != int64(1) {
		t.Fatalf("rows = %v, want one unstamped sys/metrics row", ans.Rows)
	}
}
