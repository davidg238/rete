# R3 — Remote-debug ns-posix through porta — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an operator remote-debug a deployed ns-posix node through porta — bridging the already-shipped in-image `dbg:*` debugger over a new poll-paced, store-and-forward `debug?id=` channel and a declarative `debug` verb.

**Architecture:** porta stays a stateless relay (a `debug` verb on the command queue + a `debug?id=` channel: RRQ drains `dbg:` request lines, WRQ appends `dbg:` response lines). The debug **session lives in the keeper**, which holds a long-lived oevm VM child under `OEVM_DEBUG=1` connected by **dual pipes** (no PTY — the VM flushes stdout on every newline). The debugger, its `.ns` facade, and C-assisted breakpoints are reused **verbatim**.

**Tech Stack:** Go (porta gateway, cobra CLI, sqlite, net/http), C11 (ns-posix keeper, POSIX `fork`/`pipe`/`poll`), the oevm `.oimg` VM, Python (deploy tooling), bash (integration gates).

## Global Constraints

- **Spec:** `ns-oevm/docs/design/2026-06-25-ns-posix-keeper-r3-remote-debug-design.md`. Every task implements part of it.
- **porta wire protocol is authoritative** — `porta/docs/PROTOCOL.md`. New surface (the `debug` verb, the `debug?id=` channel) MUST be documented there (Task 8).
- **R3 is independent of the deferred R2** — use the dedicated `debug?id=` channel; do NOT route debug over `data?id=`.
- **Debug transport is always compiled in** (the keeper always wires `host_debug_vm`); it must cost nothing when no session is active (`if (k->d)` + `debug_active` guards).
- **`dbg:*` stays the node/wire protocol verbatim** — do not invent new debugger verbs; only transport them.
- **C style:** C11, `-Wall -Wextra` clean, assert-based tests with `fake_seam`, mirror existing `keeper/*.c` idioms (hand-rolled JSON via `find_val`, fixed-size buffers, `strncpy` with explicit NUL).
- **Go style:** mirror existing `internal/` idioms — verb builders in `command`, store funcs with `nullStr`, handler chokepoint returns, cobra subcommands with a testable `run*` core + `apiclient` over HTTP.
- **Commit after every green step.** Each repo commits to its own `master`; do **not** push (push only on explicit say-so).
- **One app under debug at a time** (R3); multiple concurrent sessions are out of scope.

---

## File Structure

**porta (Go):**
- `internal/command/command.go` — add `Debug(name, action)` verb builder + validation (modify).
- `internal/command/debug.go` — *(new)* small helpers if needed (kept in command.go if trivial).
- `internal/store/debug.go` — *(new)* `debug_request` queue + `debug_response` log funcs.
- `internal/store/store.go` — add the two table DDLs to the `schema` const (modify).
- `internal/handler/handler.go` — add `debug?id=` RRQ (`readDebug`) + WRQ (`writeDebug`) + `Complete`/`AcceptWrite` branches (modify).
- `internal/control/control.go` — `DebugAttach`/`DebugDetach`/`DebugSend`/`DebugResponses` (modify).
- `internal/apisrv/commands.go` — add `"debug"` case to `dispatch` (modify).
- `internal/apisrv/debug.go` — *(new)* `POST .../debug/send`, `GET .../debug/responses` routes.
- `internal/apisrv/apisrv.go` — register the new routes (modify).
- `devsdk/apiclient/client.go` — `DebugAttach`/`DebugDetach`/`DebugSend`/`DebugResponses` (modify).
- `internal/portacli/debug.go` — *(new)* `porta debug attach|detach|send|poll` cobra subcommands.
- `internal/portacli/root.go` — register `newDebugCmd()` (modify).
- `docs/PROTOCOL.md` — document the `debug` verb + `debug?id=` channel (modify).

**ns-posix (C):**
- `keeper/cmd.h` / `keeper/cmd.c` — `CMD_DEBUG` + `action` field + parse (modify).
- `keeper/seam.h` — add `struct debug_vm` (modify).
- `keeper/keeper.h` / `keeper/keeper.c` — session state, `keeper_attach_debug`, `dispatch(CMD_DEBUG)`, per-poll shuttle (modify).
- `posix/oimg_debug_vm.c` — *(new)* dual-pipe long-lived VM child + `host_debug_vm()`.
- `posix/keeperd.c` — wire `host_debug_vm` via `keeper_attach_debug` (modify).
- `tests/fake_seam.h` / `tests/fake_seam.c` — `struct fake_debug_vm` + `debug_vm_make` (modify).
- `tests/test_cmd.c` — debug-verb parse asserts (modify).
- `tests/test_debug.c` — *(new)* keeper debug session + shuttle (fakes).
- `tests/test_debug_vm.sh` — *(new)* real-oevm dual-pipe round-trip.
- `tests/porta_debug_integration.sh` — *(new)* end-to-end over porta.
- `Makefile` — add `oimg_debug_vm.c` to the `keeperd` target (modify).

---

# PHASE 1 — porta protocol (Go). Independently testable; no keeper needed.

## Task 1: `debug` verb in the command codec + API dispatch

**Files:**
- Modify: `internal/command/command.go`
- Modify: `internal/apisrv/commands.go`
- Test: `internal/command/command_test.go`, `internal/apisrv/commands_test.go`

**Interfaces:**
- Produces: `command.Debug(name, action string) (Command, error)` — `Command{Verb:"debug", ArgsJSON:`{"action":"attach","name":"<app>"}`}`. Valid actions: `attach`, `detach`.
- Consumes: existing `command.Command`, `EncodeWire`; `apisrv` `dispatch`, `control`.

- [ ] **Step 1: Write the failing test for the verb builder**

In `internal/command/command_test.go`:
```go
func TestDebugVerb(t *testing.T) {
	c, err := Debug("blink", "attach")
	if err != nil || c.Verb != "debug" {
		t.Fatalf("Debug: verb=%q err=%v", c.Verb, err)
	}
	wire := string(EncodeWire(c.Verb, c.ArgsJSON))
	if wire != `{"action":"attach","name":"blink","verb":"debug"}` {
		t.Fatalf("wire = %s", wire)
	}
	if _, err := Debug("blink", "frob"); err == nil {
		t.Fatal("expected error on invalid action")
	}
}
```

- [ ] **Step 2: Run it; expect FAIL** — `go test -run TestDebugVerb ./internal/command -v` → `undefined: Debug`.

- [ ] **Step 3: Implement the builder** in `internal/command/command.go` (mirror `Stop`/`Reboot`):
```go
// Debug enqueues a declarative debug session goal: action ∈ {attach, detach}.
// attach launches app `name` under the in-image debugger (long-lived);
// detach tears the session down.
func Debug(name, action string) (Command, error) {
	if action != "attach" && action != "detach" {
		return Command{}, fmt.Errorf("invalid debug action %q (expected attach|detach)", action)
	}
	b, _ := json.Marshal(map[string]string{"name": name, "action": action})
	return Command{Verb: "debug", ArgsJSON: string(b)}, nil
}
```
(If `fmt` is not yet imported in this file, add it; `SetForward` already uses it, so it is.)

- [ ] **Step 4: Run it; expect PASS** — `go test -run TestDebugVerb ./internal/command -v`.

- [ ] **Step 5: Write the failing dispatch test** — add a case to the table in `internal/apisrv/commands_test.go`'s `TestPostCommandVerbs`:
```go
{"debug-attach", `{"verb":"debug","args":{"name":"blink","action":"attach"}}`, "debug"},
```

- [ ] **Step 6: Run it; expect FAIL** — `go test -run TestPostCommandVerbs ./internal/apisrv -v` → `unknown verb "debug"`.

- [ ] **Step 7: Add the `debug` case** to `dispatch` in `internal/apisrv/commands.go` (mirror `set-name`):
```go
	case "debug":
		var a struct {
			Name   string `json:"name"`
			Action string `json:"action"`
		}
		if err := decodeArgs(req.Args, &a); err != nil {
			return 0, err
		}
		if a.Name == "" {
			return 0, fmt.Errorf("debug requires name")
		}
		return control.Debug(h.st, id, a.Name, a.Action, "api", now)
```

