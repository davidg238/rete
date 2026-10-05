// Copyright (c) 2026 Ekorau LLC

package gate

// pipeline.go — design §3: the authorizer is also the planner.
//
// D3(b) means custos must decode before it can aggregate, and a general SQL
// surface means it does not know WHAT to decode until the query is parsed. That
// is circular, except SQLite already solves it: the authorizer fires during
// prepare and reports every (table, column) a statement reads. So:
//
//	1. build a schema-only scratch — every logical table, empty, columns from
//	   the descriptor table. This step IS §4's completeness gate at runtime.
//	2. prepare the user's SQL against it, authorizer recording. Two jobs at
//	   once: it rejects anything that is not a read, and it yields the exact
//	   table/column set the query needs.
//	3. materialize only those, scanning and decoding from R via cval.
//	4. re-prepare and run.
//
// No predicate pushdown in the first cut. The whole referenced slice is
// materialized and its cost is REPORTED — Result.Sources carries rows and
// elapsed per table — so §7's measurement needs no new code, and pushdown is
// earned by a number rather than assumed.

import (
	"context"
	"database/sql"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/davidg238/rete/protocol/cval"
)

// Query is one caller's request. The three guards are §2.3's, and they are
// required rather than invented: a read-only promise never provided any of
// them. The defaults are a first cut whose cost Task 8 measures against the
// real store.
type Query struct {
	SQL string
	// Args are bound to the statement's parameters, never spliced into it.
	Args []any
	// Deadline is wall-clock, enforced through context cancellation, which
	// go-sqlite3 turns into sqlite3_interrupt.
	Deadline time.Duration
	// MaxRows caps the result. Exceeding it sets Result.Truncated rather than
	// silently returning a prefix.
	MaxRows int
	// MaxScratchBytes caps the materialized scratch file, checked after each
	// table so a runaway materialization fails before it fills the disk.
	MaxScratchBytes int64
}

const (
	defaultDeadline        = 30 * time.Second
	defaultMaxRows         = 10_000
	defaultMaxScratchBytes = 2 << 30 // 2 GiB
)

func (q Query) withDefaults() Query {
	if q.Deadline <= 0 {
		q.Deadline = defaultDeadline
	}
	if q.MaxRows <= 0 {
		q.MaxRows = defaultMaxRows
	}
	if q.MaxScratchBytes <= 0 {
		q.MaxScratchBytes = defaultMaxScratchBytes
	}
	return q
}

// Note is one column's reading rule, riding with the data (design §3.1). A help
// desk that returns data without the rule for reading it industrialises error at
// machine speed; this is the cheapest thing that does not.
type Note struct {
	Table    string   `json:"table"`
	Column   string   `json:"column"`
	Record   string   `json:"record"`
	Name     string   `json:"name"`
	Unit     string   `json:"unit"`
	Kind     string   `json:"kind"`
	Meaning  string   `json:"meaning"`
	Validity string   `json:"validity,omitempty"`
	Segment  []string `json:"segment,omitempty"`
	Refs     []string `json:"refs,omitempty"`
}

// Source is what one materialized table cost and where it came from. It carries
// three things at once: §7's measurement, the cells-vs-events provenance a
// decoded table's meaning depends on, and the `specified` tell — a record
// nothing builds yields an empty table, and empty is otherwise indistinguishable
// from a dead node (design addendum B2).
type Source struct {
	Table   string        `json:"table"`
	From    string        `json:"from"`
	Rows    int           `json:"rows"`
	Elapsed time.Duration `json:"elapsed"`
	Status  string        `json:"status,omitempty"`
}

// Result is one answer, with the rules for reading it.
type Result struct {
	Binding    Binding  `json:"binding"`
	SnapshotMs int64    `json:"snapshot_ms"`
	Columns    []string `json:"columns"`
	Rows       [][]any  `json:"rows"`
	Notes      []Note   `json:"notes"`
	Sources    []Source `json:"sources"`
	Truncated  bool     `json:"truncated"`
	// Elapsed is the whole pipeline, materialization included.
	Elapsed time.Duration `json:"elapsed"`
}

