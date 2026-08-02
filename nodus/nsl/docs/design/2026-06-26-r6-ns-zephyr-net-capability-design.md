# R6 — ns-zephyr net capability on hardware (design)

> **Status:** design (approved 2026-06-26). Rung **R6** of the Newspeak-node roadmap
> (`docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`). Next step
> after approval: writing-plans → implementation plan.

## Goal

Prove **the thesis on hardware**: a confined native capability swaps at the backend while
the capability model above it does not move. Concretely — bring the **Model B `.oimg`
loader** and the **R4 UDP networking capability** to Zephyr, swapping the POSIX backend
(`udp_posix.c`) for a Zephyr backend (`udp_zephyr.c`), and round-trip a real UDP datagram
from a Newspeak program running on a resident oevm VM — first on `native_sim`, then
(stretch) on the nRF52840 dongle over Thread.

This is R6 in the reshaped ladder: David queued a **jump over R5**, using **networking
(UDP) as the lead capability to port** because POSIX gave the desktop *real* UDP sockets
(unlike GPIO, which has no faithful host mock — GPIO stays deferred to a later rung,
blueprint `2026-06-26-gpio-capability-deferred-blueprint.md`).

## Scope (decided)

Three brainstorming decisions set the scope:

1. **Capability + Model B loader** (not capability-only; not the full keeper/porta node).
   R6 brings the `.oimg` Model B loader to the device *and* proves the net cap. The
   ns-posix keeper + porta-on-device remain deferred (that is the "full node", a later rung).
2. **native_sim first, then Thread.** Stage the proof: validate `udp_zephyr.c` against
   Zephyr's real net stack on `native_sim` (no radio, talks to host UDP, zero RAM pressure)
   before attempting OpenThread on the dongle.
3. **Flash blob, deserialize in place.** The `.oimg` bytes become a `const` flash array;
   the loader points `oimg_blob_base` directly at flash (XIP) — no RAM copy, no mutation.

S3 (dongle/Thread bring-up) is an **explicit stretch goal** — we aim for it, but R6 still
counts as done if S2 (native_sim) passes and OpenThread proves infeasible on the dongle's
RAM budget. Thread then becomes documented future work.

## Why this is small (the seam already exists)

R4 left two clean seams that make this a backend swap rather than a rewrite:

- **`oevm/src/udp_backend.h` — 6 functions, string addresses, backend owns the family:**
  `udp_open`, `udp_socket_bind`, `udp_send`, `udp_receive`, `udp_close`. Exactly one impl
  links per build from `src/backend/$OEVM_TARGET/`. The address-family-agnostic contract
  (plain string hosts, backend resolves) is what de-risks the IPv4/dual-stack → IPv6-mesh
  move: the facade and prim never see the family.
- **The `.oimg` loader is already position-independent.** Every blob-internal reference is
  stored as `(file_offset << 2) | tag` and resolved lazily via `oimg_blob_base + offset`
  (`oimg.h` `oimg_resolve` and the field accessors). The blob is **never mutated** — there
  is no pointer-fixup pass. So deserialize-in-place from read-only flash is sound today; the
  *only* POSIX dependency in `oimg_loader.c` is byte acquisition (`open`/`fstat`/`mmap`).

What R6 does **not** touch: `udp.c` (the capability prim), the `Net`/`Socket`/`Datagram`
facade, the `.oimg` format / header layout (stays format_version 6), the compiler/serializer.
Only the backend `.c` and the Zephyr build wiring are new — that *is* the demonstration.

## Architecture — four stages

### S0 — Loader seam (oevm, language-neutral)

Split `oimg_loader.c` into two entry points over one shared parser:

- `int oimg_load_blob(const uint8_t *base, size_t size)` — validates magic / `format_version`
  / `registry_version` / `blob_size`, reads the header ref-words, sets `oimg_blob_base` and
  `oimg_blob_end`. **No POSIX.** This is the Zephyr entry point; `base` points at the flash
  array.
- `int oimg_load(const char *path)` — keeps `open`/`fstat`/`mmap`, then delegates to
  `oimg_load_blob(map, sz)`. Desktop path stays byte-identical.

The `sys/mman.h` / `fcntl.h` / `unistd.h` includes and the mmap body are guarded so the TU
compiles for Zephyr, where only `oimg_load_blob` is reachable.

**Verification:** the desktop suite stays **72/0/4 on both paths** — the extraction is
behavior-preserving by construction (Path B already exercises `oimg_load`).

