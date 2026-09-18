// Copyright (c) 2026 Ekorau LLC

// internal/space/runs_test.go
package space

import "testing"

// The gap BETWEEN two runs is a deaf interval, and a window that touches one is
// a window porta cannot testify about.
func TestWasDeafSpansTheGapBetweenRuns(t *testing.T) {
	var clock int64 = 1_000
	st := storeAtClock(t, &clock)

	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun: %v", err)
	}
	clock = 100_000
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}

	// porta is killed here and comes back an hour later.
	clock = 3_700_000
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun (second): %v", err)
	}
	clock = 3_730_000
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive (second): %v", err)
	}

	for _, tc := range []struct {
		name     string
		from, to int64
		want     bool
	}{
		{"wholly inside the first run", 2_000, 90_000, false},
		{"wholly inside the second run", 3_705_000, 3_720_000, false},
		{"spanning the outage", 90_000, 3_720_000, true},
		{"starting inside the outage", 200_000, 3_720_000, true},
		{"ending inside the outage", 90_000, 200_000, true},
		{"wholly inside the outage", 200_000, 300_000, true},
		{"before any recorded run", 0, 500, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := st.WasDeaf(tc.from, tc.to)
			if err != nil {
				t.Fatalf("WasDeaf: %v", err)
			}
			if got != tc.want {
				t.Fatalf("WasDeaf(%d, %d) = %v, want %v", tc.from, tc.to, got, tc.want)
			}
		})
	}
}

// #225's exact scenario: a node checking in every 30 s with a 4 min budget, and a
// gateway down for ten hours. The verdict across that window must NOT be #silent —
// porta has no evidence in either direction — and it must not stay #live either,
// which is what actually happened on 2026-08-19.
func TestSweepAcrossAGatewayOutageIsUnknownAndNotSilent(t *testing.T) {
	var clock int64 = 10_000
	st := storeAtClock(t, &clock)
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun: %v", err)
	}
	checkIn(t, st, &clock, "bell", 0)
	declare(t, st, "bell", 30_000, 8)
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}
	if _, err := st.Sweep(); err != nil {
		t.Fatalf("Sweep: %v", err)
	}
	if got := livenessOf(t, st, "bell"); got != "live" {
		t.Fatalf("before the outage: verdict = %q, want live", got)
	}

	// porta is down for ten hours and six minutes, then restarts.
	clock += 10*3_600_000 + 6*60_000
	if err := st.StartRun(); err != nil {
		t.Fatalf("StartRun (after the outage): %v", err)
	}
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}
	changes, err := st.Sweep()
	if err != nil {
		t.Fatalf("Sweep after the outage: %v", err)
	}
	if len(changes) != 1 || changes[0].Verdict != "unknown" {
		t.Fatalf("changes = %+v, want one #unknown — porta was deaf for the whole window", changes)
	}

	// The node checks back in: evidence of speech outranks everything.
	checkIn(t, st, &clock, "bell", 1_000)
	if err := st.MarkAlive(); err != nil {
		t.Fatalf("MarkAlive: %v", err)
	}
	if changes, err = st.Sweep(); err != nil {
		t.Fatalf("Sweep after the check-in: %v", err)
	} else if len(changes) != 1 || changes[0].Verdict != "live" {
		t.Fatalf("changes = %+v, want #live once the node speaks again", changes)
	}
}
