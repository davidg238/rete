# Zephyr rung-2, sub-project 1 — boot-the-VM + ISR→channel pulse primitive

**Status:** DESIGN 2026-07-08 (David-approved in brainstorm; ready for a `writing-plans`
implementation plan). Owner rung: `tuvm/review/program-sequence.md` Phase 2 rung 2
("Zephyr bring-up … first work order: the **ISR→channel pulse primitive**"). This spec
covers the *first* sub-project of that rung only; the rung is decomposed below.

**Entry criterion:** Milestone E green (phase-2 rung 1, exceptions) — met 2026-07-07.

---

## 1. Scope — decomposed from the full rung

`program-sequence.md`'s "Zephyr bring-up" bundles several independent subsystems. This
spec deliberately takes the thinnest vertical slice plus the two named precursors, so the
first hardware bring-up proves the ISR→channel path end-to-end and nothing else.

**In scope**

1. Cross-build the tuvm engine for Zephyr; boot the solidified image on target.
2. The **ISR→channel pulse** seam end-to-end: one GPIO-edge ISR → ISR-safe mailbox →
   safe-point drain → `space_post` → reactor observes the event → observable output.
3. **M6 C-boundary handle table** (G3 decision §M6 — "adopt with the Zephyr seam work
   order"): C holds VM object references as small root-table indices, never raw oops.
4. Precursor **FOOTPRINT_CONFIG=mcu** (tuvm#5): a real `arm-zephyr-eabi` ROM/RAM line.
5. Precursor **OEVM_BUILD=asan|ubsan**: a sanitizer knob on the host C-test harness.

**Explicitly deferred to later rung-2 sub-projects**

- The **I²C blocking-transaction seam** (`i2c write:to:` / `read:from:`). Nothing to
  reuse — greenfield against `<zephyr/drivers/i2c.h>`; its own spec. Big buffers stay in
  C per G3 §M1.
- **Timers on Zephyr** (`k_timer` backend replacing Milestone D's host virtual clock).
- Running the whole **demo-node `TempNode`** on target (needs I²C + timers).
- Heavy **on-device GC / fragmentation** validation (G3 §M2 telemetry soak, §M5 trigger).

**Targets (David-chosen): sim as gate, hardware as bar.** Develop and CI-gate on Zephyr
`native_sim`; the sub-project is not "done" until it also runs on a physical nRF52840
dongle. Matches the standing Zephyr directive — the gate is *memory, not speed*.

---

## 2. Architecture

### 2.1 The pulse backend seam

New backend header `src/pulse_backend.h`, mirroring the existing `platform_backend.h` /
`serial_backend.h` seams — exactly one impl linked per `OEVM_TARGET`:

```c
/* ISR-safe: called from a GPIO/timer interrupt. Non-allocating, non-blocking.
   Enqueues (chan_id, handle_idx) at the mailbox tail; drops nothing on the VM heap. */
void pulse_post_isr(int chan_id, int handle_idx);

/* Thread context, called at the run-to-completion safe point. Drains every queued
   (chan_id, handle_idx): resolves handle_idx -> oop via the M6 table, then space_post. */
void pulse_drain(void);
```

- **`src/backend/zephyr/pulse_zephyr.c`** — a `K_MSGQ_DEFINE(pulse_q, sizeof(pulse_item),
  DEPTH, 4)` lifted from `st-zephyr/.../test-dongle-05-jast/src/hal/zephyr/jast_event.c`.
  `pulse_post_isr` = `k_msgq_put(&pulse_q, &item, K_NO_WAIT)`; `pulse_drain` loops
  `k_msgq_get(..., K_NO_WAIT)` until empty. A full mailbox is a Tier-2 crash with a
  FROZEN string (`"pulse mailbox full"`), consistent with `space.c`'s capacity policy.
- **`src/backend/posix/pulse_posix.c`** — degenerate: a plain in-process ring (no irq,
  no threads). The host mock GPIO posts through it in thread context so host and target
  share the C code path and the same nsl-level tests.

**Why a mailbox and not a direct `space_post` from the ISR** (decided in brainstorm,
option B): tuvm is single-stack, run-to-completion; `space_post` mutates the Space ring
buffer that `Space>>drain` is reading. A direct ISR call races on that ring on the
single-core nRF52840 and would push irq-safety into the shared Space core. The mailbox
keeps `space.c` irq-free — ISRs only `k_msgq_put`; the VM heap and Space ring are touched
only in thread context. This is the "C seam carries data, not control" discipline
(`space.h` header) and the safe-point model kept since Milestone A.

### 2.2 The M6 C-boundary handle table

The mailbox must carry a VM-object reference (the value to post, e.g. `#done`) across an
asynchronous boundary that GC cannot see. Per the ratified G3 decision §M6, C never holds
a raw oop; it holds an index into a small VM-managed root table.

New in `src/gc.c` (or a sibling `handles.c` it includes), a fixed JNI-global-ref-style
table:

```c
int  handle_new(oop o);     /* pin o; return a small stable index (or crash if table full) */
oop  handle_get(int idx);   /* resolve an index to its current oop */
void handle_free(int idx);  /* release the pin */
```

- The table is **marked as a GC root** — every live handle keeps its oop reachable, so
  the value survives arbitrarily many GC cycles while it waits in the ISR slot / mailbox.
- Because the oop is rooted *and* the collector is non-moving (verified: `gc.c` is
  mark-and-sweep with free-list coalescing — no compaction/relocation; lines ~662/668/788),
  a resolved address is stable. Non-moving is now a *supporting* property (identity
  stability), not the load-bearing one — the handle table is the mechanism.
- Fixed capacity (dozens of entries; `-D` override, embedded-arena style). Exhaustion is
  a Tier-2 crash with a FROZEN string (`"handle table full"`).
- This satisfies G3 §M4 (no new heap-address-derived durable state) and §M6, and is the
  reusable foundation every later C driver (I²C, `k_timer`) holds VM references through.

### 2.3 The reactor run loop — nsl owns the loop, C owns only the blocking pump

The block-until-next-event and the ISR-safe mailbox transfer are the only parts that *must*
be C — an nsl program cannot block on a Zephyr kernel object, and the transfer touches the
irq-shared mailbox. Everything else — the loop itself and the order of pump-vs-drain — is
control, so it stays nsl policy. C therefore exposes a single **mechanism** primitive, not
a loop:

```
platform pumpEvents               (C)
  k_msgq_get(&pulse_q, K_FOREVER)     # block until an ISR posts (returns at once if non-empty)
  drain ALL queued items:             # thread ctx
    for each (chan_id, handle_idx):  space_post(chan_id, handle_get(handle_idx))
```

nsl owns the reactor loop:

```smalltalk
[ platform pumpEvents. space drain ] repeat
```

`space drain` is the existing Milestone-C nsl reactor, unchanged; `pumpEvents` blocks
*inside* the running interpreter activation and returns when work arrives. Because the loop
is one continuous nsl activation, the interpreter is **not re-entered from C per cycle** —
this avoids by construction the un-rooted-oop-across-`run_interpreter`-re-entry hazard a
C-owned loop would create (the recurring project bug); the pulse value is rooted through
the M6 table regardless. On host, `pumpEvents` blocks on the posix in-process ring (or
returns immediately under the Milestone-D test rig, which drives events via
`advanceMillis:`), so the same `[...] repeat` node source runs both places; only `main:`
wiring differs (demo-node's host/target split).

**Lost-wakeup safety (pinned):** an ISR that posts between `space drain` finishing and the
next `pumpEvents` block is not lost — `k_msgq_get` returns immediately when the queue is
non-empty, and `pumpEvents` drains *all* queued items per call, so an event posted during
drain is serviced on the next iteration. Identical requirement on the host ring.

**Deferred-feature fit:** an nsl-owned loop is also what lets the later `k_timer` backend
post to the same mailbox transparently (no `pumpEvents` change), and lets the demo-node's
deep-sleep mode be a supervisor *policy* decision ("sleep instead of pump") rather than
logic buried in C — both consistent with the run-to-completion / capabilities-as-policy
model. (Earlier brainstorm choice was a fully C-owned `runReactor`; refined to this split
because the loop and the pump-vs-drain ordering are policy and belong in nsl.)

---

## 3. The pulse primitive — nsl surface and flow

A **GPIO capability** granted as a constructor argument (Milestone-C rule: capabilities
are object references handed in at setup, never values inside messages). Thinnest surface:

```smalltalk
gpio pin: n edge: #rising post: #done on: aChannel
```

**Registration (thread context):**
1. Resolve/intern `aChannel` → `chan_id` (`space_intern`).
2. `handle_new(#done)` → `handle_idx` (pins the value oop in the M6 table).
3. Configure the edge interrupt from `be_native_button.c`'s pattern:
   `gpio_pin_interrupt_configure(..., GPIO_INT_EDGE_RISING)`, `gpio_init_callback`,
   `gpio_add_callback`. Store `(chan_id, handle_idx)` in the ISR slot for this pin.

**On edge (ISR context):** the handler does only `pulse_post_isr(chan_id, handle_idx)` —
no allocation, no heap touch.

**On drain (thread context):** `pulse_drain` pops `(chan_id, handle_idx)`, resolves
`handle_get(handle_idx)` → the `#done` oop, and `space_post(chan_id, #done)`. The reactor
sees `#done` on `aChannel` exactly as any other event.

The demo-node's `i2c onAlertPost: #done to: chan` becomes a thin facade over this
GPIO-edge primitive in the later I²C sub-project (the demo-node "what this proved" finding
#2 called for exactly this generalization).

---

## 4. Build, targets, precursors

### 4.1 Reuse map (nothing greenfield except the seam logic itself)

| Need | Reuse from | Gives |
|---|---|---|
| VM-on-Zephyr boot main | `ns-zephyr/app/src/main.c` (+ its `CMakeLists.txt`) | USB-CDC settle, `zephyr_millis()` clock hook, `gc_set_stack_base()` conservative roots, run entry point (~48 lines). Env-var tunable pattern (NCS sysbuild swallows `-D`). |
| Toolchain / env | `ns-zephyr/tools/env.sh` | The only in-repo SDK pin: NCS `2ac5840438`, `ZEPHYR_BASE=~/ncs/zephyr`, global `~/ncs` west workspace. |
| ISR-safe mailbox | `st-zephyr/.../test-dongle-05-jast/src/hal/zephyr/jast_event.c` | `K_MSGQ_DEFINE` + `k_msgq_put(K_NO_WAIT)` / `k_msgq_get`. |
| GPIO edge ISR wiring | same tree's `port/be_native_button.c` | `GPIO_INT_EDGE_*` + `gpio_init_callback` / `gpio_add_callback`; dongle LED/pin map. |
| Direct-from-ISR post reference | same tree's `src/hal/zephyr/jast_timer.c` | cleanest ISR→queue example (used later for the `k_timer` backend). |

Board string `nrf52840dongle/nrf52840`; `native_sim` via the `build_ns_node.sh` path.
`prj.conf`: cherry-pick the USB-CDC / flash lines from st-zephyr's; drop its OpenThread
stack for a minimal bring-up.

### 4.2 FOOTPRINT_CONFIG=mcu (tuvm#5)

Extend `tools/footprint.sh`: behind `FOOTPRINT_CONFIG=mcu`, cross-compile the engine
objects with `arm-zephyr-eabi-gcc -Os` and emit a real ROM(`text`)/RAM(`data+bss`) line
for the true target, replacing the host-proxy caveat. `review/tests/t0-gates/ratchet.sh`
already expects the `mcu` config behind this flag — this wires it to a real number. The
pulse seam + handle table are the first engine additions the mcu line must absorb without
tripping the ratchet.

### 4.3 OEVM_BUILD=asan|ubsan

A harness knob on the host C-test / suite path (`tools/run_c_tests.sh`,
`tools/run_tests_oevm.py`): rebuild with `-fsanitize=address` / `-fsanitize=undefined` for
sanitizer coverage of the new ISR-adjacent C (mailbox ring, handle table). Host-only; the
target build is unaffected.

---

## 5. Testing / acceptance

- **C unit** (`tools/run_c_tests.sh`, beside `test_serial`/`test_udp`): the posix pulse
  backend round-trips — `handle_new` → `pulse_post_isr` → `pulse_drain` → `space_post`
  FIFO order; `handle_get` resolves; `handle_free` releases; table-full and mailbox-full
  crash with their FROZEN strings. Runs under `OEVM_BUILD=asan` too.
- **nsl gate (host):** a new `nsl-tests/13_pulse/` suite — register a GPIO pulse against
  the mock GPIO capability, fire the edge, assert the reactor observed `#done` on the
  channel; assert an unregistered channel drops (P4 transient). Runs in the full AB suite
  (controller-owned, ~10 min).
- **native_sim (CI bar — the mergeable gate):** the same node source boots under
  `native_sim`; a simulated edge drives one pulse; console shows the reaction.
- **nRF52840 (acceptance bar — the rung's "done"):** flashed to a physical dongle, a real
  GPIO edge (button / a jumpered ALERT pin) drives one pulse to the console; banked
  alongside the FOOTPRINT_CONFIG=mcu ROM/RAM line.

---

## 6. Pinned constraints & open items

1. **C holds M6 handle indices, never raw oops** (G3 §M6). The mailbox and ISR slots
   carry `(chan_id, handle_idx)`. Supersedes the naive "hold an interned Symbol oop"
   shortcut; makes the pulse value general (any oop) and GC-safe.
2. **ISRs never touch the VM heap or the Space ring** — only `k_msgq_put`. `space.c`
   stays irq-free.
3. **The reactor loop is nsl (`[ platform pumpEvents. space drain ] repeat`), not C**
   (§2.3) — so the interpreter is not re-entered from C per cycle and the un-rooted-oop-
   across-re-entry hazard (recurring project bug) is avoided by construction. `pumpEvents`
   blocks inside the running activation; the pulse value is rooted via the M6 table.
4. **Pulse mailbox / handle table are fixed-capacity**, `-D`-overridable, exhaustion =
   Tier-2 crash with a FROZEN string (`"pulse mailbox full"`, `"handle table full"`),
   matching `space.c`'s capacity-crash policy.
5. **Non-moving GC is a verified supporting property** (`gc.c` mark-and-sweep + free-list
   coalescing, no relocation), not a new assumption. Cited: G3 decision doc.

**Resolved during design** (no longer open): the G3 GC decision doc exists at
`nsl-tuvm/docs/superpowers/specs/2026-07-04-G3-gc-strategy-decision.md`; the collector is
confirmed non-moving; M6 governs the C-held-reference design.

---

## 7. References

- Rung owner / sequence: `tuvm/review/program-sequence.md` (Phase 2 rung 2).
- Pulse-primitive need (readme-driven finding #2): `tuvm/review/demo-node.ns`.
- GC strategy + M6 handle table + M1–M6: `nsl-tuvm/docs/superpowers/specs/2026-07-04-G3-gc-strategy-decision.md`.
- ISR-callable seam substrate: `tuvm/src/space.h` (`space_post`, `space_intern`), `tuvm/src/space.c`.
- Host event-source precedent: `tuvm/src/platform.c` (Milestone D host timer → `space_post` on `sys/timer`).
- Backend seam pattern: `tuvm/src/platform_backend.h`, `tuvm/src/serial_backend.h`, `tuvm/src/backend/{posix,zephyr}/`.
- Reuse sources: `ns-zephyr/app/`, `ns-zephyr/tools/env.sh`,
  `st-zephyr/tests/dongle/test-dongle-05-jast/` (`jast_event.c`, `jast_timer.c`, `port/be_native_button.c`).
- Exceptions/crash boundary this composes with: `tuvm/review/lib/exceptions.md`, `tuvm/docs/MILESTONE_E.md`.
</content>
</invoke>