- [ ] **Step 8: Add the `control.Debug` shim** in `internal/control/control.go` (mirror `Reboot`/`Set`):
```go
// Debug enqueues a declarative debug session goal (action attach|detach).
func Debug(st *store.Store, id, name, action, issuedBy string, now int64) (int64, error) {
	c, err := command.Debug(name, action)
	if err != nil {
		return 0, err
	}
	return st.EnqueueCommand(id, c.Verb, c.ArgsJSON, issuedBy, now)
}
```

- [ ] **Step 9: Run it; expect PASS** — `go test -run TestPostCommandVerbs ./internal/apisrv -v`.

- [ ] **Step 10: Commit**
```bash
cd /home/david/workspace/porta
git add internal/command/command.go internal/command/command_test.go internal/apisrv/commands.go internal/apisrv/commands_test.go internal/control/control.go
git commit -m "feat(command): add declarative debug verb (attach/detach) + API dispatch

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `debug_request` queue + `debug_response` log store

**Files:**
- Modify: `internal/store/store.go` (add two tables to `schema`)
- Create: `internal/store/debug.go`
- Test: `internal/store/debug_test.go`

**Interfaces:**
- Produces:
  - `EnqueueDebugRequest(deviceID, line string, now int64) (int64, error)`
  - `NextUndeliveredDebugRequest(deviceID string) (*DebugRequest, error)` — oldest with `delivered_at IS NULL`, `nil` when drained.
  - `MarkDebugRequestDelivered(id, now int64) error`
  - `InsertDebugResponse(deviceID string, ts, seq int64, line string) error`
  - `DebugResponsesAfter(deviceID string, after int64, limit int) ([]DebugResponse, error)` — `id > after`, ordered by `id`.
  - Types `DebugRequest{ID int64; Line string; DeliveredAt sql.NullInt64}`, `DebugResponse{ID, TS, Seq int64; Line string}`.

- [ ] **Step 1: Add the table DDL** to the `schema` const in `internal/store/store.go` (append before the closing backtick, after `data_log`'s index):
```sql
CREATE TABLE IF NOT EXISTS debug_request (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT,
  line TEXT,
  issued_at INTEGER,
  delivered_at INTEGER
);
CREATE TABLE IF NOT EXISTS debug_response (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT,
  ts INTEGER,
  seq INTEGER,
  line TEXT
);
CREATE INDEX IF NOT EXISTS idx_dbgreq_device ON debug_request(device_id, delivered_at);
CREATE INDEX IF NOT EXISTS idx_dbgresp_device ON debug_response(device_id, id);
```

- [ ] **Step 2: Write the failing test** `internal/store/debug_test.go`:
```go
package store

import "testing"

func TestDebugRequestFIFOAndDeliver(t *testing.T) {
	st := openTmp(t)
	id1, err := st.EnqueueDebugRequest("dev", "dbg:methods", 100)
	if err != nil {
		t.Fatal(err)
	}
	st.EnqueueDebugRequest("dev", "dbg:continue", 101)

	r, _ := st.NextUndeliveredDebugRequest("dev")
	if r == nil || r.ID != id1 || r.Line != "dbg:methods" {
		t.Fatalf("FIFO wrong: %+v", r)
	}
	if err := st.MarkDebugRequestDelivered(r.ID, 200); err != nil {
		t.Fatal(err)
	}
	r, _ = st.NextUndeliveredDebugRequest("dev")
	if r == nil || r.Line != "dbg:continue" {
		t.Fatalf("after deliver, next should be continue: %+v", r)
	}
}

func TestDebugResponseAfterCursor(t *testing.T) {
	st := openTmp(t)
	st.InsertDebugResponse("dev", 10, 0, "dbg:ready")
	st.InsertDebugResponse("dev", 10, 1, "dbg:ok methods")
	all, _ := st.DebugResponsesAfter("dev", 0, 0)
	if len(all) != 2 || all[0].Line != "dbg:ready" {
		t.Fatalf("after(0) = %+v", all)
	}
	rest, _ := st.DebugResponsesAfter("dev", all[0].ID, 0)
	if len(rest) != 1 || rest[0].Line != "dbg:ok methods" {
		t.Fatalf("after(first) = %+v", rest)
	}
}
```

- [ ] **Step 3: Run it; expect FAIL** — `go test ./internal/store -run TestDebug -v` → undefined funcs.

- [ ] **Step 4: Implement** `internal/store/debug.go` (mirror `EnqueueCommand`/`NextUndelivered`/`QueryDataAfter`):
```go
// Copyright (c) 2026 Ekorau LLC

// internal/store/debug.go — the debug?id= channel backing store: a per-node
// request queue (drained like commands) + an append-only response log
// (tailed by id like data_log). porta stays a stateless relay; the debug
// session lives in the keeper.
package store

import (
	"database/sql"
	"errors"
)

type DebugRequest struct {
	ID          int64
	Line        string
	DeliveredAt sql.NullInt64
}

type DebugResponse struct {
	ID   int64
	TS   int64
	Seq  int64
	Line string
}

