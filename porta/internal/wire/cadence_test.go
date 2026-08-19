// Copyright (c) 2026 Ekorau LLC

// internal/wire/cadence_test.go
package wire

import (
	"testing"

	"github.com/davidg238/porta/internal/space"
)

// THE WIRE-SHAPE REGRESSION FOR rete#217 C1. A real check-in is a pull
// followed by a push — handlePull and handlePush each call
// space.Store.RecordExchange once (server.go:166,206) — so this drives N
// real check-ins through the ACTUAL Handle path (not through RecordExchange
// directly, which is what let the bug survive six task reviews: the old
// `checkIn` test helper in internal/space/store_test.go called RecordExchange
// once per simulated check-in, modelling a protocol the gateway does not
// implement) and asserts CadenceMs answers the true period, not half of it.
func TestCadenceIsInferredFromRealCheckIns(t *testing.T) {
	var clock int64 = 100_000
	st, err := space.Open(t.TempDir()+"/t.db", func() int64 { return clock })
	if err != nil {
		t.Fatalf("space.Open: %v", err)
	}
	defer st.Close()
	srv := NewServer(st, func(string, ...any) {})

	const period = 2_000
	const checkIns = 6
	var cursor, hw int64
	for i := 0; i < checkIns; i++ {
		clock += period

		pull, err := EncodePull("n1", cursor, 7, hw)
		if err != nil {
			t.Fatalf("check-in %d: EncodePull: %v", i, err)
		}
		hi, _, err := DecodeBatch(srv.Handle(pull))
		if err != nil {
			t.Fatalf("check-in %d: pull DecodeBatch: %v", i, err)
		}
		cursor = hi

		push, err := EncodePush("n1", 7, nil)
		if err != nil {
			t.Fatalf("check-in %d: EncodePush: %v", i, err)
		}
		newHW, err := DecodeAck(srv.Handle(push))
		if err != nil {
			t.Fatalf("check-in %d: push DecodeAck: %v", i, err)
		}
		hw = newHW
	}

	cadence, ok, err := st.CadenceMs("n1")
	if err != nil {
		t.Fatalf("CadenceMs: %v", err)
	}
	if !ok {
		t.Fatalf("CadenceMs ok = false after %d real check-ins at a %d ms period; want a verdict", checkIns, period)
	}
	if cadence != period {
		t.Fatalf("CadenceMs = %d ms, want %d ms — a check-in is a pull AND a push, two exchange rows, "+
			"so a median over every row (rather than the pull leg alone) reads exactly half the node's real period",
			cadence, period)
	}
}
