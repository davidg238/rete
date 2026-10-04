// Copyright (c) 2026 Ekorau LLC

package gate

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"sync"

	"github.com/mattn/go-sqlite3"
)

// The three-connection topology (design §2.2).
//
//	R  the snapshot (or, for `live`, porta's store)  immutable=1 / mode=ro
//	W  a scratch db, fresh per query, ON DISK        read-write, private
//	Q  the same scratch db                           _query_only + authorizer
//
// The load-bearing guard is the topology, not the authorizer: the only database
// reachable from a statement anyone types is a scratch built for that one query
// and deleted after it. `DELETE FROM events` is not denied — there is no
// `events` on Q, and no path from Q to any store. The authorizer is still not
// optional, because ATTACH would otherwise reach back out to the filesystem.
//
// Scratch goes on disk and never `:memory:` — gw85224-01 has 3.7 GB of RAM and
// 1.7 TB of disk, and OOMing the box that runs the fleet is a worse outcome than
// a slow query.

// Binding names the source a query runs against (design §2.4).
type Binding string

const (
	// BindingSnap is an immutable snapshot: all history, unbounded tables, and
	// no contact with the live store.
	BindingSnap Binding = "snap"
	// BindingLive is porta's own store, read-only, exposing BOUNDED tables only.
	// "Stale right now" must not be answered from a file — rete#225 is the
	// failure where gw/liveness read #live for ten hours while the node missed
	// every check-in, and a staleness answer computed from yesterday's snapshot
	// reproduces that bug by construction.
	BindingLive Binding = "live"
)

// exchangeWindowMs bounds the one unbounded table the live binding exposes. A
// query needing more history belongs on `snap`.
const exchangeWindowMs = 24 * 60 * 60 * 1000

// bindingSpec is what a binding structurally offers. The boundedness is in the
// topology, exactly as in §2.2: no query written against `live` can scan the
// 1.7 M-row events table, because that table is not there to scan.
type bindingSpec struct {
	base          []string // store tables this binding materializes from
	exchangeCapMs int64    // 0 means unbounded
	decoded       bool     // whether decoded record tables are offered
	views         []viewDef
}

// The fleet views are `live` only. A verdict or a convergence read from a
// snapshot is yesterday's answer to "right now" — rete#225 by construction.
var bindings = map[Binding]bindingSpec{
	BindingSnap: {base: []string{"cells", "events", "exchanges", "gw_runs", "nodes"}, exchangeCapMs: 0, decoded: true},
	BindingLive: {base: []string{"cells", "exchanges", "nodes"}, exchangeCapMs: exchangeWindowMs, decoded: true,
		views: []viewDef{viewConvergence, viewLiveness}},
}

// Gate is an open query surface over one source. It holds R; W and Q are built
// per query and discarded with it.
type Gate struct {
	binding    Binding
	source     string
	spec       bindingSpec
	desc       *Descriptors
	r          *sql.DB
	snapshotMs int64 // the moment the source was frozen; 0 on `live`, which is "now"
}

// Open answers a gate over an immutable snapshot (design §2). The snapshot is
// opened with immutable=1, which is TRUE of a snapshot and false of a live
// store — asserting it about a live store buys silently wrong reads rather than
// errors, so a `-wal` sibling is refused here rather than trusted.
func Open(snapshotPath string, d *Descriptors) (*Gate, error) {
	if d == nil {
		return nil, fmt.Errorf("gate.Open: no descriptor table")
	}
	st, err := os.Stat(snapshotPath)
	if err != nil {
		return nil, err
	}
	for _, sib := range []string{snapshotPath + "-wal", snapshotPath + "-shm"} {
		if _, err := os.Stat(sib); err == nil {
			return nil, fmt.Errorf("gate.Open: %s exists, so %s is a LIVE store, not a snapshot — "+
				"immutable=1 would assert something false about it and read silently stale pages; "+
				"take a snapshot (porta snapshot --out) or use OpenLive", filepath.Base(sib), snapshotPath)
		}
	}
	db, err := sql.Open("sqlite3", "file:"+snapshotPath+"?immutable=1&mode=ro")
	if err != nil {
		return nil, err
	}
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, fmt.Errorf("gate.Open %s: %w", snapshotPath, err)
	}
	return &Gate{
		binding:    BindingSnap,
		source:     snapshotPath,
		spec:       bindings[BindingSnap],
		desc:       d,
		r:          db,
		snapshotMs: st.ModTime().UnixMilli(),
	}, nil
}

