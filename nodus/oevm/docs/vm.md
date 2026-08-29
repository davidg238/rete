# oevm — what the VM is, as of 2026-08-29

The one-page answer to "what kind of VM is this?". Everything here is true of the code
today; where a mechanism was tried and removed, the removal is named so an older document
cannot be mistaken for the present. The historical genesis documents are in
`nodus/oevm/docs/archive/` with banners saying what changed.

## In one paragraph

oevm is a small **register VM** in C (`nodus/oevm/src/`, ~27 k lines including the ports)
built on Ian Piumarta's **Open Extensible Object Model**: three primitive kinds — object,
symbol, vtable — and everything else, including arithmetic, is a late-bound message send
that a vtable answers. It runs **nsl**, a Newspeak-derived dialect compiled on the host
(`nodus/nsl/`) into an **`.oimg` object-graph image** that the resident VM loads and
executes in place. The execution model is **run-to-completion**: one flat, non-recursive
interpreter loop over one fixed-capacity stack, no fibers, no non-local return, no
growable stacks. Concurrency is a **reactor** — a tuple space of paths with `whenever:do:`
reactions and a timer pulse mailbox — supervised on a let-it-crash model. It targets Zephyr
boards (ESP32-C6 is the bench board) and a POSIX host for the suite.

## The instruction set

- **32-bit fixed-length instructions** in the Lua/Berry family (iABC / iABx layouts),
  operating on a per-frame register window `regs[]`. The choice of register over stack
  bytecode is a decision record: `nodus/oevm/docs/deployment-model.md` §Decisions (fewer dispatches per
  method, which is why Lua/Dalvik/Berry are register VMs).
- **31 opcodes, densely numbered `0x00`–`0x1E`** (`nodus/oevm/src/oevm.h`, `enum Opcode`).
  Loads, `SEND` / `SEND_IMPLICIT` / `SEND_SUPER` / `BLOCK_INVOKE`, `RET`, arithmetic and
  comparison with a small-integer fast path that falls back to a send, jumps, slot and
  lexical access, `CLOSURE`, and the heap-`Context` trio `MK_CONTEXT` / `LD_CTX` / `ST_CTX`.
- **`OP_SEND_IMPLICIT`** is Newspeak's implicit-receiver send — it walks the lexical
  enclosure chain before the inheritance chain — and is what makes nested classes and
  module-as-object work: `nodus/oevm/docs/op_send_implicit.md`.
- Selectors are **Symbols that carry their primitive id** (rete#226): a send whose selector
  names a primitive is intercepted by id, which is why a Symbol must never be hand-built
  (`nodus/oevm/src/prim_ids.h`, 142 ids).

## Execution model — what is NOT here, and why

Milestone A (2026-07-04, `nodus/oevm/docs/MILESTONE_A.md`) deleted, deliberately and measured:

| removed | replaced by |
|---|---|
| **Fibers** (`fiberCreate:` / `fiberSwitch:`, per-fiber C stacks) | the reactor: a reaction runs to completion, the next event waits |
| **Non-local return** (`OP_NLR`, `unwindTo:value:`) | a compile error — `^` out of an escaping closure does not compile |
| **Growable stacks** | one fixed-capacity stack; overflow aborts loudly |

What was **kept** from the same work and is load-bearing: the **flat non-recursive
interpreter** (one C loop, no C recursion per nsl call) and the **heap `Context`** that
lets a block outlive the frame that made it — `whenever: chan do: [:e | …]` captures a
temp from a frame that has since popped, and that is the whole reactive model.

Above that boundary: **exceptions** — `on:do:` with predicate filtering and `ensure:` —
land over a `setjmp` crash boundary (Milestone E, `nodus/oevm/docs/MILESTONE_E.md`), and a
**supervisor** contains a crashing handler, keeps the heap and reactor alive, and
restarts or quarantines the job (Milestone D, `nodus/oevm/docs/MILESTONE_D.md`).

## Memory

- A **fixed arena** (`OEVM_HEAP_SIZE`, compile-time; 16 MB host default, 128 KB on the
  C6) holding objects, the VM stack **and** the float arena. There is no malloc at
  steady state; the arena answers pressure, and an OOM on a board is usually
  fragmentation (`largest_free_block` is the number to read).
- **Precise mark-sweep GC** (`nodus/oevm/src/gc.c`): per-slab mark bits, sweep-time
  coalescing, roots enumerated rather than scanned. Small integers are tagged
  immediates and cost nothing.

## Images and deployment — Model B

- The compiler (`nodus/nsl/tools/ns_compile_oevm.py`, Ohm grammar in
  `nodus/nsl/grammar/`) emits an **`.oimg`** — a clustered object-graph serialization,
  **format version 10** (`nodus/oevm/src/oimg_loader.c`) — with a `.stmap` source map
  beside it. The VM is flashed once; images are pushed.
- The **kernel is nsl too**: `nodus/nsl/lib/kernel.nsl` holds the space, channels,
  timers, the CBOR codec and the porta bridge; `supervisor.nsl` the supervisor. An app
  sees the kernel instance as its `platform`.
- **Model A** — solidify the image to C and link it into the binary — survives as an
  opt-in appliance build (`OEVM_APPLIANCE`). Decision record: `nodus/oevm/docs/deployment-model.md`.

## The reactor, the space and the wire

- **Space** (`nodus/oevm/src/space.c`): a tree of paths; a leaf is a **cell** (latest
  value) or a **stream** (append-only), fixed by the first write. Apps hold
  **capabilities** — a channel rooted at a prefix, which can only narrow.
- **Timers** post to a **pulse mailbox** from the ISR; `pumpEvents` blocks on it and
  `drain` runs the reactions. On the host the timer backend bypasses the mailbox, which
  is why some gates need a board.
- **Liveness** (`nodus/oevm/zephyr/src/observer.c`, rete#215/#231): a C thread judges
  the reactor's **progress** against the shortest declared timer period; the kernel
  owns a base tick so a deadline always exists; a wedge dumps, reboots and leaves a
  `RebootNote`.
- **porta** (`porta/`, Go) mirrors each node's space over plain UDP + CBOR in two
  node-initiated messages, `pull` and `push`. The contract is `protocol/`. The two
  halves never import each other.

## Targets

Zephyr: **ESP32-C6** (the bench board; Thread radio), ESP32 DevKitC, ESP32-S2 (Saola,
Adafruit MagTag), nRF52840 DK and dongle — overlays under `nodus/oevm/zephyr/boards/`,
lessons in `nodus/oevm/zephyr/docs/`. POSIX host for the suite and the porta sync gate.

## Where the numbers are

Footprint and the Milestone-A delta: `nodus/oevm/docs/MILESTONE_A.md` §8 and
`nodus/oevm/tools/footprint.sh` (an aux gate of the suite). Interpreter performance work:
`nodus/oevm/docs/optimizations.md` (the log) and `nodus/oevm/docs/topics.md` (the why). Board measurements:
`docs/results/`. Cross-VM benchmarks: `~/workspaceToit/stic/README.md`, outside this repo
on purpose.