// deniedError is what a non-read statement answers. It names the actions the
// authorizer refused, because a refusal that cannot say what it refused sends
// the caller to the source.
type deniedError struct{ actions []string }

func (e *deniedError) Error() string {
	if len(e.actions) == 0 {
		return "gate: statement is not a read"
	}
	return "gate: statement is not a read — refused " + strings.Join(e.actions, ", ") +
		" (this surface reads; it does not write, attach or configure)"
}

// Actions answers what was refused, for a caller rendering its own message.
func (e *deniedError) Actions() []string { return append([]string(nil), e.actions...) }

// Query runs one statement through the pipeline (design §3).
func (g *Gate) Query(ctx context.Context, q Query) (*Result, error) {
	q = q.withDefaults()
	started := time.Now()

	ctx, cancel := context.WithTimeout(ctx, q.Deadline)
	defer cancel()

	s, err := g.newScratch()
	if err != nil {
		return nil, err
	}
	defer s.Close()

	schema, err := g.schema()
	if err != nil {
		return nil, err
	}
	if err := createSchema(ctx, s, schema); err != nil {
		return nil, err
	}
	if err := createViews(ctx, s, g.spec.views); err != nil {
		return nil, err
	}

	// Step 2: prepare, and let the authorizer plan.
	reads, err := plan(ctx, s, q.SQL)
	if err != nil {
		return nil, err
	}

	// Step 3: materialize only what the plan touched.
	sources, err := g.materialize(ctx, s, schema, reads, q.MaxScratchBytes)
	if err != nil {
		return nil, err
	}

	// Step 4: re-prepare and run. The recording is cleared first so the second
	// prepare does not double-count what the first already reported.
	s.auth.reset()
	res, err := runQuery(ctx, s, q)
	if err != nil {
		return nil, err
	}

	res.Binding = g.binding
	res.SnapshotMs = g.snapshotMs
	res.Sources = sources
	res.Notes = g.notesFor(schema, reads)
	res.Elapsed = time.Since(started)
	return res, nil
}

// plan prepares the statement and answers the (table, column) set it reads. A
// statement that is not a read never reaches step 3.
func plan(ctx context.Context, s *scratch, query string) (map[string][]string, error) {
	stmt, err := s.q.PrepareContext(ctx, query)
	if err != nil {
		if denied := s.auth.Denied(); len(denied) > 0 {
			return nil, &deniedError{actions: denied}
		}
		return nil, fmt.Errorf("gate: %w", err)
	}
	defer stmt.Close()
	// A statement can prepare successfully and still have touched a denied
	// action, so the refusal is checked on its own rather than inferred from
	// the prepare's error.
	if denied := s.auth.Denied(); len(denied) > 0 {
		return nil, &deniedError{actions: denied}
	}
	return s.auth.Reads(), nil
}

// runQuery executes the statement and collects rows under the row cap.
func runQuery(ctx context.Context, s *scratch, q Query) (*Result, error) {
	rows, err := s.q.QueryContext(ctx, q.SQL, q.Args...)
	if err != nil {
		if denied := s.auth.Denied(); len(denied) > 0 {
			return nil, &deniedError{actions: denied}
		}
		return nil, fmt.Errorf("gate: %w", err)
	}
	defer rows.Close()

	cols, err := rows.Columns()
	if err != nil {
		return nil, err
	}
	res := &Result{Columns: cols, Rows: [][]any{}}
	for rows.Next() {
		if len(res.Rows) == q.MaxRows {
			// One row past the cap is read deliberately: the difference
			// between "exactly MaxRows" and "truncated" is not knowable
			// without it, and a silent prefix is a wrong answer.
			res.Truncated = true
			break
		}
		row := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range row {
			ptrs[i] = &row[i]
		}
		if err := rows.Scan(ptrs...); err != nil {
			return nil, err
		}
		res.Rows = append(res.Rows, row)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("gate: %w", err)
	}
	return res, nil
}