func (s *Store) EnqueueDebugRequest(deviceID, line string, now int64) (int64, error) {
	res, err := s.db.Exec(
		`INSERT INTO debug_request (device_id, line, issued_at, delivered_at)
		 VALUES (?, ?, ?, NULL)`, deviceID, line, now)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func (s *Store) NextUndeliveredDebugRequest(deviceID string) (*DebugRequest, error) {
	var r DebugRequest
	err := s.db.QueryRow(
		`SELECT id, line, delivered_at FROM debug_request
		 WHERE device_id = ? AND delivered_at IS NULL ORDER BY id LIMIT 1`,
		deviceID).Scan(&r.ID, &r.Line, &r.DeliveredAt)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &r, nil
}

func (s *Store) MarkDebugRequestDelivered(id, now int64) error {
	_, err := s.db.Exec(`UPDATE debug_request SET delivered_at = ? WHERE id = ?`, now, id)
	return err
}

func (s *Store) InsertDebugResponse(deviceID string, ts, seq int64, line string) error {
	_, err := s.db.Exec(
		`INSERT INTO debug_response (device_id, ts, seq, line) VALUES (?, ?, ?, ?)`,
		deviceID, ts, seq, line)
	return err
}

func (s *Store) DebugResponsesAfter(deviceID string, after int64, limit int) ([]DebugResponse, error) {
	q := `SELECT id, ts, seq, COALESCE(line,'') FROM debug_response
		  WHERE device_id = ? AND id > ? ORDER BY id`
	args := []any{deviceID, after}
	if limit > 0 {
		q += ` LIMIT ?`
		args = append(args, limit)
	}
	rows, err := s.db.Query(q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []DebugResponse
	for rows.Next() {
		var r DebugResponse
		if err := rows.Scan(&r.ID, &r.TS, &r.Seq, &r.Line); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}
```

- [ ] **Step 5: Run it; expect PASS** — `go test ./internal/store -run TestDebug -v`.

- [ ] **Step 6: Commit**
```bash
cd /home/david/workspace/porta
git add internal/store/store.go internal/store/debug.go internal/store/debug_test.go
git commit -m "feat(store): debug_request queue + debug_response log for debug?id=

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: `debug?id=` TFTP channel in the handler

**Files:**
- Modify: `internal/handler/handler.go`
- Test: `internal/handler/handler_test.go` (or the existing handler test file — match what's there)

**Interfaces:**
- Consumes: `store.NextUndeliveredDebugRequest`, `MarkDebugRequestDelivered`, `InsertDebugResponse` (Task 2).
- Produces: RRQ `debug?id=<id>` drains one request line (empty body = drained); WRQ `debug?id=<id>` appends response lines (newline-split). `Complete` marks the delivered debug request.

- [ ] **Step 1: Write the failing handler test** in the handler test file (mirror the existing report/data tests; use the same handler constructor the file already uses — locate it, e.g. a `newTestHandler`/store-open helper):
```go
func TestDebugChannel(t *testing.T) {
	h, st := newHandlerTest(t) // use whatever the file's helper is named
	st.TouchNode("dev", "1.2.3.4:5", 1000)
	st.EnqueueDebugRequest("dev", "dbg:methods", 1000)

	// RRQ drains the queued request line.
	body, err := h.Read("debug?id=dev", "1.2.3.4:5")
	if err != nil || string(body) != "dbg:methods" {
		t.Fatalf("read debug = %q err=%v", body, err)
	}
	// Complete marks it delivered → next RRQ drains empty.
	h.Complete(tftp.OpRRQ, "debug?id=dev", "1.2.3.4:5", true)
	body, err = h.Read("debug?id=dev", "1.2.3.4:5")
	if err != nil || body != nil {
		t.Fatalf("expected drain sentinel, got %q err=%v", body, err)
	}
	// WRQ ingests response lines.
	if err := h.AcceptWrite("debug?id=dev", "1.2.3.4:5"); err != nil {
		t.Fatalf("AcceptWrite: %v", err)
	}
	if err := h.Write("debug?id=dev", "1.2.3.4:5", []byte("dbg:ready\ndbg:ok methods\n")); err != nil {
		t.Fatalf("write debug: %v", err)
	}
	resp, _ := st.DebugResponsesAfter("dev", 0, 0)
	if len(resp) != 2 || resp[1].Line != "dbg:ok methods" {
		t.Fatalf("responses = %+v", resp)
	}
}
```
(Ensure `tftp` is imported in the test file; the existing `Complete` tests already import it.)

- [ ] **Step 2: Run it; expect FAIL** — `go test ./internal/handler -run TestDebugChannel -v` → `file not found: debug`.

- [ ] **Step 3: Add the RRQ branch** to `Read`'s switch in `handler.go` (after `case "payload"`):
```go
	case "debug":
		return h.readDebug(params["id"])
```

- [ ] **Step 4: Add `readDebug`** (mirror `readCommands` — same chokepoint contract):
```go
// readDebug is the debug-request chokepoint, mirroring readCommands: (nil,err)
// → TFTP ERROR; (nil,nil) → drained; (bytes,nil) → one dbg: request line.
func (h *Handler) readDebug(id string) ([]byte, error) {
	if id == "" {
		return nil, fmt.Errorf("debug: missing id")
	}
	r, err := h.store.NextUndeliveredDebugRequest(id)
	if err != nil {
		return nil, err
	}
	if r == nil {
		return nil, nil // drain sentinel
	}
	return []byte(r.Line), nil
}
```

- [ ] **Step 5: Allow `debug` WRQ** in `AcceptWrite` and route it in `Write`:
```go
// in AcceptWrite:
	if base != "report" && base != "data" && base != "debug" {
		return fmt.Errorf("access denied: %s", base)
	}
// in Write's switch:
	case "debug":
		return h.writeDebug(id, peer, data)
```

- [ ] **Step 6: Add `writeDebug`** (mirror `writeData` — best-effort per line):
```go
// writeDebug ingests newline-delimited dbg: response lines into debug_response.
// Best-effort per line: blank lines skipped; per-row insert errors logged.
func (h *Handler) writeDebug(id, peer string, data []byte) error {
	if err := h.store.TouchNode(id, peer, h.now()); err != nil {
		return err
	}
	now := h.now()
	seq := int64(0)
	for _, raw := range bytes.Split(data, []byte("\n")) {
		line := strings.TrimRight(string(raw), "\r")
		if line == "" {
			continue
		}
		if err := h.store.InsertDebugResponse(id, now, seq, line); err != nil {
			h.log("porta: debug response insert error for %s seq=%d: %v", id, seq, err)
			continue
		}
		seq++
	}
	return nil
}
```

- [ ] **Step 7: Mark delivered on RRQ Complete** — extend `Complete` in `handler.go` so a successful `debug` RRQ marks its request delivered (the current body early-returns unless `base=="commands"`; broaden it):
```go
// Replace the `if base != "commands" { return }` guard with a per-base switch:
	id := params["id"]
	if id == "" {
		return
	}
	switch base {
	case "commands":
		cmd, err := h.store.NextUndelivered(id)
		if err != nil || cmd == nil {
			return
		}
		_ = h.store.MarkDelivered(cmd.ID, h.now())
	case "debug":
		r, err := h.store.NextUndeliveredDebugRequest(id)
		if err != nil || r == nil {
			return
		}
		_ = h.store.MarkDebugRequestDelivered(r.ID, h.now())
	}
```
(Keep the existing `if !ok || op != tftp.OpRRQ { return }` guard and the `base, params := parseResource(resource)` line above this.)

- [ ] **Step 8: Run it; expect PASS** — `go test ./internal/handler -run TestDebugChannel -v`, then the full package `go test ./internal/handler -v`.

- [ ] **Step 9: Commit**
```bash
cd /home/david/workspace/porta
git add internal/handler/handler.go internal/handler/handler_test.go
git commit -m "feat(handler): debug?id= channel (RRQ drain requests, WRQ ingest responses)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: debug HTTP API + apiclient + `porta debug` CLI

**Files:**
- Create: `internal/apisrv/debug.go`
- Modify: `internal/apisrv/apisrv.go`
- Modify: `internal/control/control.go`
- Modify: `devsdk/apiclient/client.go`
- Create: `internal/portacli/debug.go`
- Modify: `internal/portacli/root.go`
- Test: `internal/portacli/debug_test.go`

**Interfaces:**
- Produces (control): `DebugSend(st, id, line, issuedBy, now) (int64, error)` (enqueues a request line); `DebugResponses(st, id, after, limit) ([]store.DebugResponse, error)`.
- Produces (apiclient): `DebugAttach(sel, app)`, `DebugDetach(sel)`, `DebugSend(sel, line)`, `DebugResponses(sel, after) ([]DebugLine, error)` where `DebugLine{ID int64; Line string}`.
- Produces (CLI): `porta debug attach <app> --device`, `detach`, `send <line>`, `poll [--after N]`.

- [ ] **Step 1: control helpers** in `internal/control/control.go`:
```go
// DebugSend enqueues one dbg: request line onto the node's debug channel.
func DebugSend(st *store.Store, id, line, issuedBy string, now int64) (int64, error) {
	return st.EnqueueDebugRequest(id, line, now)
}

// DebugResponses returns dbg: response lines with id > after.
func DebugResponses(st *store.Store, id string, after int64, limit int) ([]store.DebugResponse, error) {
	return st.DebugResponsesAfter(id, after, limit)
}
```

- [ ] **Step 2: API routes** `internal/apisrv/debug.go` (mirror the resolve-sel + envelope pattern used by `commands.go`/`handleTelemetry`; check the exact helper names — e.g. `h.resolve(sel)`, `writeOK`, `writeErr` — and reuse them verbatim):
```go
// Copyright (c) 2026 Ekorau LLC
package apisrv

import (
	"net/http"
	"strconv"

	"github.com/davidg238/porta/internal/control"
)

// handleDebugSend: POST /api/nodes/{sel}/debug/send  body {"line":"dbg:methods"}
func (h *Handler) handleDebugSend(w http.ResponseWriter, r *http.Request) {
	id, ok := h.resolveSel(w, r) // use the file's existing sel-resolver
	if !ok {
		return
	}
	var body struct {
		Line string `json:"line"`
	}
	if !decodeBody(w, r, &body) { // use the file's existing body decoder
		return
	}
	cid, err := control.DebugSend(h.st, id, body.Line, "api", h.now())
	if err != nil {
		writeErr(w, http.StatusBadRequest, err)
		return
	}
	writeOK(w, map[string]any{"request_id": cid, "node_id": id})
}

// handleDebugResponses: GET /api/nodes/{sel}/debug/responses?after=N
func (h *Handler) handleDebugResponses(w http.ResponseWriter, r *http.Request) {
	id, ok := h.resolveSel(w, r)
	if !ok {
		return
	}
	after, _ := strconv.ParseInt(r.URL.Query().Get("after"), 10, 64)
	rows, err := control.DebugResponses(h.st, id, after, 0)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err)
		return
	}
	out := make([]map[string]any, 0, len(rows))
	for _, x := range rows {
		out = append(out, map[string]any{"id": x.ID, "line": x.Line})
	}
	writeOK(w, map[string]any{"node_id": id, "responses": out})
}
```
> **Note for implementer:** open `internal/apisrv/commands.go` + `apisrv.go` first and copy the *actual* names of the sel-resolver, body-decoder, and OK/Err envelope writers. The names above (`resolveSel`, `decodeBody`, `writeOK`, `writeErr`) are placeholders for whatever this codebase already uses — match them exactly so the file compiles.

- [ ] **Step 3: Register routes** in `apisrv.go`'s `Register`:
```go
	mux.HandleFunc("POST /api/nodes/{sel}/debug/send", recoverer(h.handleDebugSend))
	mux.HandleFunc("GET /api/nodes/{sel}/debug/responses", recoverer(h.handleDebugResponses))
```
(`debug attach`/`detach` reuse the existing `POST /api/nodes/{sel}/commands` route with verb `debug` from Task 1 — no new route needed.)

- [ ] **Step 4: apiclient methods** in `devsdk/apiclient/client.go` (mirror `Command` for attach/detach; add a GET helper for responses):
```go
type DebugLine struct {
	ID   int64  `json:"id"`
	Line string `json:"line"`
}

func (c *Client) DebugAttach(sel, app string) (int64, string, error) {
	return c.Command(sel, "debug", map[string]any{"name": app, "action": "attach"})
}
func (c *Client) DebugDetach(sel, app string) (int64, string, error) {
	return c.Command(sel, "debug", map[string]any{"name": app, "action": "detach"})
}
func (c *Client) DebugSend(sel, line string) error {
	body, _ := json.Marshal(map[string]string{"line": line})
	req, err := http.NewRequest("POST",
		c.baseURL+"/api/nodes/"+url.PathEscape(sel)+"/debug/send", bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	_, err = c.do(req)
	return err
}
func (c *Client) DebugResponses(sel string, after int64) ([]DebugLine, error) {
	req, err := http.NewRequest("GET",
		c.baseURL+"/api/nodes/"+url.PathEscape(sel)+"/debug/responses?after="+
			strconv.FormatInt(after, 10), nil)
	if err != nil {
		return nil, err
	}
	data, err := c.do(req)
	if err != nil {
		return nil, err
	}
	var r struct {
		Responses []DebugLine `json:"responses"`
	}
	if err := json.Unmarshal(data, &r); err != nil {
		return nil, err
	}
	return r.Responses, nil
}
```
(Add `"strconv"` to the import block if not present.)

- [ ] **Step 5: Write the failing CLI test** `internal/portacli/debug_test.go` (mirror `mutate_test.go`'s `newClientServer`):
```go
package portacli

import (
	"bytes"
	"testing"
)

func TestDebugAttachAndSend(t *testing.T) {
	c, st := newClientServer(t)
	st.TouchNode("dev", "1.2.3.4:5", 1000)

	var out bytes.Buffer
	if err := runDebugAttach(&out, c, "dev", "blink"); err != nil {
		t.Fatal(err)
	}
	cmd, _ := st.NextUndelivered("dev")
	if cmd == nil || cmd.Verb != "debug" {
		t.Fatalf("attach did not enqueue debug verb: %+v", cmd)
	}
	if err := runDebugSend(&out, c, "dev", "dbg:methods"); err != nil {
		t.Fatal(err)
	}
	r, _ := st.NextUndeliveredDebugRequest("dev")
	if r == nil || r.Line != "dbg:methods" {
		t.Fatalf("send did not enqueue request: %+v", r)
	}
}
```

- [ ] **Step 6: Run it; expect FAIL** — `go test ./internal/portacli -run TestDebugAttachAndSend -v`.

- [ ] **Step 7: Implement the CLI** `internal/portacli/debug.go` (mirror `mutate.go`):
```go
// Copyright (c) 2026 Ekorau LLC
package portacli

import (
	"fmt"
	"io"

	"github.com/davidg238/porta/devsdk/apiclient"
	"github.com/spf13/cobra"
)

func runDebugAttach(out io.Writer, c *apiclient.Client, sel, app string) error {
	cid, node, err := c.DebugAttach(sel, app)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "%s: debug attach %s (command #%d)\n", node, app, cid)
	return nil
}
func runDebugDetach(out io.Writer, c *apiclient.Client, sel, app string) error {
	cid, node, err := c.DebugDetach(sel, app)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "%s: debug detach %s (command #%d)\n", node, app, cid)
	return nil
}
func runDebugSend(out io.Writer, c *apiclient.Client, sel, line string) error {
	if err := c.DebugSend(sel, line); err != nil {
		return err
	}
	fmt.Fprintf(out, "sent: %s\n", line)
	return nil
}
func runDebugPoll(out io.Writer, c *apiclient.Client, sel string, after int64) (int64, error) {
	rows, err := c.DebugResponses(sel, after)
	if err != nil {
		return after, err
	}
	for _, r := range rows {
		fmt.Fprintln(out, r.Line)
		after = r.ID
	}
	return after, nil
}

func newDebugCmd() *cobra.Command {
	var device string
	cmd := &cobra.Command{Use: "debug", Short: "Remote-debug a node over porta (dbg:* bridge)"}
	cmd.PersistentFlags().StringVar(&device, "device", "", "target node id or name")

	attach := &cobra.Command{Use: "attach <app>", Short: "Attach the debugger to an app", Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, a []string) error {
			return runDebugAttach(cmd.OutOrStdout(), apiclient.New(serverURL()), device, a[0])
		}}
	detach := &cobra.Command{Use: "detach <app>", Short: "Detach the debugger", Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, a []string) error {
			return runDebugDetach(cmd.OutOrStdout(), apiclient.New(serverURL()), device, a[0])
		}}
	send := &cobra.Command{Use: "send <dbg-line>", Short: "Send one dbg: request line", Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, a []string) error {
			return runDebugSend(cmd.OutOrStdout(), apiclient.New(serverURL()), device, a[0])
		}}
	var after int64
	poll := &cobra.Command{Use: "poll", Short: "Print new dbg: response lines (id > --after)",
		RunE: func(cmd *cobra.Command, _ []string) error {
			_, err := runDebugPoll(cmd.OutOrStdout(), apiclient.New(serverURL()), device, after)
			return err
		}}
	poll.Flags().Int64Var(&after, "after", 0, "only responses with id greater than this")
	cmd.AddCommand(attach, detach, send, poll)
	return cmd
}
```

- [ ] **Step 8: Register** in `internal/portacli/root.go`'s `root.AddCommand(...)`: add `newDebugCmd(),`.

- [ ] **Step 9: Run it; expect PASS** — `go test ./internal/portacli -run TestDebugAttachAndSend -v`, then build: `go build ./cmd/porta`.

- [ ] **Step 10: Full porta test sweep** — `go test ./... ` (expect all green).

- [ ] **Step 11: Commit**
```bash
cd /home/david/workspace/porta
git add internal/apisrv/debug.go internal/apisrv/apisrv.go internal/control/control.go devsdk/apiclient/client.go internal/portacli/debug.go internal/portacli/debug_test.go internal/portacli/root.go
git commit -m "feat(api,cli): debug send/responses endpoints + porta debug CLI

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

