// Copyright (c) 2026 Ekorau LLC

package profile

import (
	"bytes"
	"reflect"
	"strings"
	"testing"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

// Ported one for one from nodus/oevm/tools/test_mprof_resolve.py (#312 deletes it).

var testMap = &imagemap.Map{Methods: map[int]imagemap.Method{
	16: {Name: "Cbor>>u8At:", File: 1},
	32: {Name: "Cbor>>encode:", File: 1},
}}

// activations=300 counts only the slotted rows; overflow=100 activations were
// dropped. The true denominator is 400, so u8At: is 50%, not 66.7%.
const dump = "mprof[dump]: methods=2 activations=300 overflow=100 cap=256\n" +
	"mprof: 16 200\n" +
	"mprof: 32 100\n"

// A window carrying both line kinds, which is what a real log looks like. The
// no-frame bucket is deliberately non-zero: it is part of the denominator.
const allocDump = "mprof[window]: methods=2 activations=300 overflow=0 cap=256\n" +
	"mprof: 16 200\n" +
	"mprof: 32 100\n" +
	"mprofa[window]: noframe_arena=100/2 noframe_cheap=0/0 allocOverflow=0\n" +
	"mprofa: 16 600 12 0 0\n" +
	"mprofa: 32 200 4 100 2\n"

func report(text string, top int, alloc bool) string {
	var b bytes.Buffer
	Write(&b, Parse(text), testMap, top, alloc)
	return b.String()
}

func TestPercentDenominatorIncludesOverflow(t *testing.T) {
	r := Parse(dump).Rank(testMap)
	if r[0] != (Ranked{"Cbor>>u8At:", 200, 50, false}) || r[1] != (Ranked{"Cbor>>encode:", 100, 25, false}) {
		t.Fatalf("%+v", r)
	}
}

func TestPercentWithoutOverflowUsesActivations(t *testing.T) {
	clean := strings.Replace(dump, "overflow=100", "overflow=0", 1)
	n, total := 200.0, 300.0
	if got := Parse(clean).Rank(testMap)[0].Pct; got != n/total*100 {
		t.Fatalf("%v", got)
	}
}

func TestTopTruncatesButKeepsTheDenominator(t *testing.T) {
	out := report(dump, 1, false)
	if !strings.Contains(out, " 50.0%  Cbor>>u8At:") || strings.Contains(out, "Cbor>>encode:") {
		t.Fatal(out)
	}
	if !strings.Contains(out, "showing top 1 of 2 rows") {
		t.Fatal(out)
	}
}

func TestUnresolvedOffsetKeepsTheOffsetAndIsNotDropped(t *testing.T) {
	r := Parse(dump).Rank(&imagemap.Map{})
	if len(r) != 2 || !r[0].Unknown || !strings.Contains(r[0].Name, "16") || !strings.Contains(r[0].Name, "UNKNOWN") {
		t.Fatalf("%+v", r)
	}
	if out := report(strings.Replace(dump, "mprof: 16", "mprof: 999", 1), 0, false); !strings.Contains(out, "1 row(s) unresolved") {
		t.Fatal(out)
	}
}

func TestMixedWindowsWarningFiresOnSumMismatch(t *testing.T) {
	out := report("mprof[dump]: methods=2 activations=300 overflow=0 cap=256\nmprof: 16 200\nmprof: 32 50\n", 0, false)
	if !strings.Contains(out, "mixed windows") {
		t.Fatal(out)
	}
}

func TestMixedWindowsWarningDoesNotFireOnSaturatedLogWithIntactRows(t *testing.T) {
	out := report(dump, 0, false)
	if strings.Contains(out, "mixed windows") || !strings.Contains(out, "INCOMPLETE: overflow=100") {
		t.Fatal(out)
	}
}

func TestTheActivationRowsStillMatchBesideAllocRows(t *testing.T) {
	if got := Parse(allocDump).Rows; !reflect.DeepEqual(got, []Row{{"16", 200}, {"32", 100}}) {
		t.Fatalf("%+v", got)
	}
}

func TestAllocRowsDoNotSwallowActivationRows(t *testing.T) {
	want := []AllocRow{{"16", 600, 12, 0, 0}, {"32", 200, 4, 100, 2}}
	if got := Parse(allocDump).AllocRows; !reflect.DeepEqual(got, want) {
		t.Fatalf("%+v", got)
	}
}

func TestAllocRowsSortByTotalBytesAndResolveNames(t *testing.T) {
	// u8At: 600; encode: 300. Denominator 900 attributed + 100 no-frame = 1000.
	r := Parse(allocDump).RankAlloc(testMap)
	if r[0] != (RankedAlloc{"Cbor>>u8At:", 600, 12, 0, 0, 60, false}) || r[1].Name != "Cbor>>encode:" || r[1].Pct != 30 {
		t.Fatalf("%+v", r)
	}
}

func TestNoFrameBytesAreInTheDenominatorNotDropped(t *testing.T) {
	r := Parse("mprofa[window]: noframe_arena=900/9 noframe_cheap=0/0 allocOverflow=0\nmprofa: 16 100 1 0 0\n").RankAlloc(testMap)
	if r[0].Pct != 10 {
		t.Fatalf("%v", r[0].Pct) // 100 of 1000, not 100 of 100
	}
}

func TestAllocUnresolvedOffsetKeepsTheOffsetAndIsNotDropped(t *testing.T) {
	r := Parse("mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=0\nmprofa: 999 50 1 0 0\n").RankAlloc(testMap)
	if len(r) != 1 || !strings.Contains(r[0].Name, "UNKNOWN") || !strings.Contains(r[0].Name, "999") {
		t.Fatalf("%+v", r)
	}
}

func TestAllocReportWarnsWhenTheNoFrameBucketDominates(t *testing.T) {
	out := report("mprof[window]: methods=1 activations=1 overflow=0 cap=256\nmprof: 16 1\n"+
		"mprofa[window]: noframe_arena=900/9 noframe_cheap=0/0 allocOverflow=0\nmprofa: 16 100 1 0 0\n", 0, true)
	if !strings.Contains(out, "no-frame bucket EXCEEDS") || !strings.Contains(out, "distinct allocating methods: 1") {
		t.Fatal(out)
	}
}

func TestAllocReportFlagsASaturatedTable(t *testing.T) {
	out := report("mprof[window]: methods=1 activations=1 overflow=0 cap=256\nmprof: 16 1\n"+
		"mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=7\nmprofa: 16 100 1 0 0\n", 0, true)
	if !strings.Contains(out, "INCOMPLETE") || !strings.Contains(out, "allocOverflow=7") {
		t.Fatal(out)
	}
}

// --top is a display flag; the distinct count and the no-frame share are
// properties of the window.
func TestTopDoesNotShrinkTheDistinctCountOrTheNoFrameDenominator(t *testing.T) {
	out := report(allocDump, 1, true)
	for _, want := range []string{"distinct allocating methods: 2", "showing top 1 of 2 allocating methods",
		"= 10.0% of all attributed bytes"} {
		if !strings.Contains(out, want) {
			t.Fatalf("no %q in\n%s", want, out)
		}
	}
	if strings.Contains(strings.SplitN(out, "--- allocation attribution ---", 2)[1], "Cbor>>encode:") {
		t.Fatal(out)
	}
}

func TestATextWithNoProfileLinesParsesEmpty(t *testing.T) {
	d := Parse("tick 1 misses 0\nboot ok\n")
	if d.Header != nil || d.Rows != nil || d.Alloc != nil || d.AllocRows != nil {
		t.Fatalf("%+v", d)
	}
}
