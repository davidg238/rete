// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/storefixture"
)

// snapHistory opens the history fixture (porta/internal/space/fixtures_test.go
// says what each node in it is for) on the snap binding.
func snapHistory(t *testing.T) *Gate {
	t.Helper()
	g, err := Open(storefixture.Snap(t, "history"), descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { g.Close() })
	return g
}

// TestScalarPathsHaveHistoryTables: a scalar path gets a decoded table like a
// record path does, so goal/name's whole history is answerable on snap — and
// its reading rule rides with the `value` column.
func TestScalarPathsHaveHistoryTables(t *testing.T) {
	g := snapHistory(t)
	res := query(t, g, Query{SQL: `SELECT node_id, value FROM goal_name ORDER BY ts_ms`})
	if len(res.Rows) != 2 || asText(res.Rows[0][1]) != "kitchen" || asText(res.Rows[1][1]) != "porch" {
		t.Fatalf("goal_name = %v", res.Rows)
	}
	found := false
	for _, n := range res.Notes {
		if n.Table == "goal_name" && n.Column == "value" && n.Meaning != "" {
			found = true
		}
	}
	if !found {
		t.Errorf("no reading rule rode with goal_name.value: %+v", res.Notes)
	}
}

// TestRoleIsTheGoalNameHistory: one row per commissioning, open-ended on the
// current one.
func TestRoleIsTheGoalNameHistory(t *testing.T) {
	g := snapHistory(t)
	res := query(t, g, Query{SQL: `SELECT role, node_id, from_ms, to_ms FROM role ORDER BY from_ms`})
	if len(res.Rows) != 2 {
		t.Fatalf("role = %v", res.Rows)
	}
	if asText(res.Rows[0][0]) != "kitchen" || res.Rows[0][3] == nil || asInt(t, res.Rows[0][3]) != asInt(t, res.Rows[1][2]) {
		t.Errorf("kitchen should end where porch begins: %v", res.Rows)
	}
	if res.Rows[1][3] != nil {
		t.Errorf("the current role has an end: %v", res.Rows[1])
	}
}

// TestBootIsTheSegmentationUnit: one row per (node, boot) with its span and
// the build it ran.
func TestBootIsTheSegmentationUnit(t *testing.T) {
	g := snapHistory(t)
	got := rowsBy(t, query(t, g, Query{SQL: `SELECT node_id, boot_id, first_ms, last_ms, exchanges, build FROM boot`}), 2)
	if r := got["slotA 10"]; asText(r["build"]) != "b1" || asInt(t, r["exchanges"]) != 2 {
		t.Errorf("slotA 10 = %v", r)
	}
	if r := got["slotB 13"]; r == nil || asInt(t, r["first_ms"]) > asInt(t, r["last_ms"]) {
		t.Errorf("slotB 13 = %v", r)
	}
	if len(got) != 12 {
		t.Errorf("want 12 boots, got %d: %v", len(got), got)
	}
}

// TestGCEnvelopeIsTheReadableGauge: rete#153's rule as SQL — the per-cycle max
// of lfb, with the first five cycles of a boot discarded.
func TestGCEnvelopeIsTheReadableGauge(t *testing.T) {
	g := snapHistory(t)
	cyc := rowsBy(t, query(t, g, Query{SQL: `SELECT node_id, gcc, lfb_max, lb_min, samples, build FROM gc_cycle`}), 2)
	if len(cyc) != 7 {
		t.Fatalf("gc_cycle has %d cycles, want 7 (boot 0 excluded): %v", len(cyc), cyc)
	}
	if r := cyc["slotA 3"]; asInt(t, r["lfb_max"]) != 307 || asInt(t, r["lb_min"]) != 40 ||
		asInt(t, r["samples"]) != 2 || asText(r["build"]) != "b1" {
		t.Errorf("cycle 3 = %v", r)
	}
	env := query(t, g, Query{SQL: `SELECT gcc FROM gc_envelope ORDER BY gcc`})
	if len(env.Rows) != 2 || asInt(t, env.Rows[0][0]) != 6 || asInt(t, env.Rows[1][0]) != 7 {
		t.Errorf("gc_envelope = %v, want cycles 6 and 7", env.Rows)
	}
}

// TestDeviceConflictFindsTwoBoardsUnderOneId: an id whose boots overlap in time
// is two boards — §6, and #186's own damage.
func TestDeviceConflictFindsTwoBoardsUnderOneId(t *testing.T) {
	g := snapHistory(t)
	res := query(t, g, Query{SQL: `SELECT node_id, boot_a, boot_b FROM device_conflict`})
	if len(res.Rows) != 1 || asText(res.Rows[0][0]) != "dup" ||
		asInt(t, res.Rows[0][1]) != 5 || asInt(t, res.Rows[0][2]) != 7 {
		t.Errorf("device_conflict = %v, want dup 5/7 alone", res.Rows)
	}
}

// TestDeviceIsTheDerivedLineage: §5.3's invariant. slotA, slotB and the factory
// id are one board, named by its current id; `other` continues slotB's counter
// but overlaps it in time, so it stays its own device.
func TestDeviceIsTheDerivedLineage(t *testing.T) {
	g := snapHistory(t)
	got := rowsBy(t, query(t, g, Query{SQL: `SELECT node_id, device FROM device`}), 1)
	for _, id := range []string{"slotA", "slotB", "f00000000001"} {
		if asText(got[id]["device"]) != "f00000000001" {
			t.Errorf("%s device = %v, want f00000000001", id, got[id]["device"])
		}
	}
	for _, id := range []string{"other", "dup", "self", "rival"} {
		if asText(got[id]["device"]) != id {
			t.Errorf("%s device = %v, want itself", id, got[id]["device"])
		}
	}
}

// TestHistoryViewsAreSnapOnly: boot, device and the GC envelope need history
// the live binding deliberately does not hold (exchanges capped at 24 h, no
// events); offered there they would answer from a truncated slice.
func TestHistoryViewsAreSnapOnly(t *testing.T) {
	g, err := OpenLive(storefixture.Live(t, "history"), descriptors(t))
	if err != nil {
		t.Fatal(err)
	}
	defer g.Close()
	names, _ := g.Tables()
	for _, v := range []string{"boot", "gc_cycle", "gc_envelope", "role", "device", "device_conflict"} {
		if contains(names, v) {
			t.Errorf("live offers %q", v)
		}
	}
	if !strings.Contains(strings.Join(names, ","), "goal_name") {
		t.Errorf("live lacks the goal_name scalar table: %v", names)
	}
}
