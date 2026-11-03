# Rung 5 — UDP transport (Layer 1) native_sim proof Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the Zephyr `udp_zephyr.c` UDP backend functionally round-trips a datagram on native_sim (it currently only *links*), completing Layer 1 of the transport arc.

**Architecture:** The host `net` capability is already complete and green on both image paths (`nsl-tests/04_somns_basic/42_udp.ns` PASS A+B; suite 179/0/12). The Zephyr backend links on native_sim (the rung-2 pulse gate enables NSOS networking so `dispatch.c`'s unconditional `udp_object()` probe resolves) but has never moved a real datagram. This rung bakes a `net`-granting Model-A node into `tuvm/zephyr/` — reusing the pulse gate's node-agnostic `main.c`/`CMakeLists.txt`/`prj.conf` unchanged — and runs it on `native_sim` to observe a loopback round-trip. No C, facade, compiler, or host-test change. String→Bytes payload is deferred to Layer 2 (see the design §0/§4.1).

**Tech Stack:** Newspeak dialect (the gate node), Zephyr `native_sim` + west/NCS SDK, Python (`ns_compile_oevm.py` Model-A solidifier), the existing tuvm C VM.

## Global Constraints

- **Repos & branch.** Work touches **`~/workspace/tuvm`** (the gate node + zephyr README) and **`~/workspace/nsl-tuvm`** (design/plan/dialect docs) and **`~/workspace/tuvm/review/`** (program-sequence). **`nsl-tests` is untouched this rung.** Branch `rung5-udp-transport` in **tuvm** and **nsl-tuvm** before Task 1. Merge/push ONLY on David's explicit say-so (finishing-a-development-branch).
- **No source edits to the VM/facade/compiler/host tests.** If any task finds itself editing `src/udp.c`, `kernel.ns`, `ns_compile_oevm.py`, or an `nsl-tests` `.ns`, STOP — it is out of scope (except a `udp_zephyr.c` backend fix if and only if native_sim exposes a `zsock_*` symbol/behavior bug — the one allowed C edit, §Task 1).
- **Model A only on Zephyr.** `tuvm/zephyr/` excludes `oimg_loader.c`/`kernel_boot.c`; the node is baked into `src/solidified_image.c` by the solidifier. There is no `.oimg` path on device.
- **`solidified_image.c` is a generated artifact** shared by other tests. Building the gate CLOBBERS `tuvm/src/solidified_image.c` + `tuvm/src/solidified_image.stmap`. ALWAYS restore before committing anything in tuvm:
  ```bash
  git -C ~/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
  ```
- **Zephyr build needs the SDK env** sourced each shell: `source ~/workspace/ns-zephyr/tools/env.sh` (sets `ZEPHYR_BASE`, west, the `arm-zephyr-eabi`/native_sim toolchain).
- **Controller owns every full-suite run.** Implementers run single-file/host checks foreground only.
- **Commits:** commit after each task; end every commit message with:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01McJ8j3dpJmtV1bVpVWiXqK
  ```

---

## Task 1: native_sim functional UDP round-trip (the `udp_zephyr.c` gate)

**Files:**
- Create: `tuvm/zephyr/NetNode.ns` — the Model-A gate node declaring `net:`, mirroring `42_udp.ns`'s round-trip.
- (Transient, restored) `tuvm/src/solidified_image.c` + `.stmap` — baked from `NetNode.ns` for the build, restored after.
- No change to `tuvm/zephyr/{main.c,CMakeLists.txt,prj.conf}` — verified node-agnostic (main.c:94-119 boots + runs the entry and already handles a `main:` that returns; the pulse-GPIO block compiles out on native_sim).
- Allowed-only-if-needed: `tuvm/src/backend/zephyr/udp_zephyr.c` — a `zsock_*` symbol/behavior fix if native_sim exposes one (the backend's own caveat).

**Interfaces:**
- Consumes (already present): the `net` capability spine — `Kernel>>netOn:`, `class Net`/`Socket`/`Datagram` (`nsl-tuvm/lib/kernel.ns:968-1003`), the Path-A `net` boot-threading (`ns_compile_oevm.py:2389-2396`), `udp_zephyr.c` + its link in `tuvm/zephyr/CMakeLists.txt:28`, and the NSOS networking block in `tuvm/zephyr/prj.conf`.
- Produces: a committed `tuvm/zephyr/NetNode.ns` and a documented, reproducible native_sim round-trip (console prints `UDP OK`).

- [ ] **Step 1: Create the gate node `tuvm/zephyr/NetNode.ns`**

Mirror `42_udp.ns`'s round-trip (self-contained in the tuvm repo, like `PulseNode.ns`). It binds a receiver, sends to loopback, polls the non-blocking receive until the datagram arrives, checks the payload, prints the sentinel, and returns (main.c idles afterward).

```newspeak
(*
NetNode — native_sim UDP round-trip gate. Model A (baked into
src/solidified_image.c). Declares the `net:` capability, binds a receiver on a
fixed port, sends a datagram to loopback, polls the non-blocking receive until
it arrives, and prints the sentinel. Mirrors nsl-tests 42_udp.ns's roundTrip:
logic — the point here is to exercise src/backend/zephyr/udp_zephyr.c's
zsock_* send/recv over Zephyr's native_sim NSOS loopback (the host build proves
the same node against udp_posix.c). main: returns after printing; zephyr/src/
main.c then idles (K_FOREVER) — the gate is checked by console output under
`timeout`, not by process exit.
expect:
UDP OK
*)
class NetNode usingPlatform: platform net: net = () (
  public main: platform2 args: args = (
    (roundTrip: '127.0.0.1') ifFalse: [ ^ self fail: 'v4 loopback' ].
    (roundTrip: '::1')       ifFalse: [ ^ self fail: 'v6 loopback' ].
    'UDP OK' println.
    ^ 0
  )
  roundTrip: host = (
    | rx tx got |
    rx:: net udpBindPort: 54332.
    rx == nil ifTrue: [ ^ false ].
    tx:: net udp.
    tx sendTo: host port: 54332 data: 'ping'.
    got:: nil.
    [ got == nil ] whileTrue: [ got:: rx receive ].   (* loopback is ~instant *)
    rx close. tx close.
    ^ got data = 'ping'
  )
  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Prove the node logic on HOST Path A first (isolates node bugs from the Zephyr backend)**

This solidifies `NetNode.ns` and runs it on the host (posix backend), separate from any native_sim/SDK concern. Run from the nsl-tuvm tools dir:

```bash
cd ~/workspace/nsl-tuvm/tools
OEVM_DIR=~/workspace/tuvm OEVM_PATH=A python3 - <<'PY'
from pathlib import Path
import run_tests_oevm as R
p = Path('~/workspace/tuvm/zephyr/NetNode.ns')
print('A', R.run_test_file(p))
PY
git -C ~/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
```

Expected: `A ('PASS', ...)` — the node prints `UDP OK` over the posix backend. (If it fails, the bug is in the node, not the Zephyr backend — fix `NetNode.ns` before proceeding.)

- [ ] **Step 3: Bake `NetNode.ns` into the solidified image for the Zephyr build**

```bash
source ~/workspace/ns-zephyr/tools/env.sh
cd ~/workspace/nsl-tuvm
python3 tools/ns_compile_oevm.py \
  ~/workspace/tuvm/zephyr/NetNode.ns \
  ~/workspace/tuvm/src/solidified_image.c
```

Expected: exit 0; `tuvm/src/solidified_image.c` now bakes `NetNode` (and, via the `net` param, the `udp_object()` grant threaded by `ns_compile_oevm.py:2389-2396`).

- [ ] **Step 4: Build the native_sim image**

```bash
cd ~/workspace/tuvm/zephyr
west build -b native_sim -p always . 2>&1 | tee /tmp/rung5_build.log | tail -20
```

Expected: `west build` completes (the CMakeLists already lists `udp.c` + `udp_zephyr.c`; prj.conf already enables NSOS). If it fails on a `zsock_*` symbol mismatch, that is the backend caveat — fix the symbol in `tuvm/src/backend/zephyr/udp_zephyr.c` only (the `udp_backend.h` contract is fixed), rebuild.

- [ ] **Step 5: Run native_sim and observe the round-trip**

```bash
cd ~/workspace/tuvm/zephyr
timeout 8 ./build/zephyr/zephyr.exe 2>&1 | tee /tmp/rung5_run.log
grep -q 'UDP OK' /tmp/rung5_run.log && echo GATE_PASS || echo GATE_FAIL
```

Expected: `GATE_PASS` — the console shows `UDP OK`, i.e. `udp_zephyr.c`'s `zsock_socket`/`zsock_bind`/`zsock_sendto`/`zsock_recvfrom` actually round-tripped a datagram over NSOS loopback.

> **Fallback (if NSOS offloaded loopback drops the datagram — the `[ got == nil ] whileTrue:` spins to the timeout):** NSOS offloads to the host's real sockets; loopback should work like posix. If it does not, change `NetNode.ns` to a two-socket same-process exchange is already what it does (rx+tx are distinct sockets) — the remaining variable is loopback delivery. Try binding rx to `'::'`/ephemeral and sending to the discovered port, or add `CONFIG_NET_LOOPBACK=y` to `prj.conf` (native_sim-only; a genuine backend fix). Re-run Step 4–5. This is the one place the outcome cannot be predicted without running.

- [ ] **Step 6: Restore the clobbered generated image (MANDATORY before any commit)**

```bash
cd ~/workspace/tuvm
git checkout -- src/solidified_image.c src/solidified_image.stmap
git status --short   # expect ONLY zephyr/NetNode.ns as new (and maybe udp_zephyr.c if fixed)
```

Expected: the working tree shows only `zephyr/NetNode.ns` (untracked) plus any deliberate `udp_zephyr.c`/`prj.conf` fix — `src/solidified_image.c`/`.stmap` clean.

- [ ] **Step 7: Commit (tuvm)**

```bash
cd ~/workspace/tuvm
git add zephyr/NetNode.ns
# also add udp_zephyr.c / prj.conf ONLY if a native_sim fix was needed:
# git add src/backend/zephyr/udp_zephyr.c zephyr/prj.conf
git commit -m "feat(rung5): native_sim UDP round-trip gate (NetNode.ns) — udp_zephyr.c proven

Bakes a net-granting Model-A node and round-trips a datagram over Zephyr
native_sim NSOS loopback: proves src/backend/zephyr/udp_zephyr.c's zsock_*
send/recv functionally (previously link-only). Reuses the pulse gate's
node-agnostic main.c/CMakeLists/prj.conf unchanged.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01McJ8j3dpJmtV1bVpVWiXqK"
```

---

## Task 2: Close — gates confirm, docs, program-sequence

**Files:**
- Modify: `tuvm/zephyr/README.md` — add a short "native_sim net round-trip" section (recipe from Task 1, mirroring the existing pulse section).
- Modify: `tuvm/review/program-sequence.md` — the Phase-2/3 net line.
- Modify: the dialect/vocabulary doc that owns `net` — `nsl-tuvm/docs/NEWSPEAK_DIALECT.md` if it has the serial (`#13`) entry; else the tests README net line. (Confirm by grep; do NOT invent a doc.)
- No code change; the host suite is untouched, so its counts are unchanged by construction.

**Interfaces:** none new — closes the rung.

- [ ] **Step 1: Confirm the host suite + five named gates are unchanged (controller-owned)**

Because no VM/facade/compiler/test source changed, this is a confirmation, not a re-bank. Run the full both-path suite and the five named gates per the project's standard gate recipe (the same commands the rung-4 close used; see `.superpowers/sdd/progress.md` gate lines). 

Expected: **179 passed / 0 failed / 12 skipped** both paths; **grant / debugger-integration / portability-32 / boot-gc-asan / footprint** all PASS. The mcu footprint gate must read the unchanged baseline (`tuvm/tools/footprint-mcu-baseline.txt` = text 29182 / data_bss 69036) — no source changed, so it must match with no re-bank.

- [ ] **Step 2: Verify the footprint baseline is untouched**

```bash
cd ~/workspace/tuvm
git diff --stat tools/footprint-mcu-baseline.txt   # expect: no change
FOOTPRINT_CONFIG=mcu ./tools/footprint.sh 2>&1 | tail -3   # if the SDK env is sourced; else SKIP is acceptable
```

Expected: baseline unchanged; gate PASS (or SKIP if the cross toolchain isn't on PATH — SKIP is the documented no-SDK behavior, not a failure).

- [ ] **Step 3: Document the native_sim net gate in `tuvm/zephyr/README.md`**

Add a section after the pulse sections, e.g.:

```markdown
# Rung 5 — native_sim UDP round-trip gate (NetNode.ns)

Proves the Zephyr UDP backend (`../src/backend/zephyr/udp_zephyr.c`) functionally
round-trips a datagram on `native_sim` (previously it only linked). The baked
node `NetNode.ns` declares the `net:` capability, binds a receiver, sends to
loopback, polls the non-blocking receive, and prints `UDP OK`. Model A; reuses
this directory's `main.c`/`CMakeLists.txt`/`prj.conf` unchanged (the pulse-GPIO
block compiles out on native_sim, and `main()` already handles a `main:` that
returns).

## Build + run

    source ~/workspace/ns-zephyr/tools/env.sh
    # bake NetNode.ns into the solidified image (from the nsl-tuvm repo)
    cd ~/workspace/nsl-tuvm
    python3 tools/ns_compile_oevm.py \
      ~/workspace/tuvm/zephyr/NetNode.ns \
      ~/workspace/tuvm/src/solidified_image.c
    cd ~/workspace/tuvm/zephyr
    west build -b native_sim -p always .
    timeout 8 ./build/zephyr/zephyr.exe   # expect a line: UDP OK
    # restore the generated image before committing anything in tuvm
    cd ~/workspace/tuvm
    git checkout -- src/solidified_image.c src/solidified_image.stmap
```

- [ ] **Step 4: Update `tuvm/review/program-sequence.md` net line**

Find the Phase-3 capability-breadth line (currently ~line 110: *"OpenThread/UDP (r6 design re-validated; UDP prims exist in tuvm today, fiber-free entry points need review)"*) and reconcile it to as-built. Add/adjust to record: **Layer 1 net capability DONE — host both-path (green in the suite) + native_sim functional round-trip (rung 5); Layer 2 (reactive channel↔UDP bridge, CBOR + pumpEvents eventing) and Layer 3 (real OpenThread on the nRF52840-DK) named as the next transport rungs.** Keep it an index line pointing at this design/plan.

- [ ] **Step 5: Add the `net` dialect/vocabulary note**

```bash
grep -n "serial" ~/workspace/nsl-tuvm/docs/NEWSPEAK_DIALECT.md | head
```

If a serial capability row exists, append a sibling `net` row in the same column format (confined native cap; `net udpBindPort:`/`udp` → `Socket`; non-blocking `receive` → `Datagram` (`data`/`senderHost`/`senderPort`) or nil; String payloads for now — Bytes at Layer 2; real POSIX sockets on host, Zephyr NSOS on native_sim, OpenThread on the DK deferred to Layer 3). If no such doc/row exists, SKIP this step (do not invent a doc) and note the net vocabulary in the tests README instead.

- [ ] **Step 6: Commit the docs (tuvm + nsl-tuvm)**

```bash
cd ~/workspace/tuvm
git add zephyr/README.md review/program-sequence.md
git commit -m "docs(rung5): native_sim net gate recipe + program-sequence Layer-1 done

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01McJ8j3dpJmtV1bVpVWiXqK"

cd ~/workspace/nsl-tuvm
git add docs/NEWSPEAK_DIALECT.md   # only if edited in Step 5
git commit -m "docs(rung5): net capability dialect note

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01McJ8j3dpJmtV1bVpVWiXqK"
```

- [ ] **Step 7: Update the SDD ledger + hand off for merge decision**

Append a RUNG 5 close block to `nsl-tuvm/.superpowers/sdd/progress.md` (branch HEADs, the native_sim GATE_PASS evidence, suite still 179/0/12 + 5 gates, footprint unchanged). Then STOP — merge/push is David's call (finishing-a-development-branch).

---

## Self-Review

**Spec coverage (design §2 in-scope):**
- native_sim functional round-trip → Task 1 (Steps 3–5, the `udp_zephyr.c` gate).
- host suite + five gates stay green → Task 2 Steps 1–2 (confirmation; no source change).
- `net` dialect/docs + program-sequence + footprint confirm → Task 2 Steps 3–5.
- Deferred items (Bytes, bridge, CBOR, DK OpenThread, `16_net/` move, serial-XOR-net) → not tasked, per design §2 out-of-scope. ✓

**Placeholder scan:** No TBD/TODO. The one deliberately-empirical point (NSOS loopback delivery) is flagged with a concrete fallback (Task 1 Step 5). The dialect-doc step is guarded ("SKIP if no such doc — do not invent") because the doc's existence is uncertain. ✓

**Type/name consistency:** `NetNode` / `net:` param / `net udpBindPort:` / `net udp` / `sendTo:port:data:` / `receive` / `Datagram>>data` all match the live facade (`kernel.ns:968-1003`) and `42_udp.ns`. Port 54332 (distinct from 42_udp's 54331 to avoid any host-run collision). Sentinel `UDP OK` matches the `(* expect: *)` header. ✓

**Scope guard:** Global Constraints forbid VM/facade/compiler/host-test edits; the only allowed C touch is a native_sim-exposed `udp_zephyr.c` symbol fix, explicitly bounded. ✓
