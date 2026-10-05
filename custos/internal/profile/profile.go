// Copyright (c) 2026 Ekorau LLC

// Package profile turns a node's per-method profile dump into named, ranked
// rows (#164, #312). The VM reports a method as a blob-relative byte offset,
// because a CompiledMethod carries no name; the image map
// (protocol/IMAGE-ARTIFACTS.md) is the other half of the join.
//
// It refuses to guess. An offset absent from the map is shown as UNKNOWN with
// the offset intact, because a profile that silently drops the rows it cannot
// name reads as complete while hiding its largest caller. A non-zero overflow
// is INCOMPLETE, not invalid: the slotted counts are exact, but methods are
// missing and the percentages are lower bounds.
package profile

import (
	"fmt"
	"io"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

// The dump's four line shapes (vm.c mprof_report). `mprofa:` is a separate
// prefix rather than extra columns on `mprof:`: the row pattern is anchored at
// exactly two fields, so widening that line would make it match nothing — a
// silent false negative. TestTheActivationRowsStillMatchBesideAllocRows guards it.
var (
	rowRE      = regexp.MustCompile(`(?m)mprof:\s+(@?\S+)\s+(\d+)\s*$`)
	hdrRE      = regexp.MustCompile(`mprof\[(\w+)\]:\s+methods=(\d+)\s+activations=(\d+)\s+overflow=(\d+)\s+cap=(\d+)`)
	allocRowRE = regexp.MustCompile(`(?m)mprofa:\s+(@?\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$`)
	allocHdrRE = regexp.MustCompile(`mprofa\[(\w+)\]:\s+noframe_arena=(\d+)/(\d+)\s+noframe_cheap=(\d+)/(\d+)\s+allocOverflow=(\d+)`)
)

// Header is the window's `mprof[<why>]:` line.
type Header struct {
	Window                              string
	Methods, Activations, Overflow, Cap int
}

// AllocHeader is the window's `mprofa[<why>]:` line.
type AllocHeader struct {
	Window                                                     string
	NoFrameArenaB, NoFrameArenaN, NoFrameCheapB, NoFrameCheapN int
	AllocOverflow                                              int
}

// Row is one `mprof:` line. Key is a decimal blob offset, or "@0x…" for a
// method outside the image blob.
type Row struct {
	Key   string
	Count int
}

// AllocRow is one `mprofa:` line.
type AllocRow struct {
	Key                            string
	ArenaB, ArenaN, CheapB, CheapN int
}

// Dump is everything profile lines in a text say. A header is the last one in
// the text: a log holding two windows is caught by Warnings, not merged.
type Dump struct {
	Header    *Header
	Rows      []Row
	Alloc     *AllocHeader
	AllocRows []AllocRow
}

func atoi(s string) int { n, _ := strconv.Atoi(s); return n }

// Parse reads the profile lines out of any text — a console log is fine, the
// surrounding traffic is ignored.
func Parse(text string) Dump {
	var d Dump
	for _, m := range hdrRE.FindAllStringSubmatch(text, -1) {
		d.Header = &Header{m[1], atoi(m[2]), atoi(m[3]), atoi(m[4]), atoi(m[5])}
	}
	for _, m := range rowRE.FindAllStringSubmatch(text, -1) {
		d.Rows = append(d.Rows, Row{m[1], atoi(m[2])})
	}
	for _, m := range allocHdrRE.FindAllStringSubmatch(text, -1) {
		d.Alloc = &AllocHeader{m[1], atoi(m[2]), atoi(m[3]), atoi(m[4]), atoi(m[5]), atoi(m[6])}
	}
	for _, m := range allocRowRE.FindAllStringSubmatch(text, -1) {
		d.AllocRows = append(d.AllocRows, AllocRow{m[1], atoi(m[2]), atoi(m[3]), atoi(m[4]), atoi(m[5])})
	}
	return d
}

// name resolves a row's key, or says why it cannot.
func name(m *imagemap.Map, key string) (string, bool) {
	if strings.HasPrefix(key, "@") {
		return "UNKNOWN (not in the image blob: " + key + ")", true
	}
	// Exact: a key must be the decimal the VM prints. "07" or "12abc" is not a
	// spelling of an offset, it is a garbled line, and resolving it would name
	// the wrong method.
	if off, err := strconv.Atoi(key); err == nil && strconv.Itoa(off) == key {
		if meth, ok := m.Methods[off]; ok {
			return meth.Name, false
		}
	}
	return "UNKNOWN (offset " + key + " absent from the map — wrong build?)", true
}

// Ranked is one method's share of the window's activations.
type Ranked struct {
	Name    string
	Count   int
	Pct     float64
	Unknown bool
}

// Rank answers every row, by count descending. The denominator is the slotted
// rows plus overflow, not the slotted sum alone: a saturated table drops
// activations it counts only in aggregate, and leaving them out would inflate
// every percentage. With them in, each percentage is a true lower bound.
func (d Dump) Rank(m *imagemap.Map) []Ranked {
	total := 0
	for _, r := range d.Rows {
		total += r.Count
	}
	if d.Header != nil {
		total += d.Header.Overflow
	}
	if total == 0 {
		total = 1
	}
	rows := append([]Row(nil), d.Rows...)
	sort.SliceStable(rows, func(i, j int) bool { return rows[i].Count > rows[j].Count })
	out := make([]Ranked, 0, len(rows))
	for _, r := range rows {
		n, unk := name(m, r.Key)
		out = append(out, Ranked{n, r.Count, float64(r.Count) / float64(total) * 100, unk})
	}
	return out
}

// RankedAlloc is one method's share of the window's allocated bytes.
type RankedAlloc struct {
	Name                           string
	ArenaB, ArenaN, CheapB, CheapN int
	Pct                            float64
	Unknown                        bool
}

func (d Dump) noFrameBytes() int {
	if d.Alloc == nil {
		return 0
	}
	return d.Alloc.NoFrameArenaB + d.Alloc.NoFrameCheapB
}

// RankAlloc answers every allocation row, by total bytes descending. The
// denominator is the attributed bytes plus the no-frame bucket, so a run that
// allocates mostly outside any frame cannot read as "these few methods account
// for everything".
func (d Dump) RankAlloc(m *imagemap.Map) []RankedAlloc {
	total := d.noFrameBytes()
	for _, r := range d.AllocRows {
		total += r.ArenaB + r.CheapB
	}
	if total == 0 {
		total = 1
	}
	rows := append([]AllocRow(nil), d.AllocRows...)
	sort.SliceStable(rows, func(i, j int) bool {
		return rows[i].ArenaB+rows[i].CheapB > rows[j].ArenaB+rows[j].CheapB
	})
	out := make([]RankedAlloc, 0, len(rows))
	for _, r := range rows {
		n, unk := name(m, r.Key)
		out = append(out, RankedAlloc{n, r.ArenaB, r.ArenaN, r.CheapB, r.CheapN,
			float64(r.ArenaB+r.CheapB) / float64(total) * 100, unk})
	}
	return out
}

// Warnings are what makes the activation table honest: a saturated table, and
// a log whose rows do not match its header (truncated, or two windows).
func (d Dump) Warnings() []string {
	h := d.Header
	if h == nil {
		return nil
	}
	var w []string
	if h.Overflow != 0 {
		w = append(w, fmt.Sprintf("INCOMPLETE: overflow=%d activations were dropped — the table saturated, "+
			"so methods are MISSING from this table. Counts below are exact; percentages are lower bounds.", h.Overflow))
	}
	if len(d.Rows) != h.Methods {
		w = append(w, fmt.Sprintf("WARNING: %d rows for methods=%d — the log is truncated or holds more than one window.",
			len(d.Rows), h.Methods))
	}
	sum := 0
	for _, r := range d.Rows {
		sum += r.Count
	}
	if sum != h.Activations {
		w = append(w, fmt.Sprintf("WARNING: rows sum to %d, header says %d — mixed windows.", sum, h.Activations))
	}
	return w
}

// Write prints the report: the header and its warnings, the ranked table, and
// with alloc the allocation table. top limits the rows shown, never a
// denominator or a count.
func Write(w io.Writer, d Dump, m *imagemap.Map, top int, alloc bool) {
	if h := d.Header; h != nil {
		fmt.Fprintf(w, "window=%s methods=%d activations=%d cap=%d\n", h.Window, h.Methods, h.Activations, h.Cap)
	}
	for _, s := range d.Warnings() {
		fmt.Fprintln(w, s)
	}
	ranked := d.Rank(m)
	unknown := 0
	for _, r := range ranked {
		if r.Unknown {
			unknown++
		}
	}
	shown := ranked
	if top > 0 && len(shown) > top {
		shown = shown[:top]
	}
	fmt.Fprintf(w, "\n%12s  %6s  method\n", "count", "%")
	for _, r := range shown {
		fmt.Fprintf(w, "%12d  %5.1f%%  %s\n", r.Count, r.Pct, r.Name)
	}
	if top > 0 {
		fmt.Fprintf(w, "\nshowing top %d of %d rows — percentages are of ALL activations, not of the shown rows.\n",
			top, len(ranked))
	}
	if unknown > 0 {
		fmt.Fprintf(w, "\n%d row(s) unresolved — a map from a DIFFERENT build resolves nothing and looks "+
			"identical to a quiet program.\n", unknown)
	}
	if alloc {
		writeAlloc(w, d, m, top)
	}
}

func writeAlloc(w io.Writer, d Dump, m *imagemap.Map, top int) {
	// All rows, never the shown ones: both numbers below are properties of the
	// window. Truncating them made the distinct-method count read back the
	// display flag, and shrank the no-frame denominator in the direction that
	// hides the risk it measures.
	all := d.RankAlloc(m)
	fmt.Fprintln(w, "\n--- allocation attribution ---")
	if a := d.Alloc; a == nil {
		fmt.Fprintln(w, "no 'mprofa[' summary line found — the window never reported, or the build "+
			"predates allocation attribution.")
	} else {
		attributed := 0
		for _, r := range all {
			attributed += r.ArenaB + r.CheapB
		}
		nf := d.noFrameBytes()
		denom := attributed + nf
		if denom == 0 {
			denom = 1
		}
		fmt.Fprintf(w, "window=%s allocOverflow=%d\n", a.Window, a.AllocOverflow)
		fmt.Fprintf(w, "no-frame bytes: arena=%d (%d allocs) cheap=%d (%d allocs) = %.1f%% of all attributed bytes\n",
			a.NoFrameArenaB, a.NoFrameArenaN, a.NoFrameCheapB, a.NoFrameCheapN, float64(nf)/float64(denom)*100)
		if nf > attributed {
			fmt.Fprintln(w, "WARNING: the no-frame bucket EXCEEDS everything attributed to a method. Most "+
				"allocation is happening outside any frame, so this table is not yet answering 'who allocates'.")
		}
		if a.AllocOverflow != 0 {
			fmt.Fprintf(w, "INCOMPLETE: allocOverflow=%d allocations were dropped — the table saturated, so "+
				"METHODS ARE MISSING. Counts below are exact; percentages are lower bounds.\n", a.AllocOverflow)
		}
	}
	shown := all
	if top > 0 && len(shown) > top {
		shown = shown[:top]
	}
	fmt.Fprintf(w, "\n%10s %8s %10s %8s %6s  method\n", "arenaB", "arenaN", "cheapB", "cheapN", "%")
	for _, r := range shown {
		fmt.Fprintf(w, "%10d %8d %10d %8d %5.1f%%  %s\n", r.ArenaB, r.ArenaN, r.CheapB, r.CheapN, r.Pct, r.Name)
	}
	if top > 0 && len(all) > top {
		fmt.Fprintf(w, "\nshowing top %d of %d allocating methods — percentages are of ALL attributed bytes, "+
			"not of the shown rows.\n", top, len(all))
	}
	fmt.Fprintf(w, "\ndistinct allocating methods: %d   (stage-2 sizing input, design D8)\n", len(all))
}

// Report is the whole analysis as data — what the page renders, and what Write
// prints as text.
type Report struct {
	Header    *Header
	Warnings  []string
	Rows      []Ranked
	Alloc     *AllocHeader
	AllocRows []RankedAlloc
}

func NewReport(d Dump, m *imagemap.Map) Report {
	return Report{d.Header, d.Warnings(), d.Rank(m), d.Alloc, d.RankAlloc(m)}
}
