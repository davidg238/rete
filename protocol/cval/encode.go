// Copyright (c) 2026 Ekorau LLC

// internal/cval/encode.go
package cval

import (
	"bytes"
	"math"
	"sort"
)

// Encode produces deterministic CBOR (RFC 8949 §4.2): shortest-form heads,
// definite lengths only, map keys sorted bytewise over their ENCODED bytes.
// Byte-identical to the node's Cbor>>encode:.
func Encode(v any) ([]byte, error) {
	var buf bytes.Buffer
	if err := enc(&buf, v, 1); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func head(w *bytes.Buffer, major byte, arg uint64) {
	mt := major << 5
	switch {
	case arg < 24:
		w.WriteByte(mt | byte(arg))
	case arg < 1<<8:
		w.WriteByte(mt | 24)
		w.WriteByte(byte(arg))
	case arg < 1<<16:
		w.WriteByte(mt | 25)
		w.Write([]byte{byte(arg >> 8), byte(arg)})
	case arg < 1<<32:
		w.WriteByte(mt | 26)
		w.Write([]byte{byte(arg >> 24), byte(arg >> 16), byte(arg >> 8), byte(arg)})
	default:
		w.WriteByte(mt | 27)
		for i := 56; i >= 0; i -= 8 {
			w.WriteByte(byte(arg >> uint(i)))
		}
	}
}

func enc(w *bytes.Buffer, v any, depth int) error {
	if depth > maxDepth {
		return ErrDepth
	}
	switch x := v.(type) {
	case nil:
		w.WriteByte(0xf6)
	case bool:
		if x {
			w.WriteByte(0xf5)
		} else {
			w.WriteByte(0xf4)
		}
	case int64:
		if x >= 0 {
			head(w, 0, uint64(x))
		} else {
			head(w, 1, uint64(-1-x))
		}
	case Uint:
		head(w, 0, uint64(x))
	case NegUint:
		head(w, 1, uint64(x))
	case float64:
		// The node emits shortest-form floats via a C leaf; f64 is always
		// legal and always round-trips. Narrowing is a later optimisation
		// and must never change a value.
		w.WriteByte(0xfb)
		bits := math.Float64bits(x)
		for i := 56; i >= 0; i -= 8 {
			w.WriteByte(byte(bits >> uint(i)))
		}
	case string:
		head(w, 3, uint64(len(x)))
		w.WriteString(x)
	case Bytes:
		head(w, 2, uint64(len(x)))
		w.Write(x)
	case Symbol:
		head(w, 6, 39)
		head(w, 3, uint64(len(x)))
		w.WriteString(string(x))
	case List:
		head(w, 4, uint64(len(x)))
		for _, e := range x {
			if err := enc(w, e, depth+1); err != nil {
				return err
			}
		}
	case Map:
		return encMap(w, x, depth)
	case Record:
		head(w, 6, 27)
		head(w, 4, 2)
		if err := enc(w, x.Tag, depth+1); err != nil {
			return err
		}
		return enc(w, x.Fields, depth+1)
	default:
		return ErrNotAValue
	}
	return nil
}

func encMap(w *bytes.Buffer, m Map, depth int) error {
	type kv struct{ k, v []byte }
	pairs := make([]kv, 0, len(m))
	for _, p := range m {
		var kb, vb bytes.Buffer
		if err := enc(&kb, p.K, depth+1); err != nil {
			return err
		}
		if err := enc(&vb, p.V, depth+1); err != nil {
			return err
		}
		pairs = append(pairs, kv{kb.Bytes(), vb.Bytes()})
	}
	// RFC 8949 §4.2: bytewise over the ENCODED key bytes. This is NOT the
	// Map's own iteration order — they diverge whenever keys differ in
	// encoded length.
	sort.SliceStable(pairs, func(i, j int) bool { return bytes.Compare(pairs[i].k, pairs[j].k) < 0 })
	head(w, 5, uint64(len(pairs)))
	for _, p := range pairs {
		w.Write(p.k)
		w.Write(p.v)
	}
	return nil
}
