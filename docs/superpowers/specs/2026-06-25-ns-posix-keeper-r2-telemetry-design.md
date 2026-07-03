# R2 — ns-posix keeper telemetry forwarding (design)

> **Status:** design (decided 2026-06-25). Rung **R2** of the Newspeak-node roadmap
> (`docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`).
> Predecessor: R1 keeper (`2026-06-25-ns-posix-keeper-r1-design.md`).
> Implementation repo: `github.com/davidg238/ns-posix` (the C keeper). Porta is the
> wire counterparty; its contract is `porta/docs/PROTOCOL.md`.

## Decision

Build **keeper-side telemetry forwarding**: surface the resident oevm VM's captured
stdout and run-failures to porta over the `data?id=` JSONL channel, honouring porta's
per-stream forwarding policy (`set-forward`, §2.4) **fully and conformantly**.

**Scope is telemetry only.** R2's other roadmap candidate — OTA-delta / partial-graph —
is **deferred to its own later rung**: it has no porta protocol support yet (treated as
future/non-goal in porta's design specs) and would expand into both repos. This spec does
not touch it.

## Why this is all keeper-side

Recon (2026-06-25) found porta's telemetry ingest **fully built**:

- WRQ handler `writeData` — `porta/internal/handler/handler.go:240-276`
- JSONL parser `telemetry.ParseLine` — `porta/internal/telemetry/parse.go:38-131`
- Store `InsertData` / `data_log` — `porta/internal/store/data.go:21-51`

The gap is entirely in `ns-posix`: the keeper captures the VM's stdout into a buffer
(`posix/oimg_vm.c:29-37`) but **discards it** (`do_run` at `keeper/keeper.c:43` ignores
both the return code and `out`) and never issues a `data?id=` WRQ. R1 also stubbed
`set-forward` (`keeper/keeper.c:64` — "policy stored, no forwarding yet"; the policy is
not even parsed into `struct command`).

## Wire contract (porta `PROTOCOL.md`, normative)

### Telemetry batch — `data?id=<id>` WRQ, JSONL (§6)

One JSON object per line. Fields used by R2:

| Key | Type | Meaning |
|-----|------|---------|
| `kind` | string | `"print"` or `"panic"` (R2 emits these two). Default `"log"` at gateway. |
| `text` | string | Print: raw line. Panic: **base64** of the raw trace blob. |

Other fields (`level`, `name`, `value`, `ts`, `seq`) default at the gateway and are not
emitted by R2. **FATAL-level logs and `panic` entries are must-deliver** — shipped even
when the stream gate is off.

### Forwarding policy — `set-forward` (§2.4), echoed as `node_config.forward` (§3.2)

Declarative/absolute: the command is the **whole** policy, not a patch; an omitted stream
resolves to its default (**off**).

```json
{"verb":"set-forward","print":{"on":false},"log":{"on":true,"level":"warn"},"telemetry":{"on":true}}
```

- Three streams: `print {on, every_s?}`, `log {on, level?, every_s?}`, `telemetry {on, every_s?}`.
- `log.level` ∈ `trace|debug|info|warn|error|fatal`; absent ⇒ keep `warn`.
- `every_s` is **reserved** (porta carries it, no CLI flag yet) — R2 stores it for the
  echo but does not act on per-stream intervals (the keeper coalesces telemetry with its
  report/poll window).
- The node **persists** the resolved policy and **echoes** it as `node_config.forward` —
  the read-back/convergence ack. Echo shape (always-on node):
  ```json
  "forward":{"print":{"on":b},"log":{"on":b,"level":"warn"},"telemetry":{"on":b}}
  ```
  Each stream echoes `{"on":bool}`; `log` **always** carries `level` (regardless of `on`);
  `every_s` appears per stream **only when configured**. Defaults: all `on:false`,
  `log.level:"warn"`.
- Echoed **only** on cold boot and after a config change (steady-state reports omit
  `node_config`) — this reuses the keeper's existing `dirty_node_config` gate.

## What R2 emits, and the gate decisions

Per poll, `do_run` appends to a per-wake telemetry buffer:

| VM outcome | Entry | Gate |
|------------|-------|------|
| `run` returns 0, `out` non-empty | `{"kind":"print","text":"<out>"}` | shipped iff `print.on` |
| `run` returns < 0 (nonzero exit / crash) | `{"kind":"panic","text":"<base64(out)>"}` | **must-deliver** (always) |

R2 produces **no `log` or `metric` entries** — oevm has no structured telemetry channel
yet. The `log`/`telemetry` streams therefore exist only in the policy and its echo; their
gates have no emission to gate this rung. (Wiring oevm-side structured logs/metrics is a
later rung; the gate plumbing is ready for it.)

**Two ratified judgment calls (David, 2026-06-25):**

1. **Panic `text` is base64-encoded; print `text` is raw** (JSON-escaped). This matches
   §6 (`print` example is raw text; `panic` text is the base64 trace blob) and keeps the
   wire uniform across node types. oevm symbolication, if ever added, is node-defined dev
   tooling (cf. `nodus panic`) — not part of this rung.
2. **Policy persisted via the `store` kv seam** (`"forward"` key), not a host-only file.
   Where the posix `store` backs kv durably it survives a keeperd restart; where kv is
   RAM-only it degrades to process lifetime (acceptable on desktop). Real flash
   persistence arrives for free with the device backend at R6.

## Flush ordering

`keeper_poll_once` (`keeper/keeper.c:86`): drain `commands` → `upload_report` →
**flush telemetry**. After the report, if the gated buffer is non-empty, issue one WRQ to
`data?id=<id>` with the JSONL batch. (Report before data mirrors the nodus node ordering.)

## Module boundaries

| Module | Change |
|--------|--------|
| `keeper/cmd.{c,h}` | Extend `struct command` with a `forward_policy` payload; extend `cmd_parse` to decode `set-forward`. **Bounded per-object parsing** is required: all three sub-objects contain `"on"`, so the existing unbounded `find_val`+`strstr` (the triggers precedent at `cmd.c:65-72`) would alias. Add a small "value of key within object bounds" helper. This is the one real parser change. |
| `keeper/forward.{c,h}` *(new)* | `struct forward_policy` (the three streams + level + every_s flags) and its **serialize/deserialize** for kv persistence and report echo. Keeps policy concerns out of `keeper.c`. |
| `keeper/keeper.{c,h}` | `struct keeper` gains `struct forward_policy fwd` + a telemetry buffer. `keeper_init` loads the policy from kv (cold boot). `dispatch(CMD_SET_FORWARD)` applies (absolute replace) + persists via kv + sets `dirty_node_config`. `do_run` appends print/panic entries. `keeper_poll_once` flushes the gated buffer. |
| `keeper/telemetry.{c,h}` *(new)* | Entry struct; a fixed-size ring buffer (≈`KEEPER_MAX_APPS` + small headroom, drop-oldest on overflow); `telemetry_build` → JSONL with **JSON-string escaping** (VM stdout is arbitrary) and a **base64 helper** for panic text. |
| `keeper/report.{c,h}` | Extend `struct report` + `report_build` to emit `node_config.forward` (within the existing `include_node_config` block). |

The 3-interface seam (`transport`/`store`/`vm`) is **unchanged** — no new platform
surface. All new logic is VM-neutral keeper-core C, inheritable by a future device port.

## Testing (TDD, VM-neutral with `fake_seam`)

| Test | Asserts |
|------|---------|
| `tests/test_telemetry.c` *(new)* | JSONL build; JSON-string escaping (quotes/newlines in stdout); panic `text` is base64; buffer gating (print dropped when `print.on=false`). |
| `tests/test_cmd.c` | `set-forward` bounded parse: the three sub-objects' `on`/`level`/`every_s` resolve to the correct stream (no aliasing); omitted stream ⇒ off; absolute replace. |
| `tests/test_report.c` | `node_config.forward` echo shape: each stream `{on}`, `log` always carries `level`, `every_s` only when configured, defaults all-off. |
| `tests/test_keeper.c` | Full poll via fakes: success+stdout ⇒ `data?id=` WRQ with a `print` entry **iff** `print.on`; run-failure ⇒ `panic` entry shipped even with all gates off; gates-off success ⇒ **no** `data?id=` WRQ; policy round-trips through kv across a re-init. |
| `tests/porta_integration.sh` | Extend the R1 gate: queue `set-forward print on` → deploy `01_method_call.ns` → run on oevm → assert porta's `data_log` captured the `print "42"` (query via porta CLI). |

## Acceptance

- All keeper unit tests pass (existing + new), 32- and 64-bit where the R1 gate runs both.
- `NS_POSIX_IT=1 bash tests/porta_integration.sh` passes: with `print` forwarding on, the
  deployed app's stdout (`42`) lands in porta's `data_log`; a deliberately-failing image
  surfaces a `panic` entry even with all gates off.
- `set-forward` is no longer stubbed: `node_config.forward` echoes the in-effect policy on
  cold boot and after each change.

## Out of scope (explicit)

- OTA-delta / partial-graph image transfer (its own later rung).
- oevm-side structured `log`/`metric` emission (the gates are plumbed but unfed this rung).
- Per-stream `every_s` interval scheduling (reserved; coalesced with the poll window).
- Symbolication/decoding of panic blobs (node-defined dev tooling, future).
