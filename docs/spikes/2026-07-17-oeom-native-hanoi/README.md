# Spike archive: OEOM native-substitution (Hanoi AOT ceiling) — 2026-07-17

Preserved copy of the throwaway spike that proved OEOM's native-substitution lever: a hot
user selector (`hanoiMove:from:to:temp:`) serviced by a hand-written C function stored as
a `PrimitiveClosure` in a vtable `Binding`, dispatched transparently through tuvm's
existing fork (`src/dispatch.c:1673`) — callers, interpreter, and `.oimg` format all
unchanged. **This folder is the durable archive so the code survives even if the live,
flag-gated version is later removed from the tree.**

## Result (ESP32 WROOM-32E, Model B, IRAM-pinned, means-of-3)

| Hanoi n | interpreted | native-in-vtable | speedup |
| --: | --: | --: | --: |
| 16 | 770 ms    | 6.67 ms | ~116× |
| 20 | 12,316 ms | 103 ms  | ~119× |

Native ~100 ns/move, linear, allocation-free (no GC). `moves == 2ⁿ−1` confirms the
`PrimitiveClosure` path fired (not a DNU). This is the *fully-inlined* AOT ceiling
(recursion collapsed to raw C) — a real bytecode→native compiler preserving send/GC
semantics lands between the interpreter and this bound.

## Contents

- `hanoi_native.c` — the native method (`prim_hanoi` + `hanoi_c`) and the lazy-wiring
  acquisition (`hanoi_native_acquire`, scans the receiver's vtable for the canonical
  interned selector). Copy of `src/hanoi_native.c`.
- `HanoiAB.ns` — the A/B bench node (same selector to bytecode `self` and native `hn`,
  means-of-3, n=16/20). Copy of `zephyr/HanoiAB.ns`.
- `hanoi_ab_host.sh` — host sanity harness (asserts `2ⁿ−1`). Copy of
  `tests/hanoi_ab_host.sh`.
- `spike.patch` — **the complete, re-applicable diff of all five code changes**,
  including the in-place edits that aren't standalone files: the `strcmp` acquisition
  gate in `src/dispatch.c` and the Zephyr build wiring in `zephyr/CMakeLists.txt`.

## Re-apply / reproduce

Everything is gated behind `OEVM_HANOI_NATIVE` (off by default).

```bash
# From a clean tuvm tree that no longer contains the spike:
git apply docs/spikes/2026-07-17-oeom-native-hanoi/spike.patch

# Host sanity (Phase 1 — asserts 2^n-1):
./tests/hanoi_ab_host.sh          # expect "HOST SANITY PASS"

# ESP32 headline (Phase 2):
source ~/zephyr-esp32/.venv/bin/activate
source ~/zephyr-esp32/zephyr/zephyr-env.sh
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4
OEVM_HANOI_NATIVE=1 OEVM_IRAM_HOT=1 \
  zephyr/run.sh --model B --node HanoiAB.ns --pristine \
  --build-dir build-esp32-hanoi --flash --capture 110
```

Note: the host link line adds `src/backend/$TARGET/timer_$TARGET.c` (a pre-existing gap
in `tests/test_load_run.sh`); `hanoi_ab_host.sh` already includes it.

## Full write-up (docs of record)

- Design (why + method): `docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md`
- Plan (task-by-task): `docs/superpowers/plans/2026-07-17-oeom-native-hanoi-spike.md`
- Numbers: `docs/superpowers/plans/2026-07-17-hanoi-ab-results.md`
- Ladder: `review/program-sequence.md` (Addendum 2026-07-17)

Spike commits on `feat/zephyr-model-ab`: `7b0a4bf..fb422eb` (code), plus results/ladder docs.