# PHASE 2 — ns-posix keeper (C). Independently testable with fakes + real oevm.

## Task 5: `CMD_DEBUG` parse in the keeper command decoder

**Files:**
- Modify: `keeper/cmd.h`, `keeper/cmd.c`
- Test: `tests/test_cmd.c`

**Interfaces:**
- Produces: `enum cmd_verb` gains `CMD_DEBUG`; `struct command` gains `char action[16]`; `cmd_parse` decodes `{"verb":"debug","name":"<app>","action":"attach|detach"}` into `out->verb=CMD_DEBUG`, `out->name`, `out->action`.

- [ ] **Step 1: Add the failing test** to `tests/test_cmd.c` (before `return 0;`):
```c
    // debug verb: name + action
    const char *dbg = "{\"verb\":\"debug\",\"name\":\"blink\",\"action\":\"attach\"}";
    assert(cmd_parse(dbg, strlen(dbg), &c) == 0);
    assert(c.verb == CMD_DEBUG);
    assert(strcmp(c.name, "blink") == 0);
    assert(strcmp(c.action, "attach") == 0);
```

- [ ] **Step 2: Run it; expect FAIL** — `make build/test_cmd && ./build/test_cmd` → compile error (`CMD_DEBUG`/`action` undefined).

- [ ] **Step 3: Extend `keeper/cmd.h`** — add the enum value and field:
```c
enum cmd_verb { CMD_UNKNOWN=0, CMD_RUN, CMD_STOP, CMD_REBOOT,
                CMD_SET_MODE, CMD_SET_NAME, CMD_SET_FORWARD, CMD_SET, CMD_DEBUG };
```
and in `struct command`, after `newname`:
```c
    char     action[16];      // debug: "attach" | "detach"
```

