# `custos profile` Implementation Plan (#312)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `custos profile` triggers a method-profile window on a node over the cable, reads the dump, and ranks it by name through the image map; a custos page shows the same table; `mprof_resolve.py` and `view.py` are deleted.

**Architecture:** Go side first, host-testable without a board: one strict image-map reader shared by `debug` and `profile` (`custos/internal/imagemap`), a dump parser/ranker (`custos/internal/profile`), the CLI verb. Node side second: one command reader in every image (Q1), which turns a console line into a write on the node's own `dbg/req`, so the cable and porta feed one handler that the kernel owns.

**Tech Stack:** Go (custos module), nsl (`kernel.nsl`), C (`vm.c`, `platform_zephyr.c`, `platform_posix.c`).

**Spec:** `docs/design/2026-10-05-custos-profile-over-usb.md` § Answers; `docs/design/2026-10-04-custos-peer-design.md` §6a step 4 and D10.

## Global Constraints

- custos imports only `protocol/` from rete, never porta (D1, #338).
- The image map is `protocol/IMAGE-ARTIFACTS.md` §3; a missing or extra field refuses the file.
- An offset absent from the map is shown as UNKNOWN with the offset, never dropped.
- The percentage denominator is the slotted activations plus `overflow` (a lower bound).
- No backward compatibility: a dump line format may change; both ends are rebuilt together.
- Gates: `for m in protocol porta custos; do (cd $m && go test ./...); done`; nsl suite `python3 tools/run_tests_oevm.py` (read the aux line).

## Review Focus

- A map from a different build: every row UNKNOWN; the report must say so, not read as a quiet program.
- A log holding two windows: the last header wins and the row count/sum warnings fire.
- A saturated table (`overflow>0`): INCOMPLETE, percentages are lower bounds.
- No `mprof:` lines at all (image built without `OEVM_METHOD_PROFILE`): a non-zero exit naming the flag.
- `--top N` never shrinks the denominator or the distinct-method count.

---

### Task 1: one strict image-map reader

**Files:**
- Create: `custos/internal/imagemap/imagemap.go`, `custos/internal/imagemap/imagemap_test.go`
- Modify: `custos/internal/debug/sourcemap.go` (use `imagemap.Load`; delete its private `imageMap`/`mapMethod` and `CheckImage`'s hashing)

**Interfaces — Produces:**
```go
package imagemap
type Method struct { Name string; File int; Lines [][2]int }
type Map struct {
    SHA256 string; Size int64; OopSize int
    Entry string; Files []string
    Methods map[int]Method // key: blob byte offset, the key `mprof:` prints
}
func Load(path string) (*Map, error)          // strict: §3 field for field
func (m *Map) CheckImage(oimgPath string) error // sha256 + size
```

- [ ] Step 1: move the decode + required-field check from `debug.LoadSourceMap` into `imagemap.Load` unchanged (same error texts, `rete#337`), converting keys with `strconv.Atoi` (a non-integer key is an error naming it).
- [ ] Step 2: test `Load` on `protocol/fixtures/image/Fixture.oimg.map.json` (entry, files[0], a known method name), a missing field, an extra field, a non-integer key; `CheckImage` against `Fixture.oimg` and a one-byte-changed copy.
- [ ] Step 3: `LoadSourceMap` calls `imagemap.Load`; `SourceMap.CheckImage` delegates. Run `cd custos && go test ./...` — the 13 `sourcemap_test.go` tests stay green unchanged.
- [ ] Step 4: commit `custos: imagemap — one strict reader of the image map, refs #312`.

### Task 2: parse and rank a dump

**Files:**
- Create: `custos/internal/profile/profile.go`, `custos/internal/profile/profile_test.go`

**Interfaces — Consumes:** `imagemap.Map`. **Produces:**
```go
type Header struct { Window string; Methods, Activations, Overflow, Cap int }
type AllocHeader struct { Window string; NoFrameArenaB, NoFrameArenaN, NoFrameCheapB, NoFrameCheapN, AllocOverflow int }
type Row struct { Key string; Count int }                  // Key: decimal offset or "@0x…"
type AllocRow struct { Key string; ArenaB, ArenaN, CheapB, CheapN int }
type Dump struct { Header *Header; Rows []Row; Alloc *AllocHeader; AllocRows []AllocRow }
func Parse(text string) Dump                               // last header wins
type Ranked struct { Name string; Count int; Pct float64; Unknown bool }
type RankedAlloc struct { Name string; ArenaB, ArenaN, CheapB, CheapN int; Pct float64; Unknown bool }
func (d Dump) Rank(m *imagemap.Map) []Ranked               // count desc; denominator rows+overflow
func (d Dump) RankAlloc(m *imagemap.Map) []RankedAlloc     // bytes desc; denominator + no-frame bytes
func (d Dump) Warnings() []string                          // INCOMPLETE, row-count, row-sum, no-frame, allocOverflow
func Write(w io.Writer, d Dump, m *imagemap.Map, top int, alloc bool) // the text report
```
Regexes, verbatim from `mprof_resolve.py` (multi-line mode on the row forms):
`mprof:\s+(@?\S+)\s+(\d+)\s*$`, `mprof\[(\w+)\]:\s+methods=(\d+)\s+activations=(\d+)\s+overflow=(\d+)\s+cap=(\d+)`,
`mprofa:\s+(@?\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$`, `mprofa\[(\w+)\]:\s+noframe_arena=(\d+)/(\d+)\s+noframe_cheap=(\d+)/(\d+)\s+allocOverflow=(\d+)`.

- [ ] Step 1: port every test in `nodus/oevm/tools/test_mprof_resolve.py` (14) to `profile_test.go`, one Go test per Python test, same names in CamelCase, same fixtures (MAP offsets 16/32; the overflow=100 dump), plus a test that a dump with no `mprof:` rows yields `Rows == nil`.
- [ ] Step 2: run `go test ./internal/profile/` — FAIL (no package).
- [ ] Step 3: implement; text output keeps the Python report's lines (`window=… methods=… activations=… cap=…`, `INCOMPLETE: …`, `WARNING: …`, `%12d  %5.1f%%  name`, `showing top N of M rows`, the unresolved-row note, the `--- allocation attribution ---` block and `distinct allocating methods: N`).
- [ ] Step 4: `go test ./...` in custos — PASS. Commit `custos: profile — parse and rank an mprof dump through the image map, refs #312`.

### Task 3: `custos profile` (offline)

**Files:** Modify `custos/cmd/custos/main.go`; Create `custos/internal/profile/cli.go`, `cli_test.go`.

```go
func Run(args []string, stdin io.Reader, stdout, stderr io.Writer) int
// custos profile -map <name>.oimg.map.json [-top N] [-alloc] [log ...]   (stdin when no log)
```
- [ ] Step 1: tests: a log file with the fixture dump → exit 0 and the ranked table; no `mprof:` rows → exit 1 and stderr names `OEVM_METHOD_PROFILE`; a missing `-map` → exit 2 with usage.
- [ ] Step 2: implement; wire `os.Args[1] == "profile"` in `main.go` beside `debug`.
- [ ] Step 3: cross-check against the Python tool once on a real dump (`porta_sync_host.sh` LEG 8's `/tmp/porta_node.log` or a C6 console log): identical rows and percentages. Record the command and both outputs in the commit body.
- [ ] Step 4: commit `custos profile: the offline verb (a log in, a ranked table out), refs #312`.

---

**Tasks 4–6 wait for David to confirm the node-side shape below (proposed 2026-10-05, not decided).**

**Proposed node-side shape (Q1, mechanism-minimal).** Today `profile <ms>` is handled by a copy of `dbgReq:`/`profileWindowStart:`/`profileWindowEnd` in each of four app files (`PortaBridgeNodeC6.nsl:236-266`, `PortaBridgeNode.nsl`, `PortaBridgeNodeNrf.nsl`, `ReplayProbeC6.nsl`), and nothing reads the cable in a standard image without blocking the VM (`readLine`, prim 69). Proposal: (a) the kernel owns the one handler: it subscribes `dbg/req` and runs the profile window itself, and the four app copies are deleted; (b) in every image the console's line ISR (`rl_isr`, armed at boot rather than on first `readLine`) posts a completed line as a pulse, and the kernel writes it to the node's own `dbg/req`, so a cable line and a porta write reach the same handler; (c) in a debug image the Supervisor's pause rule is unchanged: a line while running stops the program and the `dbg:*` loop reads it; a `profile <ms>` line there is answered by the same handler. (d) `mprof_report` ends the dump with `mprof[<why>]: end` so a live reader knows the window is complete. USB-CDC boards (polled, no ISR) are out of scope until #310.

### Task 4: the kernel owns the profile handler; the cable feeds `dbg/req`
Files: `nodus/nsl/lib/kernel.nsl`, `nodus/oevm/src/backend/zephyr/platform_zephyr.c`, `nodus/oevm/src/backend/posix/platform_posix.c`, `nodus/oevm/src/vm.c` (end line), the four app files, `nodus/tests/23_mprof/*`. Tests: extend `23_mprof/04_dbg_req_chain.nsl` to drive the kernel handler; `porta_sync_host.sh` LEG 8 unchanged and green; a new host test feeding `profile 50` on stdin to a std (non-`OEVM_DEBUG`) VM built with `-DOEVM_METHOD_PROFILE` and asserting `mprof[window]: end`. Board: c6spare, `PortaBridgeNodeC6` std image, `printf 'profile 3000\n' > port`, dump seen in the collector log, bridge still ticking.

### Task 5: `custos profile -port <tty> -ms <N>`
Sends `profile <N>`, collects lines until `mprof[window]: end` (timeout N + 10 s), ranks as Task 3. Test with a fake `Line` (as `session_test.go`'s `fakeVM`). Board: c6spare.

### Task 6: the profile page; delete the Python
Create `custos/internal/custos/profile.go` (`mountProfile(mux, …)`: `GET /profile` the last ranked table, `POST /profile/capture` runs Task 5 against a configured port), embedded html. Delete `nodus/oevm/tools/mprof_resolve.py`, `test_mprof_resolve.py`, `zephyr/tools/console/view.py`, `test_view.py`, `console/htmx.min.js`; keep the `mprof-tools-py` gate (it still collects `test_oevm_runner.py`). Rewrite the prose that cites them: `docs/development.md:36`, `protocol/IMAGE-ARTIFACTS.md:139,163`, `nodus/nsl/docs/compiler.md:196`, `zephyr/tools/console/README.md:83,158,198,238`, comments `vm.c:554`, `observer.c:223`; list `review/tests/CLAIMS.md:86` for David.

### Close-out
Promotion: `docs/development.md` § Profile and `protocol/IMAGE-ARTIFACTS.md` absorb the verb; the ladder entry in `review/program-sequence.md` is David's. Q2 (one flag or two) is decided from the numbers in the spec's Answers.
