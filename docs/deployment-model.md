# Deployment model: resident VM + OTA'd image (Model B)

> **Status note, 2026-08-29.** The decisions below stand and Model B is what ships. Details
> that moved on since: the `.oimg` header is at format version **10** (`nodus/oevm/src/oimg_loader.c`);
> the keeper is no longer `nodus-st` on Berry but the nsl kernel on oevm itself; and porta
> delivers nothing over TFTP — images ride the bulk plane (`porta blob put`) and the space
> (`goal/apps`). Current architecture in one page: `vm.md`.

Status: **BUILT & SHIPPED**, updated 2026-06-25 (decisions recorded 2026-06-17).
Model B is now the **primary, default** deployment path: the resident VM loads a
clustered `.oimg` at runtime and runs it execute-in-place. The full conformance
suite passes on **both** paths (Path A solidify-to-C *and* Path B `.oimg`-loaded —
70/0/4), at both word sizes (64-bit and 32-bit via per-target builds). Model A
(solidify-to-C) is retained as an opt-in **appliance** build behind
`OEVM_APPLIANCE`. Implemented in `src/oimg_loader.c` + `src/kernel_boot.c` +
`oevm_image_main.c` (VM side) and `../ns-oevm/tools/oimg_serialize.py` (host
serializer); `.oimg` header is at format_version 5 (kernel / serial-capability /
Supervisor-debug descriptors). See the decoupled-XIP design + plan in
`docs/superpowers/` for the as-built detail.

## Decisions (2026-06-17)

1. **Deployment = Model B** — resident VM + supervisor (nodus keeper) running
   OTA'd container images; oevm replaces the Berry VM in `nodus-st`.
2. **Keep oevm's existing bytecode** — the **register-machine** set (Lua/Berry
   family). We are NOT switching to Cog/Newsqueak stack bytecode. (Naming note:
   "our bytecode" = the register set; "stack-based" refers to Cog/Soup, which we
   are *not* adopting.)
3. **Image format = clustered serialization (the architecture, à la
   Fuel/Parcels/`.vfuel`), NOT Soup's exact `.vfuel`.** Define an oevm-native
   image format with those properties (clustered, architecture-portable,
   deserializer-only, partial-graph messages); we cannot and will not load Soup's
   `.vfuel` blobs directly.

## The decision

oevm targets embedded/IoT (cf. sibling repos `st-zephyr`, `toit-zephyr`,
`berry`). The goal is **provisioning in the `nodus`/`porta` model: a fixed VM
resident on the device, with program containers delivered over-the-air and
executed under a supervisor.** This is **Model B**.

Concretely, the deployment world is already built around this shape:

- **`porta`** — the gateway. Owns the wire protocol; distributes container
  images to nodes over UDP/TFTP.
- **`nodus` (Toit) / `nodus-st` (Smalltalk)** — the on-device **keeper /
  supervisor**: it polls porta, installs the delivered container image, runs it
  per its declared lifecycle, deep-sleeps, and re-polls (with watchdog,
  restart/abandon, panic capture). The keeper core is portable C (`nodus-st/src/`)
  on a Zephyr/nRF52840 firmware.
- Today `nodus-st` compiles Smalltalk via **st-berry → `.bec` → the Berry VM**.

**oevm's role is to become the VM the keeper supervises — replacing the Berry
VM.** The "core" is then *oevm (the VM) + nodus (the container supervisor)*. The
image-format question below is therefore the same as "what container payload does
porta deliver and the keeper install."

This is a deliberate move *away* from how oevm builds today:

- **Model A — AOT-to-native (current).** `solidify` emits the whole image as C
  (`src/solidified_image.c`) and fuses it with the VM into one `gcc -O2` binary.
  The image lives in `.rodata` and runs in place — zero load cost, no on-device
  compiler, gcc optimizes the static image. But the artifact is a single,
  arch-specific, whole-application binary. OTA = ship a new firmware blob; no
  granular or live update; the "live image" property is lost.
