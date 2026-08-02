// Copyright (c) 2026 Ekorau LLC

// Package cval implements the CBOR encoding of the nsl message algebra,
// byte-compatible with the node's shipped codec (nsl-tuvm/lib/kernel.nsl,
// class Cbor). It is hand-rolled rather than layered on a general CBOR
// library for two reasons: the algebra's map keys are themselves tagged
// values (Symbols, tag 39), and decoding must be CLOSED — an unknown tag is
// an error, never a pass-through, so a mis-versioned peer cannot smuggle a
// value the node would faithfully relay.
package cval

import "errors"

// Kind mirrors SPACE_KIND_* in tuvm/src/space.h. The numbers are shared with
// the VM and travel on the wire; do not renumber them.
type Kind int

const (
	KindUnset  Kind = 0
	KindStream Kind = 1
	KindCell   Kind = 2
)

// The nine ranks of the algebra, as Go types:
//
//	nil, bool, int64/Uint/NegUint, float64, string, Bytes, Symbol, List, Map, Record
type (
	Bytes  []byte
	Symbol string
	List   []any
	// Map is an ordered slice, not a Go map: keys are arbitrary algebra
	// values (commonly Symbols) and Go maps cannot key on them.
	Map []Pair
	// Uint is a positive integer too large for int64 (up to 2^64−1).
	Uint uint64
	// NegUint is the negative integer −1−n, for values below math.MinInt64.
	NegUint uint64
)

type Pair struct{ K, V any }

type Record struct {
	Tag    Symbol
	Fields Map
}

// The error vocabulary mirrors the node's four decode failures exactly.
var (
	ErrNotAValue = errors.New("not a value")
	ErrDepth     = errors.New("message depth exceeded")
	ErrUTF8      = errors.New("invalid utf-8")
	ErrIntRange  = errors.New("integer out of range")
	ErrTrailing  = errors.New("trailing bytes")
)

const maxDepth = 32