### S1 — Zephyr backends (oevm/src/backend/zephyr/)

**Discovered during planning:** Model B's link set differs from Model A's. Model A links only
`vm`/`gc`/`dispatch` + `solidified_image.c` + its own `main.c` (which provides blob stubs and a
direct console), so it needs *no* `platform.c`/`serial.c`. **Model B** links the full Path B set
(`vm gc dispatch platform serial udp mirror + backend/{platform,serial,udp}_<target> + oimg_loader
kernel_boot`), because `oimg_bootstrap` wires the kernel via `platform_object()` and the capability
prims live in `platform.c`/`serial.c`/`udp.c`. So bringing Model B to Zephyr needs **three** backend
files for the zephyr target, not one:

- **`udp_zephyr.c`** — the real work (the lead capability). Seam below.
- **`platform_zephyr.c`** — `platform_console_write`/`_flush` to the Zephyr console (the proof prints
  its result); `platform_debug_readline` is a `-1` stub (debug transport is out of scope for R6).
- **`serial_zephyr.c`** — satisfies the link (`serial.c` is in the Path B set); a net-only program
  never exercises it, so the loopback mock (target-neutral, identical logic to `serial_posix.c`) suffices.

The three seams are tiny (3 + 5 + 6 functions). The framing holds — *only backend `.c` files and
build wiring are new; the facade, prims, and `.oimg` format do not move.*

`udp_zephyr.c` implements the `udp_backend.h` seam against Zephyr's BSD sockets API:

| seam fn | Zephyr impl |
|---|---|
| `udp_open` | no-op (or one-time net-if up if needed) |
| `udp_socket_bind` | `zsock_socket(AF_INET6, SOCK_DGRAM, 0)`, `zsock_bind` to `in6addr_any:port`, set non-block via `zsock_fcntl(O_NONBLOCK)` |
| `udp_send` | resolve host string (`zsock_inet_pton` for numeric; address is numeric in the mesh case), `zsock_sendto` |
| `udp_receive` | `zsock_recvfrom` (non-block → -1 on EWOULDBLOCK), `net_addr_ntop` for `host_out`, fill `*sender_port` |
| `udp_close` | `zsock_close` |

IPv6-first (`AF_INET6`) to match the IPv6-only mesh. Addresses stay plain strings — the
backend owns the family, identical to the posix contract. **One file serves both targets**:
native_sim and the dongle differ only in the linked L2 / Kconfig, not in this C.

### S2 — native_sim Model B node (ns-zephyr) — the de-risked proof

A Model-B build variant alongside the existing Model A appliance:

- **`main.c` (Model B):** mirror the desktop `oevm_image_main.c` sequence —
  `kernel_boot()` → `oimg_load_blob(oimg_image, sizeof oimg_image)` → `oimg_bootstrap(oimg_root())`.
  Link the full Path B set: `vm gc dispatch platform serial udp mirror oimg_loader kernel_boot` +
  the three zephyr backends (`platform_zephyr.c`, `serial_zephyr.c`, `udp_zephyr.c`) + the embedded
  `oimg_image.c`. (Desktop uses `oimg_load(path)`; the only swap is the byte-acquisition call and the
  backend target.)
- **Embedded image:** the `.oimg` (existing serializer output) embedded as a generated
  `const uint8_t[]` `.c` — the Model-B analogue of `solidified_image.c`.
- **Kconfig `prj_nativesim.conf`:** `CONFIG_NETWORKING`, `CONFIG_NET_UDP`, `CONFIG_NET_IPV6`,
  `CONFIG_NET_SOCKETS` + **NSOS** (`CONFIG_NET_SOCKETS_OFFLOAD`, `CONFIG_NET_NATIVE_OFFLOADED_SOCKETS`).
  *Revised during implementation:* `CONFIG_POSIX_API` exports a bare `send()` that collides with
  oevm's own `send()` dispatch (multiple-definition), and in-stack `CONFIG_NET_LOOPBACK` needs an L2
  driver. NSOS — Zephyr's idiomatic native_sim socket path — sidesteps both by forwarding the
  `zsock_*` calls to the host Linux kernel, so `127.0.0.1`/`::1` are real host loopback.
- **Data flow / test:** the existing R4 `42_udp.ns` runs **unmodified** — it binds a socket, sends
  to `127.0.0.1` and `::1`, receives its own datagram back (host loopback via NSOS), and asserts
  `data = 'ping'`. No external echo server needed.