// ---------------------------------------------------------------------------
// the schema
// ---------------------------------------------------------------------------

type column struct{ name, typ string }

type tableKind int

const (
	tableStructural  tableKind = iota // passes through from the store near-verbatim
	tableDecoded                      // one record-bearing path, columns from descriptors
	tableDescriptors                  // the reading rules themselves, queryable (D2)
)

type tableDef struct {
	name string
	kind tableKind
	cols []column
	rule string // decoded: the CDDL rule
	path string // decoded: the manifest path, wildcard included
	// derived counts the trailing cols the materializer computes rather than
	// copies (structural only; cells, views.go).
	derived int
	// scalar marks a decoded table over a scalar path: one `value` column and
	// no record tag to check (addendum D5).
	scalar bool
}

// scalarColumn is the one column a scalar path's decoded table carries.
const scalarColumn = "value"

// identityColumns prefix every decoded record table: which node, which stored
// path, when, which boot, which sequence number. Uniform across every decoded
// table — §4.2's column lists vary in their prefix, and that variation is
// shorthand rather than design; one prefix is the smaller mechanism.
//
// `src_path` rather than `path` because `Overrun.path` is a real wire field
// meaning something else entirely — the path whose entries were dropped, not the
// path this row was stored at. checkIdentityCollisions closes the class.
var identityColumns = []column{
	{"node_id", "TEXT"},
	{"src_path", "TEXT"},
	{"ts_ms", "INTEGER"},
	{"boot_id", "INTEGER"},
	{"seq", "INTEGER"},
}

// descriptorColumns are the queryable form of the reading rules — D2 landing
// rather than being asserted, since `select * from descriptors where key='lfb'`
// is the same read as everything else.
var descriptorColumns = []column{
	{"rule", "TEXT"}, {"record", "TEXT"}, {"paths", "TEXT"}, {"status", "TEXT"},
	{"key", "TEXT"}, {"name", "TEXT"}, {"unit", "TEXT"}, {"kind", "TEXT"},
	{"meaning", "TEXT"}, {"validity", "TEXT"}, {"segment", "TEXT"}, {"refs", "TEXT"},
}

// checkIdentityCollisions refuses a descriptor field whose key would collide
// with an identity column. The one real collision (`Overrun.path`) is why
// identityColumns says `src_path`; this makes the class closed rather than
// closed-by-luck, and it fails at schema build, not at a wrong answer.
func checkIdentityCollisions(d *Descriptors) error {
	reserved := map[string]bool{}
	for _, c := range identityColumns {
		reserved[c.name] = true
	}
	var bad []string
	for _, rule := range d.RecordRules() {
		for _, key := range d.Columns(rule) {
			if reserved[key] {
				bad = append(bad, fmt.Sprintf("%s.%s", rule, key))
			}
		}
	}
	if len(bad) > 0 {
		return fmt.Errorf("descriptor field(s) %s collide with a decoded table's identity column; "+
			"rename the identity column in gate.identityColumns — a wire field is not renameable",
			strings.Join(bad, ", "))
	}
	return nil
}