// OpenLive answers a gate over porta's live store, read-only and bounded.
//
// mode=ro is a promise SQLite keeps, not a capability this process lacks — it
// still holds write permission on the file, and a WAL reader still writes -shm.
// That is why this binding exposes bounded tables only: the guard is what the
// query can reach, not what the handle promises.
func OpenLive(storePath string, d *Descriptors) (*Gate, error) {
	if d == nil {
		return nil, fmt.Errorf("gate.OpenLive: no descriptor table")
	}
	if _, err := os.Stat(storePath); err != nil {
		return nil, err
	}
	db, err := sql.Open("sqlite3", "file:"+storePath+"?mode=ro&_busy_timeout=5000")
	if err != nil {
		return nil, err
	}
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, fmt.Errorf("gate.OpenLive %s: %w", storePath, err)
	}
	return &Gate{
		binding: BindingLive,
		source:  storePath,
		spec:    bindings[BindingLive],
		desc:    d,
		r:       db,
	}, nil
}

// Binding answers which source this gate reads.
func (g *Gate) Binding() Binding { return g.binding }

// SnapshotMs answers the moment the source was frozen, or 0 on `live` — which
// means "now", and is the distinction rete#225 turns on.
func (g *Gate) SnapshotMs() int64 { return g.snapshotMs }

// Descriptors answers the reading rules this gate annotates answers with.
func (g *Gate) Descriptors() *Descriptors { return g.desc }

// BaseTables answers the store tables this binding will materialize from.
func (g *Gate) BaseTables() []string { return append([]string(nil), g.spec.base...) }

// ExchangeCapMs answers the window applied to `exchanges` on this binding, or 0
// when it is offered whole.
func (g *Gate) ExchangeCapMs() int64 { return g.spec.exchangeCapMs }

// Close releases R.
func (g *Gate) Close() error {
	if g.r == nil {
		return nil
	}
	err := g.r.Close()
	g.r = nil
	return err
}

// scratch is the per-query W/Q pair and the file they share. Fresh for every
// query and removed with it, which is what makes a store unreachable from Q
// rather than merely forbidden.
type scratch struct {
	dir  string
	path string
	w    *sql.DB // the materializer's connection: read-write, private
	q    *sql.DB // the caller's connection: _query_only, authorized
	auth *authorizer
}

// newScratch builds the W/Q pair for one query.
func (g *Gate) newScratch() (*scratch, error) {
	dir, err := os.MkdirTemp("", "gate-scratch-")
	if err != nil {
		return nil, err
	}
	s := &scratch{dir: dir, path: filepath.Join(dir, "scratch.db"), auth: newAuthorizer()}

	if s.w, err = sql.Open("sqlite3", "file:"+s.path+"?_busy_timeout=5000"); err != nil {
		s.Close()
		return nil, err
	}
	if err := s.w.Ping(); err != nil {
		s.Close()
		return nil, err
	}

	// Q gets its own driver instance rather than a registered name: the hook
	// closes over THIS scratch's authorizer, so nothing is global and two
	// concurrent queries cannot see each other's recording.
	s.q = sql.OpenDB(&hookedConnector{
		dsn: "file:" + s.path + "?_query_only=1&_busy_timeout=5000",
		hook: func(c *sqlite3.SQLiteConn) error {
			c.RegisterAuthorizer(s.auth.check)
			for id, v := range queryLimits {
				c.SetLimit(id, v)
			}
			return nil
		},
	})
	s.q.SetMaxOpenConns(1) // one conn, so the authorizer's recording is per query
	if err := s.q.Ping(); err != nil {
		s.Close()
		return nil, err
	}
	return s, nil
}

// queryLimits are §2.3's structural caps on Q. They bound what a statement can
// ask the VDBE to do at all, which no read-only promise ever did.
var queryLimits = map[int]int{
	sqlite3.SQLITE_LIMIT_EXPR_DEPTH:          256,
	sqlite3.SQLITE_LIMIT_COMPOUND_SELECT:     16,
	sqlite3.SQLITE_LIMIT_VDBE_OP:             250_000,
	sqlite3.SQLITE_LIMIT_ATTACHED:            0,
	sqlite3.SQLITE_LIMIT_LIKE_PATTERN_LENGTH: 4096,
}

// Close drops both connections and removes the scratch file. The file going
// away is the point: a query's database does not outlive the query.
func (s *scratch) Close() error {
	var first error
	for _, db := range []*sql.DB{s.q, s.w} {
		if db == nil {
			continue
		}
		if err := db.Close(); err != nil && first == nil {
			first = err
		}
	}
	s.q, s.w = nil, nil
	if err := os.RemoveAll(s.dir); err != nil && first == nil {
		first = err
	}
	return first
}

// hookedConnector hands database/sql a driver carrying a per-scratch connect
// hook, so no driver name is registered globally.
type hookedConnector struct {
	dsn  string
	hook func(*sqlite3.SQLiteConn) error
}

func (c *hookedConnector) Connect(context.Context) (driver.Conn, error) {
	return (&sqlite3.SQLiteDriver{ConnectHook: c.hook}).Open(c.dsn)
}

func (c *hookedConnector) Driver() driver.Driver { return &sqlite3.SQLiteDriver{} }