**Pass = the `zsock_*` seam + Model B `.oimg` loader (at native_sim's 32-bit ABI) + the R4 net
capability proven end-to-end in a Zephyr image, zero hardware.** This satisfies R6's must-have.
**Honest scope:** NSOS offloads sockets to the host kernel, so this does NOT exercise Zephyr's
**in-kernel** UDP/IPv6 stack — that (and the radio) are proven on the dongle in S3. native_sim
de-risks the seam, the loader, and the capability; S3 proves the stack.

### S3 — dongle Model B appliance (stretch)

Same image and `main.c`, retargeted to `nrf52840dongle_nrf52840`:

- **Image in flash:** `.oimg` blob is a `const` flash array; `oimg_blob_base` points at it
  directly (XIP, in place) — no RAM copy.
- **L2:** OpenThread (`CONFIG_NET_L2_OPENTHREAD`, IPv6-only mesh). Counterpart = a border
  router or a 2nd Thread node echoing datagrams.
- **Round-trip:** the same net-cap `.ns` program sends to the counterpart over Thread and
  receives the echo, observed on the USB-CDC console.

**Known risk (flagged):** OpenThread is ~tens of KB of RAM/flash on a board already at ~79%
RAM with the bare Model A VM. Mitigations: blob in flash (not RAM), shrink the managed arena
from its host default, the already-shrunk perf caches (`METHOD_CACHE_SIZE`/`PRIM_CACHE_SIZE`).
**Fallback:** if it will not fit, S2 stands as R6's proof and Thread bring-up is recorded as
future work (likely with a roomier board).

## Error handling

- **Loader:** returns non-zero on bad magic / unsupported `format_version` / registry
  mismatch / `blob_size` mismatch (behavior already present). The Zephyr `main.c` prints the
  failure to the USB-CDC console and halts cleanly rather than faulting.
- **Backend:** every `udp_*` returns -1 on socket error, mirroring posix; `udp_receive`
  returns -1 when nothing is queued (non-blocking). The `Net`/`Socket`/`Datagram` facade
  already copes with -1, so no facade change is needed.

## Testing strategy

- **S0:** desktop suite 72/0/4 both paths (no regression) — the gate that the loader split is
  behavior-preserving.
- **S1:** compiles for the Zephyr target; exercised functionally by S2 (no standalone C test
  harness on device — consistent with the existing backend pattern).
- **S2:** end-to-end on native_sim — bind → send → host echo → receive → expected print,
  diffed by the host harness. This is the primary R6 acceptance test.
- **S3 (stretch):** end-to-end on the dongle over Thread — same round-trip, observed on the
  USB console.

## Repos touched

| repo | change |
|---|---|
| `oevm` | S0 loader split (`oimg_loader.c`, guard POSIX includes); S1 `src/backend/zephyr/udp_zephyr.c`; build lists |
| `ns-zephyr` | S2/S3 Model-B build variants, Model-B `main.c`, embedded-`.oimg` generator, `prj_nativesim.conf` + dongle Kconfig, host echo + diff harness |
| `ns-tests` | a net-cap program suitable for the round-trip (reuse an R4 program if one fits) |
| `ns-oevm` | expected unchanged (compiler/serializer untouched) |

Per the oevm#10 decomposition, S0 (loader) and S1 (backend) are **language-neutral** — a
future `st-oevm` inherits them.

## Footprint — first-sniff sizing (interim)

> **Status: interim "first sniff," 2026-06-26.** Code (flash) numbers are exact — pulled per-object
> off the dongle `zephyr.elf` (`arm-zephyr-eabi-size`). RAM (bss) attribution is inference from
> object bss matching the known config knobs. Berry figures are from memory / Berry docs, not a build
> on this toolchain. A precise report (Zephyr `rom_report`/`ram_report`, a minimum-RAM build, and a
> bare BerryVM built on the same nRF52840 toolchain) is **deferred** — see below.

Measured on the loopback dongle build (Model B + in-kernel net stack, `-m32`, `OEVM_HEAP_SIZE=32 KB`,
`METHOD_CACHE_SIZE=PRIM_CACHE_SIZE=512`). Total image: FLASH 188972 B text / 18.5%; RAM bss 181347 B.

**Flash (code) — exact:**

| Piece | Flash | Note |
|---|---:|---|
| Interpreter (`vm.c` 7652 + `dispatch.c` 8308) | ~16 KB | bytecode loop + message dispatch/caching |
| GC (`gc.c`) | ~2.5 KB | mark-sweep collector |
| boot / kernel / platform / serial / udp / mirror / loader + 3 zephyr backends | ~5.5 KB | `udp_zephyr.c` is 346 B |
| **VM engine total** | **~24 KB** | **fixed — the actual "VM"** |
| Embedded `.oimg` image (`oimg_image.c`) | ~37 KB | program **+ class library**, serialized — scales with the program, not the VM |

Of the ~189 KB total flash, ~100 KB is Zephyr's **in-kernel net stack** (M1/M2, no networking, were
~84 KB flash total). OpenThread adds a further ~188 KB flash / ~54 KB RAM over the loopback build.