// schema answers every logical table this binding offers. Structural columns are
// read from R with PRAGMA table_info rather than restated here, so the scratch's
// copy of porta's schema cannot drift from porta's schema. Decoded columns come
// only from the descriptor table.
func (g *Gate) schema() (map[string]*tableDef, error) {
	if err := checkIdentityCollisions(g.desc); err != nil {
		return nil, err
	}
	out := map[string]*tableDef{}

	for _, name := range g.spec.base {
		cols, err := g.structuralColumns(name)
		if err != nil {
			return nil, err
		}
		td := &tableDef{name: name, kind: tableStructural, cols: cols}
		if name == "cells" {
			td.cols = append(td.cols, cellDerivedColumns...)
			td.derived = len(cellDerivedColumns)
		}
		out[name] = td
	}

	if g.spec.decoded {
		for _, rule := range g.desc.RecordRules() {
			rec := g.desc.Records[rule]
			for _, p := range rec.Paths {
				name := TableName(p)
				if prev, dup := out[name]; dup {
					return nil, fmt.Errorf("gate: table %q claimed by both %q and %q — "+
						"two paths deriving one table name", name, prev.rule, rule)
				}
				cols := append([]column(nil), identityColumns...)
				for _, key := range g.desc.Columns(rule) {
					cols = append(cols, column{key, ""}) // no affinity: the wire decides
				}
				out[name] = &tableDef{name: name, kind: tableDecoded, cols: cols, rule: rule, path: p}
			}
		}
		// A scalar path is a decoded table too — identity plus its value — so
		// goal/name's history is the same read as sys/metrics' (addendum D5).
		for _, rule := range g.desc.ScalarRules() {
			for _, p := range g.desc.Scalars[rule].Paths {
				name := TableName(p)
				if prev, dup := out[name]; dup {
					return nil, fmt.Errorf("gate: table %q claimed by both %q and %q — "+
						"two paths deriving one table name", name, prev.rule, rule)
				}
				cols := append(append([]column(nil), identityColumns...), column{scalarColumn, ""})
				out[name] = &tableDef{name: name, kind: tableDecoded, cols: cols, rule: rule, path: p, scalar: true}
			}
		}
	}

	out["descriptors"] = &tableDef{name: "descriptors", kind: tableDescriptors, cols: descriptorColumns}
	return out, nil
}