- **Model B — resident VM + serialized image (chosen).** Ship the VM once as
  firmware. OTA a serialized **object-graph image**; the resident interpreter
  loads it and runs from an initial message. Small, arch-independent payloads;
  one VM hosts many containers; supports update. Cost: on-device interpretation
  speed matters (no gcc optimizing your static image), so the open perf levers
  (inline caching, unboxed floats — see `optimizations.md`) become first-order.

Primordial Soup is the reference implementation of Model B: "the VM starts by
deserializing a snapshot into a heap, then interprets bytecode from an initial
message" (`primordialsoup/docs/design.md`).

## What Model B requires of oevm

1. **Separate the image from the VM binary.** Today the image is `#include`d
   into the VM build. Model B needs the VM to be standalone firmware that
   *loads* a container image at runtime (handed to it by the keeper).
2. **An on-device loader (deserializer).** The VM gains a deserializer; the
   serializer can live on the build host / in the frontend (it need not ship to
   the device — keeps the runtime small).
3. **A serialized image format** (see "Image format" below). The image is an
   object graph: vtables/classes, interned symbols, compiled methods (bytecode),
   and instance data, rooted at an object store + an initial activation. It must
   be a single deliverable **blob** (porta TFTPs it; the keeper installs it).
4. **Fit the keeper lifecycle.** The supervisor runs a container, deep-sleeps,
   and re-polls; it watchdogs and may restart/abandon. So the VM needs: cheap
   **start-from-image** (ideally cheap *re-*start after wake — see zero-copy
   note), bounded/observable execution the supervisor can watchdog, and clean
   teardown. Fault isolation between containers (isolate-style) is desirable so
   one container's panic doesn't take the node down.
5. **Container/message story.** Inter-container delivery (actor-style) wants the
   same format carrying *partial* graphs — so OTA deltas and message passing fall
   out of one mechanism.

---

## Followup 1: our bytecode vs Cog's

**Where oevm sits.** oevm has its **own register-machine bytecode** — 16 opcodes,
3-address (op + A/B/C operands packed in a 32-bit word):

```
LD_CONST LD_REG SEND SEND_IC RET NLR ADD SUB MUL DIV
JMP JMP_IF_FALSE LD_SLOT ST_SLOT NEW SEND_IMPLICIT   (+ BREAK)
```

This is a **register architecture** in the Lua / Dalvik / **Berry** family
(fixed-width instructions, explicit register operands, the frontend does
register allocation). Note the lineage is *architectural*, not literal: oevm's
opcodes are its own — the retired `ns-berry` path emitted actual Berry bytecode,
whereas `ns-oevm` emits the register set above. The two are not interchangeable.