// authorizer is both guard and planner (design §3): it refuses anything that is
// not a read, and in doing so it reports the exact (table, column) set the
// statement needs — which is what makes "decode only what the query touches"
// possible without custos parsing SQL itself.
type authorizer struct {
	mu     sync.Mutex
	reads  map[string]map[string]bool // table -> columns
	denied []string                   // what it refused, in order, for the error
}

func newAuthorizer() *authorizer {
	return &authorizer{reads: map[string]map[string]bool{}}
}

// permitted actions. SQLITE_READ additionally records; everything absent here is
// denied by action code, ATTACH and PRAGMA included.
var permittedActions = map[int]bool{
	sqlite3.SQLITE_READ:     true,
	sqlite3.SQLITE_SELECT:   true,
	sqlite3.SQLITE_FUNCTION: true,
}

func (a *authorizer) check(op int, arg1, arg2, _ string) int {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !permittedActions[op] {
		a.denied = append(a.denied, actionName(op))
		return sqlite3.SQLITE_DENY
	}
	if op == sqlite3.SQLITE_READ && arg1 != "" {
		cols, ok := a.reads[arg1]
		if !ok {
			cols = map[string]bool{}
			a.reads[arg1] = cols
		}
		if arg2 != "" {
			cols[arg2] = true
		}
	}
	return sqlite3.SQLITE_OK
}

// Reads answers the (table, column) set the statements prepared so far touched,
// with tables and columns each in a deterministic order.
func (a *authorizer) Reads() map[string][]string {
	a.mu.Lock()
	defer a.mu.Unlock()
	out := map[string][]string{}
	for tbl, cols := range a.reads {
		list := make([]string, 0, len(cols))
		for c := range cols {
			list = append(list, c)
		}
		sort.Strings(list)
		out[tbl] = list
	}
	return out
}

// Denied answers what the authorizer refused, deduplicated in first-seen order.
func (a *authorizer) Denied() []string {
	a.mu.Lock()
	defer a.mu.Unlock()
	seen := map[string]bool{}
	out := []string{}
	for _, d := range a.denied {
		if !seen[d] {
			seen[d] = true
			out = append(out, d)
		}
	}
	return out
}

// reset clears the recording between the two prepares of one query (design §3
// steps 2 and 4), so the second prepare does not double-count.
func (a *authorizer) reset() {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.reads = map[string]map[string]bool{}
	a.denied = nil
}

// actionName renders an authorizer action code for an error message. A refusal
// that cannot say what it refused sends the caller to the source.
func actionName(op int) string {
	switch op {
	case sqlite3.SQLITE_CREATE_INDEX:
		return "CREATE INDEX"
	case sqlite3.SQLITE_CREATE_TABLE:
		return "CREATE TABLE"
	case sqlite3.SQLITE_CREATE_TEMP_TABLE:
		return "CREATE TEMP TABLE"
	case sqlite3.SQLITE_CREATE_TEMP_TRIGGER:
		return "CREATE TEMP TRIGGER"
	case sqlite3.SQLITE_CREATE_TEMP_VIEW:
		return "CREATE TEMP VIEW"
	case sqlite3.SQLITE_CREATE_TRIGGER:
		return "CREATE TRIGGER"
	case sqlite3.SQLITE_CREATE_VIEW:
		return "CREATE VIEW"
	case sqlite3.SQLITE_CREATE_VTABLE:
		return "CREATE VIRTUAL TABLE"
	case sqlite3.SQLITE_DELETE:
		return "DELETE"
	case sqlite3.SQLITE_DROP_INDEX:
		return "DROP INDEX"
	case sqlite3.SQLITE_DROP_TABLE:
		return "DROP TABLE"
	case sqlite3.SQLITE_DROP_TRIGGER:
		return "DROP TRIGGER"
	case sqlite3.SQLITE_DROP_VIEW:
		return "DROP VIEW"
	case sqlite3.SQLITE_INSERT:
		return "INSERT"
	case sqlite3.SQLITE_PRAGMA:
		return "PRAGMA"
	case sqlite3.SQLITE_TRANSACTION:
		return "TRANSACTION"
	case sqlite3.SQLITE_UPDATE:
		return "UPDATE"
	case sqlite3.SQLITE_ATTACH:
		return "ATTACH"
	case sqlite3.SQLITE_DETACH:
		return "DETACH"
	case sqlite3.SQLITE_ALTER_TABLE:
		return "ALTER TABLE"
	case sqlite3.SQLITE_REINDEX:
		return "REINDEX"
	case sqlite3.SQLITE_ANALYZE:
		return "ANALYZE"
	case sqlite3.SQLITE_SAVEPOINT:
		return "SAVEPOINT"
	case sqlite3.SQLITE_COPY:
		return "COPY"
	}
	return fmt.Sprintf("action %d", op)
}