// structuralColumns reads a store table's shape from R itself.
func (g *Gate) structuralColumns(table string) ([]column, error) {
	rows, err := g.r.Query(`SELECT name, type FROM pragma_table_info(?)`, table)
	if err != nil {
		return nil, fmt.Errorf("gate: reading %s's shape: %w", table, err)
	}
	defer rows.Close()
	var cols []column
	for rows.Next() {
		var c column
		if err := rows.Scan(&c.name, &c.typ); err != nil {
			return nil, err
		}
		cols = append(cols, c)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(cols) == 0 {
		return nil, fmt.Errorf("gate: %s has no columns in %s — this binding names a table the source does not have",
			table, g.source)
	}
	return cols, nil
}

// createSchema builds every logical table, empty, on W. Q sees them because Q is
// the same file — and sees nothing else, which is the topology doing the work.
func createSchema(ctx context.Context, s *scratch, schema map[string]*tableDef) error {
	names := make([]string, 0, len(schema))
	for n := range schema {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		td := schema[n]
		parts := make([]string, len(td.cols))
		for i, c := range td.cols {
			parts[i] = quoteIdent(c.name)
			if c.typ != "" {
				parts[i] += " " + c.typ
			}
		}
		ddl := fmt.Sprintf("CREATE TABLE %s (%s)", quoteIdent(td.name), strings.Join(parts, ", "))
		if _, err := s.w.ExecContext(ctx, ddl); err != nil {
			return fmt.Errorf("gate: creating %s: %w", td.name, err)
		}
	}
	return nil
}

// quoteIdent renders an identifier for DDL. Every identifier here comes from the
// descriptor table or from PRAGMA table_info — never from query text (§2.3, W
// being the one place injection would matter) — so this guards a typo rather
// than an attacker, and doubling is enough.
func quoteIdent(s string) string {
	return `"` + strings.ReplaceAll(s, `"`, `""`) + `"`
}

// ---------------------------------------------------------------------------
// materialization
// ---------------------------------------------------------------------------

// materialize fills only the tables the plan touched.
func (g *Gate) materialize(ctx context.Context, s *scratch, schema map[string]*tableDef,
	reads map[string][]string, maxBytes int64) ([]Source, error) {

	names := make([]string, 0, len(reads))
	for t := range reads {
		if _, ok := schema[t]; ok {
			names = append(names, t)
		}
	}
	sort.Strings(names)

	var out []Source
	for _, name := range names {
		td := schema[name]
		started := time.Now()
		var (
			n    int
			from string
			err  error
		)
		switch td.kind {
		case tableStructural:
			from = td.name
			n, err = g.copyStructural(ctx, s, td)
		case tableDecoded:
			from = g.decodedSource()
			n, err = g.copyDecoded(ctx, s, td, from)
		case tableDescriptors:
			from = "descriptors.yaml"
			n, err = g.copyDescriptors(ctx, s, td)
		}
		if err != nil {
			return nil, err
		}
		src := Source{Table: name, From: from, Rows: n, Elapsed: time.Since(started)}
		if td.kind == tableDecoded && !td.scalar {
			src.Status = g.desc.Records[td.rule].Status
		}
		out = append(out, src)

		if err := checkScratchSize(s, maxBytes); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// decodedSource answers which store table decoded rows come from, and the answer
// changes what they MEAN: `snap` offers events, so a decoded table is the full
// history; `live` does not, so it is the latest value per path and nothing more.
// Result.Sources carries this out with the answer rather than leaving it to be
// assumed.
func (g *Gate) decodedSource() string {
	if contains(g.spec.base, "events") {
		return "events"
	}
	return "cells"
}

func checkScratchSize(s *scratch, maxBytes int64) error {
	var total int64
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return err
	}
	for _, e := range entries {
		info, err := e.Info()
		if err != nil {
			continue
		}
		total += info.Size()
	}
	if total > maxBytes {
		return fmt.Errorf("gate: materialization exceeded the scratch cap (%d B > %d B) — "+
			"narrow the query, or raise Query.MaxScratchBytes deliberately", total, maxBytes)
	}
	return nil
}

// copyStructural copies one store table through, applying the binding's cap,
// and computes its derived columns when it has any.
func (g *Gate) copyStructural(ctx context.Context, s *scratch, td *tableDef) (int, error) {
	base := td.cols[:len(td.cols)-td.derived]
	names := make([]string, len(base))
	for i, c := range base {
		names[i] = quoteIdent(c.name)
	}
	var deriver *cellDeriver
	if td.derived > 0 {
		var err error
		if deriver, err = g.newCellDeriver(ctx, td, base); err != nil {
			return 0, err
		}
		defer deriver.Close()
	}
	sel := fmt.Sprintf("SELECT %s FROM %s", strings.Join(names, ", "), quoteIdent(td.name))

	// The one unbounded table `live` exposes is capped to each node's own last
	// 24 h. Per node rather than wall-clock, so a node that has stopped
	// checking in keeps its final window instead of vanishing from the table
	// that would explain why it is stale.
	if td.name == "exchanges" && g.spec.exchangeCapMs > 0 {
		sel += fmt.Sprintf(
			" WHERE ts_ms >= (SELECT MAX(x2.ts_ms) FROM exchanges x2 WHERE x2.node_id = exchanges.node_id) - %d",
			g.spec.exchangeCapMs)
	}

	rows, err := g.r.QueryContext(ctx, sel)
	if err != nil {
		return 0, fmt.Errorf("gate: reading %s: %w", td.name, err)
	}
	defer rows.Close()

	ins, done, err := beginInsert(ctx, s, td)
	if err != nil {
		return 0, err
	}
	defer done(&err)

	n := 0
	for rows.Next() {
		vals := make([]any, len(base))
		ptrs := make([]any, len(base))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err = rows.Scan(ptrs...); err != nil {
			return 0, err
		}
		if deriver != nil {
			var extra []any
			if extra, err = deriver.derive(ctx, vals); err != nil {
				return 0, err
			}
			vals = append(vals, extra...)
		}
		if _, err = ins.ExecContext(ctx, vals...); err != nil {
			return 0, fmt.Errorf("gate: materializing %s: %w", td.name, err)
		}
		n++
	}
	if err = rows.Err(); err != nil {
		return 0, err
	}
	return n, nil
}

// copyDecoded scans the record-bearing path and decodes each value through cval
// — the one codec, since a second would be a defect (D3(b)).
func (g *Gate) copyDecoded(ctx context.Context, s *scratch, td *tableDef, src string) (int, error) {
	match, wildcard := pathMatch(td.path)
	pred := "path = ?"
	if wildcard {
		pred = "path GLOB ?"
	}
	sel := fmt.Sprintf("SELECT node_id, path, ts_ms, boot_id, seq, value_cbor FROM %s WHERE %s",
		quoteIdent(src), pred)
	if src == "events" {
		sel += " ORDER BY id"
	}

	rows, err := g.r.QueryContext(ctx, sel, match)
	if err != nil {
		return 0, fmt.Errorf("gate: reading %s for %s: %w", src, td.name, err)
	}
	defer rows.Close()

	ins, done, err := beginInsert(ctx, s, td)
	if err != nil {
		return 0, err
	}
	defer done(&err)

	var (
		keys    []string
		wantTag cval.Symbol
	)
	if td.scalar {
		keys = []string{scalarColumn}
	} else {
		keys = g.desc.Columns(td.rule)
		wantTag = cval.Symbol(g.desc.Records[td.rule].Name)
	}

	n := 0
	for rows.Next() {
		var (
			nodeID, storedPath string
			tsMs, bootID, seq  int64
			blob               []byte
		)
		if err = rows.Scan(&nodeID, &storedPath, &tsMs, &bootID, &seq, &blob); err != nil {
			return 0, err
		}
		vals := make([]any, 0, len(identityColumns)+len(keys))
		vals = append(vals, nodeID, storedPath, tsMs, bootID, seq)
		if td.scalar {
			var v any
			if v, err = cval.Decode(blob); err != nil {
				return 0, fmt.Errorf("gate: %s: decoding %s seq %d at %s: %w", td.name, nodeID, seq, storedPath, err)
			}
			if v, err = sqlValue(v, td, scalarColumn, nodeID, seq); err != nil {
				return 0, err
			}
			vals = append(vals, v)
		} else {
			var fields cval.Map
			if fields, err = decodeRecord(blob, wantTag, td, nodeID, storedPath, seq); err != nil {
				return 0, err
			}
			for _, key := range keys {
				var v any
				v, err = sqlValue(lookup(fields, key), td, key, nodeID, seq)
				if err != nil {
					return 0, err
				}
				vals = append(vals, v)
			}
		}
		if _, err = ins.ExecContext(ctx, vals...); err != nil {
			return 0, fmt.Errorf("gate: materializing %s: %w", td.name, err)
		}
		n++
	}
	if err = rows.Err(); err != nil {
		return 0, err
	}
	return n, nil
}

// pathMatch turns a manifest path into a store predicate. A wildcard segment is
// a row's key, so it matches rather than being dropped.
func pathMatch(p string) (string, bool) {
	if !strings.Contains(p, "<") {
		return p, false
	}
	segs := strings.Split(p, "/")
	for i, s := range segs {
		if strings.HasPrefix(s, "<") {
			segs[i] = "*"
		}
	}
	return strings.Join(segs, "/"), true
}

// decodeRecord decodes one stored value and checks it is the shape the
// descriptor claims rides at that path.
//
// A decode failure or a tag mismatch is FATAL to the query, naming the row. A
// skipped row would be a wrong answer that reads exactly like a right one, and
// this surface exists to stop that class rather than to add to it. If the real
// store turns out to hold rows this refuses, Task 8's recorded run is where that
// shows up, with the evidence to widen the rule deliberately.
func decodeRecord(blob []byte, wantTag cval.Symbol, td *tableDef, nodeID, storedPath string, seq int64) (cval.Map, error) {
	v, err := cval.Decode(blob)
	if err != nil {
		return nil, fmt.Errorf("gate: %s: decoding %s seq %d at %s: %w",
			td.name, nodeID, seq, storedPath, err)
	}
	rec, ok := v.(cval.Record)
	if !ok {
		return nil, fmt.Errorf("gate: %s: %s seq %d at %s holds %s, not a %s record",
			td.name, nodeID, seq, storedPath, cval.Format(v), wantTag)
	}
	if rec.Tag != wantTag {
		return nil, fmt.Errorf("gate: %s: %s seq %d at %s holds a #%s record where the catalogue says #%s",
			td.name, nodeID, seq, storedPath, rec.Tag, wantTag)
	}
	return rec.Fields, nil
}

// lookup answers a record field by key, or nil when the record does not carry
// it. Absent is NULL rather than an error: the manifest is the shape gate, and a
// row written by an older build is missing data, not malformed.
func lookup(fields cval.Map, key string) any {
	want := cval.Symbol(key)
	for _, p := range fields {
		if sym, ok := p.K.(cval.Symbol); ok && sym == want {
			return p.V
		}
	}
	return nil
}

// sqlValue maps one algebra value to a SQLite value.
//
// The integer band is ±(2^64−1), which exceeds int64 (addendum A4). A value
// outside int64 is a LOUD ERROR naming the field and the row — never a clamp,
// because a clamped number is a wrong answer that no reader can detect. No field
// in today's catalogue reaches the band; this is the guard for the day one does.
func sqlValue(v any, td *tableDef, key, nodeID string, seq int64) (any, error) {
	switch t := v.(type) {
	case nil:
		return nil, nil
	case bool:
		return t, nil
	case int64:
		return t, nil
	case float64:
		return t, nil
	case string:
		return t, nil
	case cval.Bytes:
		return []byte(t), nil
	case cval.Symbol:
		return string(t), nil // bare, as at:/size read it — the '#' is printString's
	case cval.Uint:
		if uint64(t) > uint64(math.MaxInt64) {
			return nil, outOfBand(td, key, nodeID, seq, fmt.Sprintf("%d", uint64(t)))
		}
		return int64(t), nil
	case cval.NegUint:
		// −1−n, which by construction is used only below math.MinInt64.
		return nil, outOfBand(td, key, nodeID, seq, fmt.Sprintf("-1-%d", uint64(t)))
	case cval.List, cval.Map, cval.Record:
		return cval.Format(t), nil
	default:
		return nil, fmt.Errorf("gate: %s.%s: %s seq %d holds an algebra value this materializer does not map (%T)",
			td.name, key, nodeID, seq, v)
	}
}

func outOfBand(td *tableDef, key, nodeID string, seq int64, text string) error {
	return fmt.Errorf("gate: %s.%s: %s seq %d is %s, outside SQLite's int64 — "+
		"the algebra's band is ±(2^64−1) and this value is not representable exactly. "+
		"It is refused rather than clamped, because a clamped number is a wrong answer "+
		"no reader can detect (design addendum A4)",
		td.name, key, nodeID, seq, text)
}

// copyDescriptors materializes the reading rules as rows, so the dictionary is
// queryable by the same read as everything else.
func (g *Gate) copyDescriptors(ctx context.Context, s *scratch, td *tableDef) (int, error) {
	ins, done, err := beginInsert(ctx, s, td)
	if err != nil {
		return 0, err
	}
	defer done(&err)

	n := 0
	for _, rule := range g.desc.RecordRules() {
		r := g.desc.Records[rule]
		paths := strings.Join(r.Paths, ",")
		for _, f := range r.Fields {
			if _, err = ins.ExecContext(ctx, rule, r.Name, paths, r.Status, f.Key, f.Name,
				f.Unit, f.Kind, f.Meaning, f.Validity,
				strings.Join(f.Segment, ","), strings.Join(f.Refs, ",")); err != nil {
				return 0, err
			}
			n++
		}
	}
	// A scalar path has no fields, so it carries one meaning rather than a
	// field list. It gets a row with an empty key: a path whose rule exists is
	// discoverable here, and silence at it is explained rather than inferred.
	for _, rule := range g.desc.ScalarRules() {
		sc := g.desc.Scalars[rule]
		if _, err = ins.ExecContext(ctx, rule, "", strings.Join(sc.Paths, ","), "", "", rule,
			sc.Unit, "", sc.Meaning, sc.Validity, "", strings.Join(sc.Refs, ",")); err != nil {
			return 0, err
		}
		n++
	}
	return n, nil
}

// beginInsert opens a transaction on W with a prepared INSERT for the table, and
// answers a finisher that commits or rolls back according to err at the call
// site. One transaction per table is what keeps a 1.7 M-row materialization from
// being 1.7 M commits.
func beginInsert(ctx context.Context, s *scratch, td *tableDef) (*sql.Stmt, func(*error), error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return nil, nil, err
	}
	names := make([]string, len(td.cols))
	marks := make([]string, len(td.cols))
	for i, c := range td.cols {
		names[i] = quoteIdent(c.name)
		marks[i] = "?"
	}
	stmt, err := tx.PrepareContext(ctx, fmt.Sprintf("INSERT INTO %s (%s) VALUES (%s)",
		quoteIdent(td.name), strings.Join(names, ", "), strings.Join(marks, ", ")))
	if err != nil {
		tx.Rollback()
		return nil, nil, err
	}
	done := func(errp *error) {
		stmt.Close()
		if *errp != nil {
			tx.Rollback()
			return
		}
		if cerr := tx.Commit(); cerr != nil && *errp == nil {
			*errp = cerr
		}
	}
	return stmt, done, nil
}

// ---------------------------------------------------------------------------
// notes
// ---------------------------------------------------------------------------

// notesFor answers the reading rule for every descriptor-backed column the query
// touched. Identity columns carry no note: they are the row's address, not a
// measurement, and a note per address would bury the ones that matter.
func (g *Gate) notesFor(schema map[string]*tableDef, reads map[string][]string) []Note {
	identity := map[string]bool{}
	for _, c := range identityColumns {
		identity[c.name] = true
	}
	out := []Note{}
	tables := make([]string, 0, len(reads))
	for t := range reads {
		tables = append(tables, t)
	}
	sort.Strings(tables)
	for _, t := range tables {
		td, ok := schema[t]
		if !ok || td.kind != tableDecoded {
			continue
		}
		if td.scalar {
			sc := g.desc.Scalars[td.rule]
			for _, col := range reads[t] {
				if col == scalarColumn {
					out = append(out, Note{Table: t, Column: col, Name: td.rule, Unit: sc.Unit,
						Meaning: sc.Meaning, Validity: sc.Validity, Refs: sc.Refs})
				}
			}
			continue
		}
		rec := g.desc.Records[td.rule]
		for _, col := range reads[t] {
			if identity[col] {
				continue
			}
			f := g.desc.Note(td.rule, col)
			if f == nil {
				continue
			}
			out = append(out, Note{
				Table: t, Column: col, Record: rec.Name, Name: f.Name,
				Unit: f.Unit, Kind: f.Kind, Meaning: f.Meaning, Validity: f.Validity,
				Segment: f.Segment, Refs: f.Refs,
			})
		}
	}
	return out
}

// Tables answers the logical tables and views this binding offers, for
// `gate tables`.
func (g *Gate) Tables() ([]string, error) {
	schema, err := g.schema()
	if err != nil {
		return nil, err
	}
	out := make([]string, 0, len(schema)+len(g.spec.views))
	for n := range schema {
		out = append(out, n)
	}
	for _, v := range g.spec.views {
		out = append(out, v.name)
	}
	sort.Strings(out)
	return out, nil
}

// scratchPath is the scratch file, exposed for the guard tests that assert it
// does not outlive the query.
func (s *scratch) scratchPath() string { return filepath.Clean(s.path) }