- [ ] **Step 4: Extend `keeper/cmd.c`** — add the verb mapping (after the `set-forward` line in the `if/else` chain):
```c
    else if (!strcmp(verb, "debug"))      out->verb = CMD_DEBUG;
```
and the field parse (add a case to the `switch (out->verb)`):
```c
    case CMD_DEBUG:
        get_str(json, "name", out->name, sizeof out->name);
        get_str(json, "action", out->action, sizeof out->action);
        break;
```

- [ ] **Step 5: Run it; expect PASS** — `make build/test_cmd && ./build/test_cmd && echo OK`.

- [ ] **Step 6: Commit**
```bash
cd /home/david/workspace/ns-posix
git add keeper/cmd.h keeper/cmd.c tests/test_cmd.c
git commit -m "feat(cmd): parse the debug verb (name + attach/detach action)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: `debug_vm` seam + keeper session state + per-poll shuttle (fakes)

**Files:**
- Modify: `keeper/seam.h`, `keeper/keeper.h`, `keeper/keeper.c`
- Modify: `tests/fake_seam.h`, `tests/fake_seam.c`
- Create: `tests/test_debug.c`

**Interfaces:**
- Produces (`keeper/seam.h`):
```c
struct debug_vm {
    int (*start)(void *ctx, const char *name, const uint8_t *img, size_t len); // 0 ok, <0 fail
    int (*write)(void *ctx, const char *line);                                  // 0 ok, <0 fail
    int (*drain)(void *ctx, char *out, size_t cap, int timeout_ms);             // >=0 bytes (0 timeout), <0 child gone
    int (*stop)(void *ctx);                                                      // 0 ok
    void *ctx;
};
```
- Produces (`keeper/keeper.h`): `void keeper_attach_debug(struct keeper *k, struct debug_vm *d);` and `struct keeper` gains `struct debug_vm *d; char debug_app[64]; int debug_active;`.
- Consumes: `tftp_get`/`tftp_put` for `debug?id=`; the cached image via `find_app` + `img_get`.

- [ ] **Step 1: Add `struct debug_vm`** to `keeper/seam.h` (after `struct vm`).

- [ ] **Step 2: Extend `keeper/keeper.h`** — add fields to `struct keeper` and the setter decl:
```c
    struct debug_vm *d;        // optional debug transport (NULL = no debug)
    char debug_app[64];        // app currently under debug ("" = none)
    int  debug_active;         // 1 while a session is live
```
```c
void keeper_attach_debug(struct keeper *k, struct debug_vm *d);
```

- [ ] **Step 3: Add the fake** to `tests/fake_seam.h`:
```c
// Fake debug VM: records start/write/stop; hands back scripted drain output once.
struct fake_debug_vm {
    int start_calls; char last_name[64]; int last_len;
    char written[16][128]; int write_n;
    const char *drain_script; int drained;   // returned once, then 0 (timeout)
    int alive;                                // when 0, drain returns <0 (gone)
    int stop_calls;
};
struct debug_vm fake_debug_vm_make(struct fake_debug_vm *f);
```

- [ ] **Step 4: Implement the fake** in `tests/fake_seam.c`:
```c
static int fd_start(void *ctx, const char *name, const uint8_t *img, size_t len) {
    struct fake_debug_vm *f = ctx; (void)img; f->start_calls++;
    strncpy(f->last_name, name, sizeof f->last_name - 1); f->last_len = (int)len;
    f->alive = 1; return 0;
}
static int fd_write(void *ctx, const char *line) {
    struct fake_debug_vm *f = ctx;
    if (f->write_n < 16) { strncpy(f->written[f->write_n], line, 127); f->write_n++; }
    return 0;
}
static int fd_drain(void *ctx, char *out, size_t cap, int to) {
    (void)to; struct fake_debug_vm *f = ctx;
    if (!f->alive) return -1;                 // child gone
    if (f->drain_script && !f->drained) {
        f->drained = 1;
        size_t n = strlen(f->drain_script); if (n > cap) n = cap;
        memcpy(out, f->drain_script, n); return (int)n;
    }
    return 0;                                  // timeout (still alive)
}
static int fd_stop(void *ctx) { struct fake_debug_vm *f = ctx; f->stop_calls++; f->alive = 0; return 0; }
struct debug_vm fake_debug_vm_make(struct fake_debug_vm *f) {
    struct debug_vm d = { fd_start, fd_write, fd_drain, fd_stop, f }; return d;
}
```

- [ ] **Step 5: Write the failing keeper test** `tests/test_debug.c`:
```c
#include <assert.h>
#include <string.h>
#include "keeper.h"
#include "fake_seam.h"

// One TFTP DATA block (opcode 3, block 1) carrying `s`.
static uint8_t *blk(const char *s, uint8_t *buf, size_t *len) {
    size_t n = strlen(s); buf[0]=0;buf[1]=3;buf[2]=0;buf[3]=1;
    memcpy(buf+4, s, n); *len = 4+n; return buf;
}

