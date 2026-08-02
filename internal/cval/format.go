// Copyright (c) 2026 Ekorau LLC

// internal/cval/format.go
package cval

import (
	"fmt"
	"math/big"
	"strings"
)

// Format renders a value for operator display. It is NEVER a storage format:
// the CBOR bytes are the source of truth (design D4.1), so this may be lossy
// for display purposes without consequence.
func Format(v any) string {
	switch x := v.(type) {
	case nil:
		return "nil"
	case bool:
		return fmt.Sprintf("%t", x)
	case int64:
		return fmt.Sprintf("%d", x)
	case Uint:
		return fmt.Sprintf("%d", uint64(x))
	case NegUint:
		// NegUint(n) is −1−n, so the printed magnitude is n+1 — which
		// overflows uint64 at exactly the band edge NegUint exists to carry
		// (NegUint(2^64−1) is −2^64). Computing this in uint64 would print
		// "-0" for the one value most worth printing correctly.
		m := new(big.Int).SetUint64(uint64(x))
		return new(big.Int).Neg(m.Add(m, big.NewInt(1))).String()
	case float64:
		return fmt.Sprintf("%g", x)
	case string:
		return fmt.Sprintf("%q", x)
	case Bytes:
		return fmt.Sprintf("0x%x", []byte(x))
	case Symbol:
		return "#" + string(x)
	case List:
		parts := make([]string, len(x))
		for i, e := range x {
			parts[i] = Format(e)
		}
		return "[" + strings.Join(parts, " ") + "]"
	case Map:
		parts := make([]string, len(x))
		for i, p := range x {
			parts[i] = Format(p.K) + ": " + Format(p.V)
		}
		return "{" + strings.Join(parts, ", ") + "}"
	case Record:
		return "#" + string(x.Tag) + Format(x.Fields)
	}
	return "<?>"
}
