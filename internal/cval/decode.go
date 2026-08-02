// Copyright (c) 2026 Ekorau LLC

// internal/cval/decode.go
package cval

import (
	"encoding/binary"
	"math"
	"unicode/utf8"
)

// Decode parses exactly one value. It is CLOSED: any major or tag outside the
// algebra is ErrNotAValue, indefinite lengths are ErrNotAValue, and trailing
// bytes after the first complete value are ErrTrailing.
func Decode(b []byte) (any, error) {
	v, n, err := dec(b, 1)
	if err != nil {
		return nil, err
	}
	if n != len(b) {
		return nil, ErrTrailing
	}
	return v, nil
}

// Validate reports whether b is exactly one well-formed value.
func Validate(b []byte) error {
	_, err := Decode(b)
	return err
}

// readHead returns (major, argument, bytes consumed).
func readHead(b []byte) (byte, uint64, int, error) {
	if len(b) == 0 {
		return 0, 0, 0, ErrNotAValue
	}
	major := b[0] >> 5
	ai := b[0] & 0x1f
	switch {
	case ai < 24:
		return major, uint64(ai), 1, nil
	case ai == 24:
		if len(b) < 2 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, uint64(b[1]), 2, nil
	case ai == 25:
		if len(b) < 3 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, uint64(binary.BigEndian.Uint16(b[1:])), 3, nil
	case ai == 26:
		if len(b) < 5 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, uint64(binary.BigEndian.Uint32(b[1:])), 5, nil
	case ai == 27:
		if len(b) < 9 {
			return 0, 0, 0, ErrNotAValue
		}
		return major, binary.BigEndian.Uint64(b[1:]), 9, nil
	default:
		// 28–30 reserved, 31 indefinite — both closed out.
		return 0, 0, 0, ErrNotAValue
	}
}

func dec(b []byte, depth int) (any, int, error) {
	if depth > maxDepth {
		return nil, 0, ErrDepth
	}
	major, arg, n, err := readHead(b)
	if err != nil {
		return nil, 0, err
	}
	switch major {
	case 0:
		if arg <= math.MaxInt64 {
			return int64(arg), n, nil
		}
		return Uint(arg), n, nil
	case 1:
		if arg < 1<<63 {
			return -1 - int64(arg), n, nil
		}
		return NegUint(arg), n, nil
	case 2:
		if uint64(len(b)-n) < arg {
			return nil, 0, ErrNotAValue
		}
		out := make(Bytes, arg)
		copy(out, b[n:uint64(n)+arg])
		return out, n + int(arg), nil
	case 3:
		if uint64(len(b)-n) < arg {
			return nil, 0, ErrNotAValue
		}
		s := b[n : uint64(n)+arg]
		if !utf8.Valid(s) {
			return nil, 0, ErrUTF8
		}
		return string(s), n + int(arg), nil
	case 4:
		out := make(List, 0, arg)
		off := n
		for i := uint64(0); i < arg; i++ {
			e, m, err := dec(b[off:], depth+1)
			if err != nil {
				return nil, 0, err
			}
			out = append(out, e)
			off += m
		}
		return out, off, nil
	case 5:
		out := make(Map, 0, arg)
		off := n
		for i := uint64(0); i < arg; i++ {
			k, m, err := dec(b[off:], depth+1)
			if err != nil {
				return nil, 0, err
			}
			off += m
			v, m2, err := dec(b[off:], depth+1)
			if err != nil {
				return nil, 0, err
			}
			off += m2
			out = append(out, Pair{k, v})
		}
		return out, off, nil
	case 6:
		return decTag(b, arg, n, depth)
	case 7:
		switch b[0] {
		case 0xf4:
			return false, 1, nil
		case 0xf5:
			return true, 1, nil
		case 0xf6:
			return nil, 1, nil
		case 0xfb:
			if len(b) < 9 {
				return nil, 0, ErrNotAValue
			}
			return math.Float64frombits(binary.BigEndian.Uint64(b[1:])), 9, nil
		}
		return nil, 0, ErrNotAValue
	}
	return nil, 0, ErrNotAValue
}

// decTag handles the only two tags in the algebra: 39 (Symbol) and 27
// (Record). Every other tag is ErrNotAValue — that is what "closed" means.
func decTag(b []byte, tag uint64, n, depth int) (any, int, error) {
	switch tag {
	case 39:
		v, m, err := dec(b[n:], depth+1)
		if err != nil {
			return nil, 0, err
		}
		s, ok := v.(string)
		if !ok {
			return nil, 0, ErrNotAValue
		}
		return Symbol(s), n + m, nil
	case 27:
		v, m, err := dec(b[n:], depth+1)
		if err != nil {
			return nil, 0, err
		}
		l, ok := v.(List)
		if !ok || len(l) != 2 {
			return nil, 0, ErrNotAValue
		}
		// The record NAME is a Symbol, not a text string.
		name, ok := l[0].(Symbol)
		if !ok {
			return nil, 0, ErrNotAValue
		}
		fields, ok := l[1].(Map)
		if !ok {
			return nil, 0, ErrNotAValue
		}
		return Record{Tag: name, Fields: fields}, n + m, nil
	}
	return nil, 0, ErrNotAValue
}