int main(void) {
    // Image for app "tgt" pre-cached (crc 0x1234).
    struct fake_store fs = { .kv_len=-1, .img_len=-1 };
    uint8_t img[4] = {1,2,3,4}; fs.img_crc = 0x1234; memcpy(fs.img, img, 4); fs.img_len = 4;

    // Transport script per poll:
    //  poll#1: commands RRQ -> debug attach tgt; commands drain; (report WRQ);
    //          debug?id= RRQ -> "dbg:methods"; debug drain (empty); (debug WRQ).
    static uint8_t b[8][600]; size_t L[8];
    const char *attach = "{\"verb\":\"debug\",\"name\":\"tgt\",\"action\":\"attach\"}";
    struct fake_transport ft = {0};
    int i = 0;
    ft.replies[i] = blk(attach, b[i], &L[i]); ft.reply_lens[i]=L[i]; i++;   // commands -> attach
    ft.replies[i] = blk("", b[i], &L[i]); ft.reply_lens[i]=L[i]; i++;       // commands drain
    ft.replies[i] = blk("dbg:methods", b[i], &L[i]); ft.reply_lens[i]=L[i]; i++; // debug req
    ft.replies[i] = blk("", b[i], &L[i]); ft.reply_lens[i]=L[i]; i++;       // debug drain
    ft.reply_n = i;

    // Make the run command's app already known so attach can find its crc:
    // pre-seed via a run first is heavy; instead the keeper finds crc from the
    // image cache by name is not possible — so the debug test pre-installs the
    // app through a run command. Simpler: drive attach AFTER a run in one poll.
    // For unit scope we instead pre-register the app in keeper state:
    struct fake_debug_vm fd = { .drain_script = "dbg:ready\ndbg:ok methods\n" };
    struct transport t = fake_transport_make(&ft);
    struct store     s = fake_store_make(&fs);
    struct fake_vm   fv = { .run_rc = 0 };
    struct vm        v = fake_vm_make(&fv);
    struct debug_vm  d = fake_debug_vm_make(&fd);
    struct keeper_config cfg = { .device_id="aabbccddeeff0001", .chip="host", .loop_sleep_s=2 };

    struct keeper k; keeper_init(&k, &t, &s, &v, &cfg);
    keeper_attach_debug(&k, &d);
    // Pre-register app "tgt" with crc 0x1234 so attach can resolve its image.
    strncpy(k.apps[0].name, "tgt", sizeof k.apps[0].name - 1);
    k.apps[0].crc = 0x1234; k.app_n = 1;

    assert(keeper_poll_once(&k) == 0);

    // attach launched the debug VM with the cached image, marked active.
    assert(fd.start_calls == 1);
    assert(strcmp(fd.last_name, "tgt") == 0 && fd.last_len == 4);
    assert(k.debug_active == 1 && strcmp(k.debug_app, "tgt") == 0);
    // the drained request was written to the VM.
    assert(fd.write_n == 1 && strcmp(fd.written[0], "dbg:methods") == 0);
    // a debug?id= WRQ carried the VM's response lines.
    int saw_debug_wrq = 0;
    for (int j = 0; j < ft.sent_n; j++)
        if (ft.sent[j][0]==0 && ft.sent[j][1]==2 &&
            strstr((char*)ft.sent[j]+2, "debug?id=aabbccddeeff0001")) saw_debug_wrq = 1;
    assert(saw_debug_wrq);
    return 0;
}
```
> **Implementer note:** the `_Static_assert`/helper style mirrors `tests/test_keeper.c`. If resolving the attach image by pre-seeding `k.apps[...]` feels too white-box, an acceptable alternative is to make the first command an actual `run tgt` (image already cached) followed by the `debug attach` in the same `commands` drain — both reach `dispatch`. Keep whichever compiles cleanly and asserts the same observable effects.

- [ ] **Step 6: Run it; expect FAIL** — `make build/test_debug && ./build/test_debug` → undefined `keeper_attach_debug` / `fake_debug_vm_make`.

- [ ] **Step 7: Implement `keeper_attach_debug`** in `keeper/keeper.c`:
```c
void keeper_attach_debug(struct keeper *k, struct debug_vm *d) { k->d = d; }
```

- [ ] **Step 8: Implement `dispatch(CMD_DEBUG)`** — add to the `switch` in `keeper.c`:
```c
    case CMD_DEBUG: do_debug(k, c); break;
```
and define `do_debug` (above `dispatch`):
```c
// Attach: launch the named (already-installed) app under the debugger, long-lived.
// Detach: tear the session down. No-op if no debug transport is wired.
static void do_debug(struct keeper *k, const struct command *c) {
    if (!k->d) return;
    if (!strcmp(c->action, "attach")) {
        struct report_app *a = find_app(k, c->name);
        if (!a) return;                          // app not installed → ignore
        static uint8_t img[KEEPER_MAX_IMAGE];
        int n = k->s->img_get(k->s->ctx, a->crc, img, sizeof img);
        if (n < 0) return;
        if (k->d->start(k->d->ctx, c->name, img, (size_t)n) != 0) return;
        strncpy(k->debug_app, c->name, sizeof k->debug_app - 1);
        k->debug_active = 1;
    } else if (!strcmp(c->action, "detach")) {
        if (k->debug_active) { k->d->stop(k->d->ctx); }
        k->debug_active = 0; k->debug_app[0] = '\0';
    }
}
```

- [ ] **Step 9: Exclude the debug app from run-once** — at the top of `do_run`:
```c
    if (k->debug_active && !strcmp(c->name, k->debug_app)) return; // debug owns this app
