// Copyright (c) 2026 Ekorau LLC

package custos

import (
	"bytes"
	"encoding/json"
	"path/filepath"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/space"
)

func snapshotOfStore(t *testing.T, store string) string {
	t.Helper()
	st, err := space.Open(store, func() int64 { return 0 })
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()
	snap := filepath.Join(t.TempDir(), "snap.db")
	if err := st.SnapshotInto(snap); err != nil {
		t.Fatal(err)
	}
	return snap
}

func runCLI(t *testing.T, args ...string) (string, error) {
	t.Helper()
	var out bytes.Buffer
	err := RunCLI(args, &out)
	return out.String(), err
}

// TestSQLAnswersWithReadingRules: `custos sql` is the gate's answer as JSON,
// notes included, on whichever binding the flags name.
func TestSQLAnswersWithReadingRules(t *testing.T) {
	store := fleetStore(t)
	for _, binding := range [][]string{
		{"-store", store},
		{"-snapshot", snapshotOfStore(t, store)},
	} {
		args := append([]string{"sql", "-descriptors", repoDescriptors(t)}, binding...)
		out, err := runCLI(t, append(args, `SELECT node_id, value FROM goal_name`)...)
		if err != nil {
			t.Fatalf("%v: %v", binding, err)
		}
		var a struct {
			Binding string `json:"binding"`
			Rows    [][]any
			Notes   []struct{ Column string }
		}
		if err := json.Unmarshal([]byte(out), &a); err != nil {
			t.Fatalf("%v: %v\n%s", binding, err, out)
		}
		if len(a.Rows) != 1 || a.Rows[0][1] != "porch" || len(a.Notes) == 0 {
			t.Errorf("%v: %s", binding, out)
		}
	}
}

// TestSQLNeedsExactlyOneSource: a query that does not say what it reads is an
// error, never a default — snap and live answer different questions.
func TestSQLNeedsExactlyOneSource(t *testing.T) {
	store := fleetStore(t)
	for _, args := range [][]string{
		{"sql", "-descriptors", repoDescriptors(t), "SELECT 1"},
		{"sql", "-descriptors", repoDescriptors(t), "-store", store, "-snapshot", store, "SELECT 1"},
	} {
		if _, err := runCLI(t, args...); err == nil {
			t.Errorf("%v answered", args)
		}
	}
}

func TestTablesAndDescribe(t *testing.T) {
	store := fleetStore(t)
	out, err := runCLI(t, "tables", "-descriptors", repoDescriptors(t), "-store", store)
	if err != nil || !strings.Contains(out, "liveness\n") || !strings.Contains(out, "goal_name\n") {
		t.Errorf("tables: %v\n%s", err, out)
	}
	out, err = runCLI(t, "describe", "-descriptors", repoDescriptors(t), "lfb")
	if err != nil || !strings.Contains(out, "largestFreeBlock") || !strings.Contains(out, "rete#153") {
		t.Errorf("describe lfb: %v\n%s", err, out)
	}
}
