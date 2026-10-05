// Copyright (c) 2026 Ekorau LLC

// Package wire is the node-facing space-sync exchange: two node-initiated
// CBOR datagrams over plain UDP.
//
// There is no retransmit layer and that is deliberate. The exchange is
// idempotent by cursor in both directions — a lost pull response means the
// next check-in re-pulls the same range, and a lost push is resent and
// deduped on the node-seq high-water. Loss is absorbed by convergence, not
// by retransmission (design D1).
package wire

import (
	"errors"
	"fmt"

	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/rete/protocol/cval"
)

var errShape = errors.New("malformed message")

func field(m cval.Map, name string) (any, bool) {
	for _, p := range m {
		if k, ok := p.K.(cval.Symbol); ok && string(k) == name {
			return p.V, true
		}
	}
	return nil, false
}

func intField(m cval.Map, name string) (int64, error) {
	v, ok := field(m, name)
	if !ok {
		return 0, fmt.Errorf("%w: missing #%s", errShape, name)
	}
	n, ok := v.(int64)
	if !ok {
		return 0, fmt.Errorf("%w: #%s is not an integer", errShape, name)
	}
	return n, nil
}

func strField(m cval.Map, name string) (string, error) {
	v, ok := field(m, name)
	if !ok {
		return "", fmt.Errorf("%w: missing #%s", errShape, name)
	}
	s, ok := v.(string)
	if !ok {
		return "", fmt.Errorf("%w: #%s is not a string", errShape, name)
	}
	return s, nil
}

// entriesToList renders [[seq, path, kind, value], …]. The value rides as an
// already-encoded value, so it is decoded here only to nest it structurally.
func entriesToList(entries []space.Entry) (cval.List, error) {
	out := make(cval.List, 0, len(entries))
	for _, e := range entries {
		v, err := cval.Decode(e.Value)
		if err != nil {
			return nil, fmt.Errorf("stored value at %s is not decodable: %w", e.Path, err)
		}
		out = append(out, cval.List{e.Seq, e.Path, int64(e.Kind), v})
	}
	return out, nil
}

func listToEntries(v any) ([]space.Entry, error) {
	l, ok := v.(cval.List)
	if !ok {
		return nil, fmt.Errorf("%w: #ent is not a list", errShape)
	}
	out := make([]space.Entry, 0, len(l))
	for _, item := range l {
		row, ok := item.(cval.List)
		if !ok || len(row) != 4 {
			return nil, fmt.Errorf("%w: an entry is not a 4-element list", errShape)
		}
		seq, ok1 := row[0].(int64)
		path, ok2 := row[1].(string)
		kind, ok3 := row[2].(int64)
		if !ok1 || !ok2 || !ok3 {
			return nil, fmt.Errorf("%w: entry header types", errShape)
		}
		raw, err := cval.Encode(row[3])
		if err != nil {
			return nil, err
		}
		out = append(out, space.Entry{Seq: seq, Path: path, Kind: cval.Kind(kind), Value: raw})
	}
	return out, nil
}

func EncodePull(id string, cursor, bootID, hw, wak int64) ([]byte, error) {
	return cval.Encode(cval.Record{Tag: "pull", Fields: cval.Map{
		{K: cval.Symbol("id"), V: id},
		{K: cval.Symbol("cursor"), V: cursor},
		{K: cval.Symbol("bid"), V: bootID},
		{K: cval.Symbol("hw"), V: hw},
		{K: cval.Symbol("wak"), V: wak},
	}})
}

func EncodePush(id string, bootID int64, entries []space.Entry) ([]byte, error) {
	l, err := entriesToList(entries)
	if err != nil {
		return nil, err
	}
	return cval.Encode(cval.Record{Tag: "push", Fields: cval.Map{
		{K: cval.Symbol("id"), V: id},
		{K: cval.Symbol("bid"), V: bootID},
		{K: cval.Symbol("ent"), V: l},
	}})
}

func encodeBatch(hi int64, entries []space.Entry) ([]byte, error) {
	l, err := entriesToList(entries)
	if err != nil {
		return nil, err
	}
	return cval.Encode(cval.Record{Tag: "batch", Fields: cval.Map{
		{K: cval.Symbol("hi"), V: hi},
		{K: cval.Symbol("ent"), V: l},
	}})
}

// encodeBatchCapped builds the reply and guarantees the DATAGRAM fits the cap,
// not merely the payloads inside it.
//
// space.OutboundSince budgets each entry's value and path with a fixed 8 bytes
// of slack, which does not cover the #batch record wrapper, the per-entry list
// heads, or a seq head that grows to nine bytes once seqs get large. The cap
// is a 6LoWPAN constraint (RECORDS §2.1/§2.2) — a fragmented datagram survives
// only if every fragment does — so an overrun is lost packets on the mesh.
//
// Trimming drops entries from the TAIL and lowers hi with them: hi must name
// what actually rode, or the node advances its cursor past writes it never
// saw. The dropped entries are not lost, they ride the next check-in.
func encodeBatchCapped(hi int64, entries []space.Entry, maxBytes int) ([]byte, error) {
	for {
		b, err := encodeBatch(hi, entries)
		if err != nil {
			return nil, err
		}
		if len(b) <= maxBytes || len(entries) == 0 {
			return b, nil
		}
		entries = entries[:len(entries)-1]
		if len(entries) == 0 {
			// Even one entry did not fit. Answer an honest empty batch at the
			// caller's cursor rather than a datagram the mesh will drop; the
			// 256 B value cap makes this unreachable in practice, and if it
			// ever fires the value cap has been breached upstream.
			hi = 0
		} else {
			hi = entries[len(entries)-1].Seq
		}
	}
}

func encodeAck(hw int64) ([]byte, error) {
	// #hint (the §4.4 back-pressure hint) is reserved and deliberately not
	// emitted, so the shape does not move when it lands.
	return cval.Encode(cval.Record{Tag: "ack", Fields: cval.Map{{K: cval.Symbol("hw"), V: hw}}})
}

func encodeErr(msg string) []byte {
	b, err := cval.Encode(cval.Record{Tag: "err", Fields: cval.Map{{K: cval.Symbol("msg"), V: msg}}})
	if err != nil {
		// An unencodable error record would leave a node with no reply at
		// all; a constant is always encodable.
		b, _ = cval.Encode(cval.Record{Tag: "err", Fields: cval.Map{{K: cval.Symbol("msg"), V: "internal"}}})
	}
	return b
}

func DecodeBatch(b []byte) (int64, []space.Entry, error) {
	v, err := cval.Decode(b)
	if err != nil {
		return 0, nil, err
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "batch" {
		return 0, nil, fmt.Errorf("%w: not a #batch (%s)", errShape, cval.Format(v))
	}
	hi, err := intField(r.Fields, "hi")
	if err != nil {
		return 0, nil, err
	}
	ev, ok := field(r.Fields, "ent")
	if !ok {
		return 0, nil, fmt.Errorf("%w: missing #ent", errShape)
	}
	entries, err := listToEntries(ev)
	return hi, entries, err
}

func DecodeAck(b []byte) (int64, error) {
	v, err := cval.Decode(b)
	if err != nil {
		return 0, err
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "ack" {
		return 0, fmt.Errorf("%w: not an #ack (%s)", errShape, cval.Format(v))
	}
	return intField(r.Fields, "hw")
}