```

- [ ] **Step 10: Add the per-poll shuttle** — define `debug_shuttle` and call it in `keeper_poll_once` **after** the command-drain loop and **before** `upload_report(k)`:
```c
// Shuttle one round of the debug session: drain queued dbg: requests to the VM,
// read response lines back, upload them. Ends the session if the VM exited.
static void debug_shuttle(struct keeper *k) {
    if (!k->d || !k->debug_active) return;
    char res[64]; snprintf(res, sizeof res, "debug?id=%s", k->cfg->device_id);
    // 1) feed all queued requests.
    for (int guard = 0; guard < 64; guard++) {
        uint8_t buf[512];
        int n = tftp_get(k->t, res, buf, sizeof buf - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        k->d->write(k->d->ctx, (const char *)buf);
    }
    // 2) read response lines (bounded), detecting VM exit.
    char acc[2048]; size_t off = 0; int gone = 0;
    for (int guard = 0; guard < 32 && off < sizeof acc - 256; guard++) {
        int n = k->d->drain(k->d->ctx, acc + off, sizeof acc - 1 - off, 200);
        if (n < 0) { gone = 1; break; }
        if (n == 0) break;                       // quiescent (paused or idle)
        off += (size_t)n;
    }
    // 3) upload whatever the VM said.
    if (off > 0) { acc[off] = '\0'; tftp_put(k->t, res, (const uint8_t *)acc, off); }
    // 4) if the VM exited, end the session.
    if (gone) { k->d->stop(k->d->ctx); k->debug_active = 0; k->debug_app[0] = '\0'; }
}
```
and in `keeper_poll_once`:
```c
    // ... after the for(guard) command-drain loop:
    debug_shuttle(k);
    upload_report(k);
```

- [ ] **Step 11: Run it; expect PASS** — `make build/test_debug && ./build/test_debug && echo OK`. Then the whole suite: `make test`.

- [ ] **Step 12: Commit**
```bash
cd /home/david/workspace/ns-posix
git add keeper/seam.h keeper/keeper.h keeper/keeper.c tests/fake_seam.h tests/fake_seam.c tests/test_debug.c
git commit -m "feat(keeper): debug_vm seam + session state + per-poll debug shuttle

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: posix dual-pipe debug VM + keeperd wiring (real oevm)

**Files:**
- Create: `posix/oimg_debug_vm.c`
- Modify: `posix/keeperd.c`
- Modify: `Makefile`
- Test: `tests/test_debug_vm.sh`

**Interfaces:**
- Produces: `struct debug_vm host_debug_vm(void);` implementing start/write/drain/stop over `fork` + two pipes, launching `$OEVM_IMG <tmp.oimg>` with `OEVM_DEBUG=1`.
- Consumes: `keeper_attach_debug` (Task 6).

- [ ] **Step 1: Implement `posix/oimg_debug_vm.c`** (mirror `oimg_vm.c`'s bin resolution; dual pipes + poll):
```c
// posix/oimg_debug_vm.c — struct debug_vm: a long-lived oevm VM child under
// OEVM_DEBUG=1, connected by two pipes (parent writes the child's stdin, reads
// its stdout). The VM flushes stdout on every newline, so dbg: response lines
// arrive without a PTY. A crash is contained in the child (keeper survives).
#define _POSIX_C_SOURCE 200809L
#include "seam.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

struct dbg_ctx { char bin[256]; pid_t pid; int to_child; int from_child; };

static int dbg_start(void *c, const char *name, const uint8_t *img, size_t len) {
    struct dbg_ctx *b = c; (void)name;
    const char *tmp = "/tmp/ns-posix-keeperd-debug.oimg";
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(img, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fclose(f);

    int in[2], out[2];                 // in: parent->child stdin; out: child->parent stdout
    if (pipe(in) != 0) return -1;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(in[0]);close(in[1]);close(out[0]);close(out[1]); return -1; }
    if (pid == 0) {                    // child
        dup2(in[0], 0); dup2(out[1], 1);
        close(in[0]);close(in[1]);close(out[0]);close(out[1]);
        setenv("OEVM_DEBUG", "1", 1);
        execl(b->bin, b->bin, tmp, (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    b->pid = pid; b->to_child = in[1]; b->from_child = out[0];
    fcntl(b->from_child, F_SETFL, O_NONBLOCK);
    return 0;
}

static int dbg_write(void *c, const char *line) {
    struct dbg_ctx *b = c;
    if (b->to_child < 0) return -1;
    size_t n = strlen(line);
    if (write(b->to_child, line, n) < 0) return -1;
    if (n == 0 || line[n-1] != '\n') { if (write(b->to_child, "\n", 1) < 0) return -1; }
    return 0;
}

static int child_alive(struct dbg_ctx *b) {
    if (b->pid <= 0) return 0;
    int st; pid_t r = waitpid(b->pid, &st, WNOHANG);
    return r == 0;                     // 0 => still running
}

static int dbg_drain(void *c, char *out, size_t cap, int timeout_ms) {
    struct dbg_ctx *b = c;
    if (b->from_child < 0) return -1;
    struct pollfd p = { .fd = b->from_child, .events = POLLIN };
    int pr = poll(&p, 1, timeout_ms);
    if (pr == 0) return child_alive(b) ? 0 : -1;     // timeout: 0 if alive, -1 if gone
    if (pr < 0) return 0;
    ssize_t n = read(b->from_child, out, cap);
    if (n > 0) return (int)n;
    if (n == 0) return -1;             // EOF: child closed stdout (exited)
    return child_alive(b) ? 0 : -1;
}

static int dbg_stop(void *c) {
    struct dbg_ctx *b = c;
    if (b->to_child >= 0)   { close(b->to_child);   b->to_child = -1; }
    if (b->from_child >= 0) { close(b->from_child); b->from_child = -1; }
    if (b->pid > 0) { kill(b->pid, SIGTERM); waitpid(b->pid, NULL, 0); b->pid = 0; }
    unlink("/tmp/ns-posix-keeperd-debug.oimg");
    return 0;
}

struct debug_vm host_debug_vm(void) {
    static struct dbg_ctx c = { .pid = 0, .to_child = -1, .from_child = -1 };
    const char *env = getenv("OEVM_IMG");
    if (env && env[0]) snprintf(c.bin, sizeof c.bin, "%s", env);
    else { const char *home = getenv("HOME");
           snprintf(c.bin, sizeof c.bin, "%s/workspace/oevm/oevm_img", home ? home : ""); }
    struct debug_vm d = { dbg_start, dbg_write, dbg_drain, dbg_stop, &c };
    return d;
}
```

- [ ] **Step 2: Wire it in `posix/keeperd.c`** — add the extern + attach:
```c
struct debug_vm  host_debug_vm(void);   // posix/oimg_debug_vm.c
```
```c
    struct keeper k; keeper_init(&k, &t, &s, &v, &cfg);
    static struct debug_vm d; d = host_debug_vm();
    keeper_attach_debug(&k, &d);
```

- [ ] **Step 3: Add `oimg_debug_vm.c` to the `keeperd` Makefile target**:
```makefile
keeperd: posix/keeperd.c posix/udp_transport.c posix/file_store.c posix/oimg_vm.c posix/oimg_debug_vm.c $(CORE)
	@mkdir -p build
	$(CC) $(CFLAGS) -Ikeeper $^ -o build/keeperd
```

- [ ] **Step 4: Build the daemon** — `make keeperd` (expect clean `-Wall -Wextra`).

- [ ] **Step 5: Write the real-oevm dual-pipe test** `tests/test_debug_vm.sh` (mirror `test_oimg_vm.sh`; drives `host_debug_vm` directly via a tiny harness):
```bash
#!/usr/bin/env bash
# Drive the dual-pipe debug VM seam against a REAL oevm debug image:
# start 46_debug_target under OEVM_DEBUG=1, exchange dbg: lines, assert dbg:ready
# and a breakpoint pause come back over the pipe.
set -euo pipefail
cd "$(dirname "$0")/.."
OEVM=/home/david/workspace/oevm
NSOEVM=/home/david/workspace/ns-oevm
SRC=/home/david/workspace/ns-tests/04_somns_basic/46_debug_target.ns

if [ ! -x "$OEVM/oevm_img" ]; then
  TARGET=posix
  ( cd "$OEVM" && gcc -Wall -std=c99 -Isrc \
      src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/mirror.c \
      "src/backend/$TARGET/platform_$TARGET.c" "src/backend/$TARGET/serial_$TARGET.c" \
      src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o oevm_img -lm )
fi
export OEVM_IMG="$OEVM/oevm_img"

gcc -std=c99 -I"$OEVM/src" "$OEVM/tools/dump_layout.c" -o /tmp/ns-posix-dl
/tmp/ns-posix-dl > /tmp/ns-posix-layout.json
python3 "$NSOEVM/tools/ns_compile_oevm.py" "$SRC" /tmp/ns-posix-dbg.oimg \
    --format=oimg --layout=/tmp/ns-posix-layout.json

cat > /tmp/ns-posix-dbgharness.c <<'EOF'
#include "seam.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct debug_vm host_debug_vm(void);
static int pump(struct debug_vm *d, char *acc, size_t cap) {
    size_t off = 0; for (int g = 0; g < 50 && off < cap-256; g++) {
        int n = d->drain(d->ctx, acc+off, cap-1-off, 200);
        if (n < 0) { acc[off]=0; return -1; }
        if (n == 0 && off > 0) break;
        if (n > 0) off += (size_t)n;
    } acc[off]=0; return (int)off;
}
int main(void){
    FILE *f=fopen("/tmp/ns-posix-dbg.oimg","rb");
    static unsigned char img[256*1024]; size_t n=fread(img,1,sizeof img,f); fclose(f);
    struct debug_vm d = host_debug_vm();
    if (d.start(d.ctx,"dbgtgt",img,n)!=0){ printf("start fail\n"); return 2; }
    char acc[4096];
    pump(&d, acc, sizeof acc);                 // expect dbg:ready
    if (!strstr(acc,"dbg:ready")){ printf("no dbg:ready: %s\n", acc); return 1; }
    d.write(d.ctx,"dbg:methods");
    pump(&d, acc, sizeof acc);                 // expect a methods listing + dbg:ok methods
    if (!strstr(acc,"dbg:ok methods")){ printf("no methods: %s\n", acc); return 1; }
    d.write(d.ctx,"dbg:continue");
    pump(&d, acc, sizeof acc);                 // app runs to completion (no bp set)
    d.stop(d.ctx);
    printf("PASS: dual-pipe debug round-trip\n");
    return 0;
}
EOF
cc -std=c11 -Ikeeper /tmp/ns-posix-dbgharness.c posix/oimg_debug_vm.c -o /tmp/ns-posix-dbgharness
/tmp/ns-posix-dbgharness
```
> **Implementer note:** confirm the exact `dbg:methods` sentinel string the facade prints (`supervisor.ns`'s `listMethods` ends with `dbg:ok methods`). If `46_debug_target.ns` is not at the path above, locate it under `ns-tests` and update `SRC`. To additionally assert a breakpoint pause, discover the `countTo:` id from the methods listing, then `dbg:break <id> 0` + `dbg:continue` and assert `dbg:paused` — keep the first green version minimal, then extend.

- [ ] **Step 6: Run it; expect PASS** — `chmod +x tests/test_debug_vm.sh && bash tests/test_debug_vm.sh` → `PASS: dual-pipe debug round-trip`.

- [ ] **Step 7: Commit**
```bash
cd /home/david/workspace/ns-posix
git add posix/oimg_debug_vm.c posix/keeperd.c Makefile tests/test_debug_vm.sh
git commit -m "feat(posix): dual-pipe debug VM seam + keeperd wiring; real-oevm gate

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

# PHASE 3 — end-to-end + protocol docs.

## Task 8: Remote-debug-over-porta integration gate + PROTOCOL.md

**Files:**
- Create: `ns-posix/tests/porta_debug_integration.sh`
- Modify: `porta/docs/PROTOCOL.md`

**Interfaces:**
- Consumes: everything from Tasks 1–7 (porta `debug` verb + `debug?id=` channel + `porta debug` CLI; keeper session + dual-pipe VM).

- [ ] **Step 1: Write the end-to-end gate** `ns-posix/tests/porta_debug_integration.sh` (extend the structure of `porta_integration.sh`):
```bash
#!/usr/bin/env bash
# R3 acceptance gate. End-to-end remote debug THROUGH porta:
# deploy 46_debug_target -> debug attach -> dbg:methods -> break -> continue
# -> assert dbg:paused returns over debug?id= -> inspect -> detach.
# Skips (exit 0) unless NS_POSIX_IT=1.
set -euo pipefail
[ "${NS_POSIX_IT:-}" = "1" ] || { echo "SKIP: set NS_POSIX_IT=1 to run"; exit 0; }

cd "$(dirname "$0")/.."
ID="686f73740000d3b6"
NAME="dbgtgt"
PORTA_DIR=/home/david/workspace/porta
SERVER="http://localhost:6970"
SRC=/home/david/workspace/ns-tests/04_somns_basic/46_debug_target.ns
OEVM=/home/david/workspace/oevm
export OEVM_IMG="$OEVM/oevm_img"

# 1) build oevm_img + keeperd
if [ ! -x "$OEVM/oevm_img" ]; then
  ( cd "$OEVM" && TARGET=posix gcc -Wall -std=c99 -Isrc \
      src/vm.c src/gc.c src/dispatch.c src/platform.c src/serial.c src/mirror.c \
      "src/backend/$TARGET/platform_$TARGET.c" "src/backend/$TARGET/serial_$TARGET.c" \
      src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o oevm_img -lm )
fi
make keeperd >/dev/null

# 2) fresh porta serve
( cd "$PORTA_DIR" && go build -o /tmp/ns-posix-porta ./cmd/porta )
PCLI=/tmp/ns-posix-porta
rm -f /tmp/ns-posix-dbg-porta.db; rm -rf /tmp/ns-posix-keeperd
$PCLI serve --db /tmp/ns-posix-dbg-porta.db >/tmp/ns-posix-dbg-porta.log 2>&1 &
PP=$!; trap 'kill $PP 2>/dev/null || true' EXIT
for i in $(seq 1 50); do curl -fsS "$SERVER/api/nodes" >/dev/null 2>&1 && break; sleep 0.1; done

run_keeper(){ NS_POSIX_ID="$ID" PORTA_HOST="127.0.0.1" build/keeperd --once 2>>/tmp/ns-posix-dbg-keeperd.log; }

# 3) register + deploy the debug target (installs the app image on the node)
run_keeper
python3 tool/deploy.py "$SRC" --to "$ID" --name "$NAME" --server "$SERVER"
run_keeper   # node pulls run, installs + runs once (image now cached on node)

# 4) attach the debugger and drive a session, one poll per turn
$PCLI debug attach "$NAME" --device "$ID" --server "$SERVER"
run_keeper   # keeper sees debug attach -> launches VM under OEVM_DEBUG=1

# discover methods
$PCLI debug send "dbg:methods" --device "$ID" --server "$SERVER"
run_keeper   # keeper feeds dbg:methods, uploads the listing
$PCLI debug poll --device "$ID" --server "$SERVER" | tee /tmp/ns-posix-dbg-out.txt
grep -q "dbg:ok methods" /tmp/ns-posix-dbg-out.txt \
  || { echo "FAIL: no methods listing over porta"; cat /tmp/ns-posix-dbg-out.txt; exit 1; }
MID=$(grep "countTo:" /tmp/ns-posix-dbg-out.txt | head -1 | awk '{print $1}')

# set a breakpoint, continue, expect a pause to return over debug?id=
$PCLI debug send "dbg:break $MID 0" --device "$ID" --server "$SERVER"
$PCLI debug send "dbg:continue" --device "$ID" --server "$SERVER"
run_keeper   # VM runs autonomously to the breakpoint, prints dbg:paused, blocks
$PCLI debug poll --device "$ID" --server "$SERVER" | tee -a /tmp/ns-posix-dbg-out.txt
grep -q "dbg:paused" /tmp/ns-posix-dbg-out.txt \
  || { echo "FAIL: breakpoint pause did not return over porta"; cat /tmp/ns-posix-dbg-out.txt; exit 1; }

# inspect the paused frame
$PCLI debug send "dbg:inspect" --device "$ID" --server "$SERVER"
run_keeper
$PCLI debug poll --device "$ID" --server "$SERVER" | tee -a /tmp/ns-posix-dbg-out.txt
grep -q "dbg:stack" /tmp/ns-posix-dbg-out.txt \
  || { echo "FAIL: inspect did not return a stack frame"; cat /tmp/ns-posix-dbg-out.txt; exit 1; }

# detach
$PCLI debug detach "$NAME" --device "$ID" --server "$SERVER"
run_keeper
echo "PASS: remote attach -> methods -> break -> paused -> inspect -> detach over porta"
```
> **Implementer note:** `--server` may be a persistent flag on the root command (it is, per `root.go`). If a subcommand rejects it, move `--server` before the subcommand. The exact `dbg:paused`/`dbg:stack` strings come from `supervisor.ns`; confirm against `ns-oevm/tools/test_debugger_integration.py`'s assertions. Timing: each `run_keeper` is one poll; `dbg:continue` runs to the breakpoint within that single poll (C-assisted, autonomous), so one poll after continue should surface `dbg:paused`.

- [ ] **Step 2: Run the gate** — `chmod +x tests/porta_debug_integration.sh && NS_POSIX_IT=1 bash tests/porta_debug_integration.sh` → final `PASS:` line.

- [ ] **Step 3: Document the protocol** — in `porta/docs/PROTOCOL.md`: (a) add `debug` to the §2 verb table and a `### 2.x debug` subsection (declarative `{name, action: attach|detach}`, last-write-wins, superseding run-once while attached); (b) add a `## 8. Debug channel (node ↔ gateway)` section describing RRQ `debug?id=` (drain dbg: request lines, like commands) + WRQ `debug?id=` (append dbg: response lines, newline-delimited), noting porta is a stateless relay and the session lives in the keeper; (c) add a Conformance bullet in §7: "(If it supports remote debug) drain `debug?id=` requests, feed them to the in-image debugger, and ship response lines back via `debug?id=` WRQ." Match the existing prose style and section numbering.

- [ ] **Step 4: Verify both repos' suites are green**
```bash
cd /home/david/workspace/porta && go test ./...
cd /home/david/workspace/ns-posix && make test
```

- [ ] **Step 5: Commit (two repos)**
```bash
cd /home/david/workspace/ns-posix
git add tests/porta_debug_integration.sh
git commit -m "test(ns-posix): R3 remote-debug-over-porta integration gate

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
cd /home/david/workspace/porta
git add docs/PROTOCOL.md
git commit -m "docs(protocol): document the debug verb + debug?id= channel (R3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Acceptance (whole plan)

- `porta`: `go test ./...` green, including the new command/store/handler/cli debug tests.
- `ns-posix`: `make test` green (incl. `test_cmd`, `test_debug`); `make keeperd` clean.
- `bash tests/test_debug_vm.sh` drives a real oevm debug image over dual pipes (`dbg:ready` + `dbg:ok methods`).
- `NS_POSIX_IT=1 bash tests/porta_debug_integration.sh` completes a remote attach → break → continue → **`dbg:paused`** → inspect → **`dbg:stack`** → detach **through porta**.
- `porta/docs/PROTOCOL.md` documents the `debug` verb and `debug?id=` channel.
- Debug transport is always compiled into `keeperd`; with no session, `debug_active==0` and the shuttle is a no-op.

## Notes carried from the spec (do not re-litigate)

- **Independent of R2** — dedicated `debug?id=` channel; no `data?id=` coupling.
- **Dual pipes, no PTY** — the VM flushes on newline.
- **`debug_vm` is a separate seam, always wired** — the device backend implements the same four functions at R6.
- **Future (out of scope):** MCP bridge; a DAP adapter (client-side DAP ↔ `dbg:*`); tightened poll cadence during a session; concurrent sessions.