**Cog / Newsqueak V4 (Soup's bytecode)** is a different paradigm: a
*variable-length stack-machine* bytecode "derived from the Newsqueak V4 bytecode
of the Cog VM" (`primordialsoup/docs/design.md`). Operands are implicit on an
expression stack; it has compact send bytecodes, dedicated special-selector
bytecodes (arithmetic etc. inlined as bytecodes), closure-creation bytecodes,
and a direct bytecode for eventual (async) sends.

| | Cog / Newsqueak (Soup) | oevm |
|---|---|---|
| Model | stack machine | register machine |
| Encoding | variable-length | fixed 32-bit, 3-address |
| Operands | implicit (expr stack) | explicit register indices |
| Instrs executed / method | more (push/pop traffic) | fewer |
| Per-instruction dispatch | more frequent | less frequent |
| Code density | higher (compact) | lower (3 operands) |
| Compiler effort | low | higher (register allocation) |
| Special selectors | dedicated bytecodes | primitive interception in `dispatch.c` |
| Blocks | closure-creation bytecode | inlined, else separate compiled method |
| Async/eventual send | dedicated bytecode | not yet modelled |

**Assessment.** The register design is the better *interpreter* substrate for
embedded (fewer dispatches per method is why Lua/Dalvik/Berry are register VMs),
and `ns-oevm` already produces it. Cog's stack bytecode leans on a JIT to be
fast — which Soup does **not** have, and which we are not planning. **Decided:
keep the register bytecode.** The cost of that choice is the one below: our
image cannot be Soup's image.

---

## Followup 2: can we support `.vfuel`? (we have to pick something)

Two different questions hide here.

**(a) Run Soup's `.vfuel` snapshots on oevm — no.** A `.vfuel` snapshot encodes
*Soup's* object model and *Soup's* stack-bytecode compiled methods. oevm's object
model (vtable/delegate, see `compiler.md`) and register bytecode differ on both
axes. Consuming `.vfuel` directly would mean adopting Soup's VM wholesale —
i.e. becoming Soup. Not viable, and it would throw away the register-bytecode
decision above.

**(b) Adopt the `.vfuel` *design* for an oevm-native image format — yes, this is
the decision.** `.vfuel` is not a memory dump and not a recursive
serialization; it is **clustered serialization** in the style of Pharo's
[Fuel](http://rmod.inria.fr/web/software/Fuel) and Parcels
(`primordialsoup/docs/design.md`, "Snapshots"). Its properties are exactly what
Model B on constrained targets wants:

- **Clustered** (group objects by class, write all nodes before all edges):
  type info once per cluster, and deserialization is a tight unconditional
  fill-loop — cheap in code and time on an MCU.
- **Architecture-portable** (word-size independent): one build can be OTA'd to a
  32-bit ESP32 and a 64-bit host alike. A classic Smalltalk image (raw dump +
  pointer fixups) is *not* portable this way — reject that option.
- **Deserializer-only in the VM**: the serializer is host-side code, so the
  device runtime stays small.
- **Partial graphs for messages**: inter-isolate messages reuse the same format
  with a partial graph over an implicit common set — i.e. the container-message
  / OTA-delta mechanism is the same code as image loading.

So: **don't pick `.vfuel`-the-format; pick clustered serialization as the
architecture and define an oevm-native image format around oevm's object graph**
(vtables, symbols, compiled methods in oevm register bytecode, instances; rooted
at an object store + initial activation).

### Format options weighed

| Option | Portable | Load cost | Cyclic graphs / identity | Verdict |
|---|---|---|---|---|
| Raw image dump + fixups | ✗ (word size) | low | native | reject (not OTA-portable) |
| Recursive serialization (Dart-style) | ✓ | medium | native | workable, slower load |
| **Clustered (Fuel/Parcels/.vfuel)** | ✓ | low (linear) | native | **recommended** |
| CBOR / protobuf | ✓ | medium | hand-rolled | reinvents graph handling |
| FlatBuffers (zero-copy) | ✓ | ~zero (mmap) | awkward (cycles, `become`) | see note |

**Zero-copy note (more attractive here than usual).** A zero-copy format run in
place from flash/NVS (no deserialize into RAM) is the embedded ideal, and it
rhymes with what Model A already does (image in `.rodata`, run in place). It is
especially compelling under the keeper's **deep-sleep / re-poll** lifecycle:
install the image to flash once, then run it in place across many wake cycles
with no per-wake deserialize. It is also the hardest to design (GC and
`become:`/identity over an immutable flash-resident region), so treat it as a
later optimization layered on the clustered format — but design the clustered
format with an eventual in-place/`mmap` variant in mind (e.g. relocatable or
offset-based references rather than absolute pointers).

---

## Path — done and remaining

**Done (shipped):**

1. ✅ **Decouple** the image from the VM build — the VM is standalone firmware
   that takes an image path; `oevm_image_main.c` supplies `main`,
   `solidified_image.c` is no longer compiled in the default build.
2. ✅ **Define the oevm image format** + host serializer + on-device loader —
   clustered, portable, execute-in-place (`oimg_serialize.py` host-side;
   `oimg_loader.c` + `kernel_boot.c` device-side). Proven at 64- and 32-bit.
5. ✅ **Keep solidify-to-C** as an opt-in *appliance/release* build behind
   `#ifdef OEVM_APPLIANCE` (`src/oevm_main.c`), so the two builds do not collide.

**Remaining:**

3. **Reuse the format for messages** (partial graphs) to get container delivery —
   designed-for, not built.
4. **Invest in the interpreter** — under Model B you feel the full steady-state
   gap to Soup (~1.9× geomean, `../ns-tests/00_perf/RESULTS.md`), so inline
   caching (`OP_SEND_IC` is defined but never emitted) and unboxed floats move
   from "nice" to "load-bearing".
6. **Harden the loader path** — the immutability guard (abort, not SIGSEGV, on a
   write to a blob object) and reachability-based emission (prune orphan methods
   from the blob — payload size matters for the porta/TFTP budget) are filed
   follow-ups, not yet built.

## Build lines

### Model B — default build (resident VM, loads a `.oimg` image at runtime)

The VM links the interpreter core, the capability seams (`platform`/`serial`/
`mirror`), the per-target backend that implements those seams (selected by
`$OEVM_TARGET`, default `posix` — see `oevm-target-decomposition`), the loader,
the firmware kernel boot, and the standalone image `main`:

```bash
TARGET="${OEVM_TARGET:-posix}"
gcc -Wall -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c \
    src/platform.c src/serial.c src/mirror.c \
    "src/backend/$TARGET/platform_$TARGET.c" \
    "src/backend/$TARGET/serial_$TARGET.c" \
    src/oimg_loader.c src/kernel_boot.c \
    oevm_image_main.c \
    -o oevm_img -lm
```

Produce a `.oimg` host-side (per-target ABI; `dump_layout` emits the layout
descriptor the serializer consumes), then run it:

```bash
gcc -std=c99 -Isrc tools/dump_layout.c -o dump_layout && ./dump_layout > layout.json
python3 ../ns-oevm/tools/ns_compile_oevm.py <program.ns> app.oimg --format=oimg --layout=layout.json
./oevm_img app.oimg
```

`src/oevm_main.c` and `src/solidified_image.c` are **not** compiled in this
build; `oevm_image_main.c` supplies `main`. This is exactly the Path B build the
regression gate uses (`../ns-oevm/tools/run_tests_oevm.py`), which also runs the
32-bit (`-m32`) portability gate (`tests/test_portability_32.sh`).

> **Format invariant (32-bit-load-bearing):** the `.oimg` header's descriptor
> ref-words are a **dense word-strided** block — the loader reads each at
> `a + n*W` (W = `sizeof(oop)`, `a = ALIGN_UP(20, W)`) with **no** inter-group
> padding, so the serializer must pad descriptor groups to `W`, not to a fixed
> 8. On 64-bit `W==8` masks any divergence; on 32-bit it does not. The whole
> header is separately rounded to 8 so the object region stays double-aligned.

### Model A — appliance build (solidified image fused into VM binary)

Generate the image C source first (host-side, using the ns-oevm compiler tool):

```bash
python3 ../ns-oevm/tools/ns_compile_oevm.py <program.ns> src/solidified_image.c
```

Then build:

```bash
gcc -DOEVM_APPLIANCE -Wall -std=c99 \
    src/vm.c src/gc.c src/dispatch.c \
    src/solidified_image.c src/oevm_main.c \
    -lm -o oevm_app
```

The `-DOEVM_APPLIANCE` flag activates `src/oevm_main.c`'s `main` (otherwise
that translation unit is empty). `src/oimg_loader.c`, `src/kernel_boot.c`, and
`oevm_image_main.c` are **not** compiled in this build.

## Open questions

- GC interaction with a loaded (vs. baked-in) image: which clusters are
  immutable/ROMable, and can the collector skip them? (Prerequisite for the
  zero-copy/flash-resident variant.)
- Versioning/identity across OTA updates: how does an updated image rebind to
  live device state, or do containers start fresh each install? Ties into nodus's
  NVS-survival and restart/abandon model (`nodus/docs/RELIABILITY.md`).
- Do containers share one heap/VM (cheaper) or get isolate-style separation
  (Soup's model; better fault isolation, capability story)? The keeper's
  watchdog/restart/abandon semantics argue for enough isolation that one
  container's panic is recoverable without resetting the node.
- Division of labour between the **C keeper** (`nodus-st/src/`) and the VM: the
  supervisor stays native (poll/install/lifecycle/watchdog); oevm is the managed
  execution engine it hands an installed image to. Where exactly is the seam
  (process? thread? VM-embedded API)?
- Payload size vs porta/TFTP: clustered serialization is compact, but confirm
  typical container images fit the gateway's delivery/chunking budget.
