# T0.1 — sanitizer gate (the highest-value single item in the whole test plan)

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
- UBSAN's `-fno-sanitize-recover=all` makes any UB a hard failure — intended.

## Policy

- Full conformance suite under both configs **before every task-final review** and at
  every milestone gate (cheap: minutes on host).
- Any report = suite failure. No suppression files without a comment linking a decision.
- The C unit tests (`nodus/tuvm/tools/run_c_tests.sh`) run under the same two configs.
