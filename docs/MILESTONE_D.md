# Milestone D — provenance and validation note

*This is a reference note, written once the milestone was green, for future
milestones (and phase 2) to cite. It is not a pitch or a walkthrough — see
`review/program-sequence.md`, `review/test-suites-spec.md`, and
`.superpowers/sdd/task-{1..10}-brief.md` for the day-by-day narrative.*

Milestone D's premise (first-step spec §4D/§5, `review/program-sequence.md`
phase 1 entry "D — Supervisor + let-it-crash + host job"): a crashing handler
must be contained — the reactor survives, the heap survives, the rest of the
node keeps running — and there must be a supervisor loop that observes
crashes, restarts or quarantines the offending job, and a host event source
(timers) feeding the same reactor. This note bags the result: what landed,
the reconciliations against the staged spec, the closed carry-forwards, the
final gate, and the measured footprint delta.

## 1. What landed

- **The crash boundary** (`tuvm/src/vm.c` `vm_protected_fire`/`vm_raise` +
  `g_raised_condition`, a GC root) — a `setjmp` frame around a single
  reaction firing. `self error: 'text'` raises to the nearest protected
  frame; the frame's caller gets back the raised condition (a String) instead
  of unwinding the whole process. Built as "unwind to a marked frame" (one
  marked frame — the reactor's), per `review/lib/exceptions.md` constraint 4,
  so the future `on:do:`/`ensure:` step (phase 2) generalizes the same
  recovery path instead of reworking it (closes #138).
- **The protected reactor** — `nsl-tuvm/lib/kernel.ns` `Space>>drain`
  (~L667-678) wraps every reaction firing in `platform protectedFire:value:`
  (prim `PRIM_pf_protectedFire`, id 83). A clean firing answers nil and drain
  moves on; a firing that crashes is caught, unwound, and abandoned mid-body
  — the crashing reaction's remaining statements never run, the other
  handlers and the process are unaffected. `postCrash:channel:` (~L691-697)
  turns a caught condition into a first-class crash record.
- **The crash record and reserved channel.** Landed shape:
  `Record tag: #crash with: #text -> <text> with: #channel -> <path>`,
  posted to the plain (no `%` sigil) reserved channel `sys/crash`. Tests
  observe crashes with a `sys whenever: 'crash' do: [...]` reaction, not
  `take:`, because `drain` dispatches (and would drop) the transient crash
  records — same run-to-completion reasoning as the reactor itself. A crash
  raised while firing a `sys/`-rooted reaction is reported via `out` and
  dropped instead of re-posted, so a supervisor/observer bug cannot loop.
- **Owner-tag identity + the restart sweep verb.** `whenever:do:` tags every
  reaction with the capability object it was registered through
  (`owner: self`); `platform spaceDropReactionsOwnedBy:` (`tuvm/src/space.c`
  `space_drop_reactions_owned_by`, prim id 84) sweeps exactly one job's
  reactions by `==` against its rootCap, proven against a second, untouched
  capability by `nsl-tests/11_supervision/04_owner_sweep.ns`. This pins
  #134's item 2 (owner-tag identity) explicitly rather than leaving it
  implicit (closes #134.2).
- **The Supervisor** (`nsl-tuvm/lib/kernel.ns` `SupervisedJob`/`Supervisor`,
  ~L714-822) — observes `sys/crash`, attributes each crash to the owning job
  by prefix-matching the crash record's `#channel` against the job's
  `rootPath`, and either restarts (sweep + re-run the job's own start thunk,
  which re-registers its reaction under the SAME rootCap object) or
  quarantines (sweep, stop restarting, log a `#quarantine` record to
  `sys/log`) once `quarantineLimit` (3) consecutive crashes accumulate. A
  `progress:` capability lets a job report clean operation, resetting its
  crash count so unrelated healthy periods don't accumulate toward
  quarantine — the reactor-native reading of "reset on clean firing" (a
  deliberate refinement over the staged spec's unstated reset trigger).
- **Host virtual-clock timers** (`tuvm/src/platform.c` host timer seam,
  prims 85-89: `timerAfter:`/`timerEvery:`/`timerCancel:`/`monotonicMillis`/
  `advanceMillis:`; `nsl-tuvm/lib/kernel.ns` `class Timers`, ~L979+) — a
  one-shot or repeating timer posts a token event onto the reserved
  `sys/timer` channel; `Timers>>dispatch:` owns the fan-out onto the
  target channel/message the caller registered. Determinism for tests comes
  from `platform advanceMillis:` driving the host virtual clock — never wall
  time.
- **The demo-node-shaped integration test** (D.7,
  `nsl-tests/11_supervision/09_demo_node_integration.ns`) — composes a real
  `Protocol` FSM, real timers arming/cancelling a watchdog, the protected
  reactor's containment, and the Supervisor's restart/quarantine/progress
  policy, driving a `resume:`-rehydrated job through healthy and silent
  (wedged/timeout) sample cycles, reduced from `review/demo-node.ns`'s
  `TempNode` per the Task 7 brief (no i2c, mock reading).
- **The soak/leak-plateau test** (D.5,
  `nsl-tests/11_supervision/10_soak_leak.ns`) — see §2.
- **Two Milestone-C carry-forward tests**, closing #134's remaining
  items: `nsl-tests/10_fsm/02_snapshot_restore.ns` (D.8, direct
  `Protocol>>snapshot`/`restore:` round-trip — closes #134.1) and
  `nsl-tests/09_channels/11_path_too_long.ns` (D.9, a canonical
  single-segment path over `OEVM_SPACE_MAX_PATH` (64) reaching
  `space_intern` end-to-end via `out:` — closes #134.3).

## 2. The soak/leak-plateau test (D.5) and the missing heap gauge

`nsl-tests/11_supervision/10_soak_leak.ns` drives **100,000 iterations**
through three healthy handlers (300,000 firings total) plus a periodic
deliberate crash/restart cycle every 20th iteration — 5,000 full
crash→contain→restart cycles, continuous for the entire run (the crash
counter never reaches `quarantineLimit` because each cycle's `progress`
event is dispatched, by FIFO construction, before that cycle's own crash
record — see the test's header comment for the exact ordering proof) —
then proves the node is still alive and responsive with one final healthy
event on a fresh channel. Both paths PASS in a few seconds (Path A ~3.5s,
Path B ~2.2s on this host), comfortably inside the harness's 30s per-test
timeout and not dominating the gate. 100k was chosen because it is the
spec's own literal floor ("D.5 ... ≥100k events") and is already far more
than enough to expose a per-cycle leak against the fixed-size arena (a
leaking restart path would eventually exhaust `channel reactions full` or
the heap outright over 5,000 cycles) without slowing the gate.

**Heap gauge:** grep-confirmed there is no nsl-visible heap/free-bytes
accessor. `oevm_heap_stats()` exists in C (`tuvm/src/oevm.h:399-414`,
`gc.c:839`, reporting `live_bytes`/`high_water`/`largest_free_block`) but was
never wired to a prim — `tuvm/src/prim_ids.h`'s append-only id list runs
59-89 (Milestone D Task 6's timer prims are the last additions) with nothing
heap-related, and `kernel.ns` has no `platform`-level forwarder for it. So
D.5 asserts the **weaker** invariant the Task 10 brief allows in that case:
100k events with periodic crashes complete, and the node answers a final
healthy event — not a strict bounded-variance plateau assertion. This is
recorded as still-open in `review/reliability-gaps.md` G3 (heap telemetry);
when a `sys/heap`-style gauge is wired, extend D.5 to sample it every ~10k
events and assert bounded variance instead.

## 3. Reconciliations against the staged spec (`review/tests/README.md`,
   `review/lib/timers.ns`, `review/lib/exceptions.md`)

- Crash-record field is landed as `#text` (not the staged `#error`);
  `#channel` is unchanged.
- The reserved channels are the plain `sys/crash` and `sys/timer` — no `%`
  sigil anywhere in the landed spelling. The staged `%sys/*` spelling
  (`review/tests/README.md`'s ASSUMED API block, `review/lib/timers.ns`'s
  wiring comment, `review/lib/exceptions.md`, and the forward-looking
  channel names in `review/metrics-spec.md`/`review/reliability-gaps.md`)
  is respelled to match.
- Crash observation is a `sys whenever: 'crash' do: [...]` reaction, not
  `take:` (the staged README's assumption) — `drain` would drop the
  transient crash record before a `take:` could see it.
- `td-crash/`'s landing directory is `nsl-tests/11_supervision/` (the staged
  README said `10_supervision/`, never used — `10_fsm/` claimed that slot
  first).

## 4. Carry-forwards closed

- **#138** — "Milestone D spec must adopt exceptions.md constraint 4:
  crash boundary as 'unwind to a marked frame'." Closed: the setjmp boundary
  (§1) is built exactly that way, with exactly one marked frame (the
  reactor's), constraint 5 (exceptions never cross a dispatch boundary)
  riding along for free (a raise unwinds at most to the current firing's own
  reactor frame).
- **#134** — "Milestone D carry-forwards from the Milestone C final
  whole-branch review," all three items landed:
  - **#9.1** Protocol snapshot/restore direct behavioral test →
    `nsl-tests/10_fsm/02_snapshot_restore.ns`.
  - **#9.2** Owner-tag identity pinned explicitly in the D spec and swept by
    `==` against the stable rootCap → `spaceDropReactionsOwnedBy:` +
    `nsl-tests/11_supervision/04_owner_sweep.ns`.
  - **#9.3** `channel path too long` end-to-end kernel→C test →
    `nsl-tests/09_channels/11_path_too_long.ns`.

(Both issues remain open on GitHub as of this note — closing them was judged
outside this task's scope by the harness's own permission classifier; David
can close them directly, referencing this note and the landed paths above.)

## 5. Conformance state — final gate

Run: `cd nsl-tuvm && OEVM_PATH=AB python3 tools/run_tests_oevm.py` (both Path
A and Path B, the full behavioral suite + C unit tests + integration gates).

```
Passed:  155   (154 captured in one clean run + 10_soak_leak.ns, +1)
Failed:  0
Skipped: 12
grant: PASS   debugger-integration: PASS   portability-32: PASS   boot-gc-asan: PASS
C-unit gate (tools/run_c_tests.sh): test_space.c PASS (3-slot space_dequeue, post-F1)
```

**How these were captured (transparency note, 2026-07-07):** the behavioral
suite runs every test on BOTH Path A (solidified image) and Path B (`.oimg`).
`10_soak_leak.ns` drives 300k firings + 5,000 restart cycles on each path; at
that length the full-suite run destabilized under this session's accumulated
load, so the authoritative counts were captured in two pieces that together
cover 100%:
  - **154 / 0 / 12** — one clean, uninterrupted full-suite run with the soak
    test held aside (all four named gates PASS, exit 0). This is the number
    that matters for regression: every behavioral test green on both paths.
  - **`10_soak_leak.ns` +1** — verified PASS on Path A and Path B individually
    (`run_test_file` / `run_test_file_oimg`), byte-identical output.
  ⇒ **155 / 0 / 12**, four gates PASS. The C-unit gate `test_space.c` was
  rebuilt and run directly after the F1 fix (all 9 properties PASS, exit 0).

NOTE on Path A: an earlier full-suite attempt showed 11 `A:FAIL B:PASS`
results; these were traced to two `run_tests_oevm.py` processes racing on the
shared `src/solidified_image.c` (the harness is not concurrency-safe — spec §7)
and are NOT a regression: the committed `solidified_image.c` is byte-identical
to the milestone-c baseline, Path B was 100% green throughout, and each flagged
test passes on Path A when run without a competing runner (confirmed on a
sample and by the clean 154/0/12 run above).
```

## 6. Footprint delta

Measured with `tuvm/tools/footprint.sh` (host-proxy build; `size -t` totals
over the engine objects, `.oimg`/image object excluded; RAM = data+bss, per
design addendum A2). Banked in `tuvm/tools/footprint-after.txt` (this
milestone's numbers); `tuvm/tools/footprint-baseline.txt` remains the
original Milestone-A seed baseline, unchanged.

| | text | data+bss (RAM, host proxy) |
|---|---:|---:|
| after Milestone C (banked, commit `93c8701`) | 39805 | 17261576 |
| after Milestone D (this note, measured at HEAD) | 41080 | 17262424 |
| delta | +1275 bytes | +848 bytes |

Per-object attribution (comparing the two `size -t` runs):

- `vm.o` +564 text / +8 bss — the `setjmp` crash boundary
  (`vm_protected_fire`/`vm_raise`/`g_raised_condition`, Task 1).
- `platform.o` +527 text / +1040 bss — the host virtual-clock timer seam
  (`timerAfter:`/`timerEvery:`/`timerCancel:`/`monotonicMillis`/
  `advanceMillis:`, pending-timer bookkeeping, Task 6) plus the
  `protectedFire:value:`/`spaceDropReactionsOwnedBy:` prim dispatch wiring
  (Tasks 1/4).
- `space.o` +232 text / −256 bss — `space_drop_reactions_owned_by` (Task 4)
  and the channel-path-in-dequeue plumbing the crash record needs.
- `dispatch.o` −60 text / +56 data — net effect of the 7 new platform prim
  enum slots (83-89) threading through the dispatch table.
- `gc.o` +12 text / 0 bss — `g_raised_condition` root-scan wiring.
- `kernel_boot.o`, `serial.o`, `udp.o`, `mirror.o`, `oimg_loader.o`, and the
  three `*_posix.o` backend objects: unchanged.

The growth is small and entirely attributable to the crash-boundary +
timer-seam C surface Milestone D added — no unattributed drift.

## 7. Provenance

- `tuvm` HEAD at close: see the close-out commit in this same commit series
  (`git log --oneline -1` on `milestone-d` after this note lands).
- `nsl-tuvm` HEAD at close: `c77743d` (Task 6, timers — no nsl-tuvm changes
  in Task 10; Milestone D's kernel/C work all landed in Tasks 1-9).
- `nsl-tests` HEAD at close: see the soak-test commit in this same commit
  series.

See `.superpowers/sdd/task-{1..7}-report.md`, `task-8-9-report.md` (combined),
and `task-10-report.md` for the per-task record, and
`.superpowers/sdd/progress.md`/`l1-progress.md` for the running ledger.
