# T0.1 — sanitizer gate (the highest-value single item in the whole test plan)

> ## ⚠ STALE PATHS — READ BEFORE FOLLOWING ANY FILE REFERENCE (2026-08-29, rete#277)
>
> **The claims and the ordering in this file are current. The PATHS are not.** Every
> `nodus/tuvm/...` reference below predates the 2026-08-02 consolidation and the #79
> rename; the tree is **`nodus/oevm/`**. Checked path by path on 2026-08-29: of the 62
> distinct `nodus/tuvm/` paths across `review/`, 56 rewrite straight across, and these
> six do not —
>
> | cited | actually |
> |---|---|
> | `tools/run_c_tests.sh` | **DELETED** by rete#275. Every C test is now one row in `C_UNIT_TESTS` in `nodus/nsl/tools/run_tests_oevm.py`, which also owns `ENG_A`/`ENG_B` and `OEVM_BUILD=asan\|ubsan` |
> | `zephyr/NetBridgeNode.ns`, `NetNode.ns`, `ThreadEchoNodeDK.ns`, `TimerNode.ns` | `.nsl` — the extension changed 2026-07-18 |
> | `tools/profile_cost.py` | `nodus/oevm/zephyr/tools/profile_cost.py` |
>
> **Line-number citations (`file.py:158`) are not to be trusted at all** — several have
> already moved. Grep for the symbol. A pinned line number is a citation that rots at the
> next edit, which is how this file got here.
>
> Not swept, deliberately: `review/` is David's tree, and rewriting 117 references is his
> call, not a tidy-up. The rewrite is verified safe if he wants it (rete#277).

Memory corruption and UB are exactly the failure class invisible to nsl-level tests and
to a non-VM-guy reading C. One extra build config each buys detection of both.

## Build configs (add to the tuvm build + `run_tests_oevm.py` as `OEVM_BUILD=`)

| Config | Flags |
|---|---|
| `asan` | `-fsanitize=address,leak -fno-omit-frame-pointer -g -O1` |
| `ubsan` | `-fsanitize=undefined -fno-sanitize-recover=all -g -O1` |

Notes specific to this VM:
- **Precise GC vs ASAN:** the GC walks its own heap arena — fine under ASAN as long as
  the arena is one malloc'd block. If any code scans beyond object payloads
  (conservative leftovers), ASAN finds it — that is a feature, not a false positive.
- **`setjmp`/`longjmp` (Milestone D) under ASAN:** supported; ASAN's fake-stack must be
  told about the jump only if fibers/ucontext existed — they don't anymore. If reports
  appear at the crash boundary, that is a real finding about the recovery path.
- **LSAN + let-it-crash:** leaks *within the VM heap* are the GC's business (T-D.5
  covers creep); LSAN watches *C-level* allocations (image loader, seam buffers).
- UBSAN's `-fno-sanitize-recover=all` makes any UB a hard failure — intended, and
  **the runner passes exactly this as of 2026-08-29 (rete#277)**. It had passed
  `=undefined` since the deleted script; measured before changing it, the two are
  identical while `-fsanitize=undefined` is the only enabled sanitizer, and they
  diverge only once one outside that group is added — which is when a silently
  recovering check would be worst. The code moved to match this doc, not the reverse.

## Policy

- Full conformance suite under both configs **before every task-final review** and at
  every milestone gate (cheap: minutes on host).
- Any report = suite failure. No suppression files without a comment linking a decision.
- The C unit tests run under the same two configs — **all 43 of them**, via
  `run_c_unit_tests()` in `nodus/nsl/tools/run_tests_oevm.py`, which reads `OEVM_BUILD`.
  (Was `nodus/tuvm/tools/run_c_tests.sh`, deleted by rete#275: a second aggregate runner
  nothing invoked, so it sat red without anyone learning it.)