**RAM (static bss) — inferred from config:**

| Piece | RAM | Tunable? |
|---|---:|---|
| VM operand/call stack (`VM_STACK_MAX_WORDS`=16384 × 4 B) | ~64 KB | **yes** — generous default |
| Managed heap arena (`OEVM_HEAP_SIZE`) | ~32 KB | **yes** |
| Method + prim caches (512 each) | ~12 KB | **yes** — at host default 4096 these alone were ~160 KB |
| GC metadata + misc | ~10 KB | mostly fixed |
| **VM static RAM total** | **~118 KB** | **~108 KB is tunable** |

**Against BerryVM (first read):** the VM's irreducible cost is small and the heavy RAM is almost all
policy, not engine. Engine code (~24 KB flash) is competitive with — arguably leaner than — Berry's
core (~40 KB ROM per Berry docs); a Smalltalk-family VM under Berry's code size is the surprising-but-
real result. oevm is heavier in the **image** (~37 KB — the object/class graph is a bigger fixed
payload than a compact Berry script; grows slowly with classlib, not VM features). RAM defaults look
large (~118 KB) but ~108 KB is tunable — the 64 KB stack and 32 KB arena are deliberately generous,
and caches were already cut 8× to reclaim ~150 KB; the fixed RAM floor, once stack/arena are sized to
the workload, is low tens of KB. For a **networked** node the net stack (~100 KB flash, or
OpenThread's ~188 KB / ~54 KB) dwarfs the VM, so the language choice barely moves the total.

**Minimum-RAM build — MEASURED (2026-06-27, answers deferred item (b)).** Same dongle loopback image
at shrunk knobs `VM_STACK_MAX_WORDS=4096` (16 KB register file), `METHOD_CACHE_SIZE=PRIM_CACHE_SIZE=128`
(~3 KB), `OEVM_HEAP_SIZE=16 KB`: **RAM 110,344 B / 42.1%** (down from 185,608 B / 70.8% — **~73 KB
saved**), FLASH unchanged at 18.5% (code doesn't shrink). **`42_udp.ns` still prints `UDP OK`** at these
knobs (verified on native_sim). So the ~108 KB tunable RAM *more than halves* with no loss of function
for this workload — most of it free from the over-provisioned register file (a capacity, not a speed,
knob). For "scripting Zephyr functions" this is a comfortable floor (~150 KB headroom); it also relieves
the tight S3 OpenThread build (91% RAM at generous knobs).

**→ Full report (2026-06-29):** all three deferred items are now resolved in
`ns-zephyr/docs/2026-06-29-footprint-report.md` — exact `rom_report`/`ram_report` trees
(engine refined to **19.9 KB** flash, VM RAM 120,973 B), the minimum-RAM build (110.3 KB /
42.1 %), and the oevm-vs-Berry head-to-head (size + Perfstex speed). The notes below are the
original deferral list, kept for context.

**Deferred to the full report:** (a) Zephyr `rom_report`/`ram_report` per-symbol tree; (c) an **oevm-vs-Berry
head-to-head on the same nRF52840 — size *and* speed in one pass**: run a benchmark from
`ns-tests/00_perf` (or `05_somns_benchmarks`) on the oevm node vs the equivalent on `st-zephyr-b`
(Berry on Zephyr, already the behavioral oracle), giving apples-to-apples flash/RAM + throughput.
Note: both are **register-based** VMs (verified — `vm.c` uses three-address register ops + register-
windowed sends, no push/pop), so this measures implementation maturity + the dynamic-dispatch
(inline-cache-miss) tax, **not** a stack-vs-register architectural difference.

## Out of scope (deferred)

- Keeper + porta on the device (the "full node" — a later rung; R6 is capability + loader).
- GPIO and other capabilities (GPIO blueprint deferred; R5 breadth skipped per David's jump).
- `.oimg` delivery over the air / over USB (flash blob is the R6 delivery mechanism).
- Fibers (R7).
