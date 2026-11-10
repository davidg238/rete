# rete#477 — where the arena goes, and safe-point compaction

*Design note, 2026-11-09. Status: **proposed, not built.** The measurements are host-only (`-m32`, the boards'
32-bit layout), on master `1001a48a`; the numbers and the method are in #477's comments.*

## The question

Would compiling nsl save RAM? No: the bytecode is already in flash. RAM is the arena, and the arena is
decided by its **peak**. So the question became what fills the arena at its peak, and why a node needs about
twice its live data.

## What was measured

PortaBridgeNode, the real check-in app, ran against porta through a copy of `porta_sync_host.sh`. Its VM was
patched to count objects and bytes by class at every sweep, and to describe the free space when memory ran out.

| Finding | Number |
|---|---|
| Live at peak | 27 KB, of which **16 KB is the VM stack reserve** (`VM_STACK_INIT_WORDS=4096`) |
| Stack actually used (high-water) | 3.6 KB (896 words; a 768-word stack overflows at "need 792") |
| Smallest arena: 16 KB stack / 4 KB stack | 44 KB / **32 KB**: the stack cut saves 1:1 |
| At the 28 KB out-of-memory point | live 14.5 KB; free 13.8 KB in 81 pieces, the largest 972 B; the request was a 1032 B `ByteBuilder` doubling |
| Captured block variables + closures | 43 % of bytes allocated, ~0.6 KB of peak live: escape analysis saves GC time, not arena |
| Size segregation (cmpctmalloc's idea: large objects from the top, small from the bottom) | **no gain**, on AWFY or on the app (detail below) |
| One kernel-held 1 KB encode buffer, reused by `Cbor encode:` | **26 KB passes**; 24 KB fails on the 570 B exact copy `frozenBytes` makes |

The segregation spike is what located the problem. The large-object region stayed clean (the stack, the
bridge, `Kernel`). The holes were all among the ~300 small objects that survive, scattered over ~22 KB. A
placement policy cannot undo scatter in a heap whose objects never move; cmpctmalloc has the same limit,
since a page with one survivor never comes back whole.

## The levers, cheapest first

1. **Size the stack reserve from a measured high-water**, per board, plus a margin. It is a build knob.
   A stack overflow aborts the node and cannot be caught, so the high-water must be measured on the board,
   including paths the host never runs (BLE, the debugger, sensors).
2. **Remove the large transient buffers on the encode path.** The scratch builder is −6 KB measured. The
   `frozenBytes` exact copy is next: hand the scratch's bytes and count to the send, or freeze a slice.
3. **Safe-point compaction** (below), which removes the scatter itself.

Do 1 and 2 first and measure again. Compaction is earned only if, after them, the smallest working arena is
still well above live, and the out-of-memory picture is still "enough free, no piece big enough". If it is,
it is a falsifiable claim: compaction should bring the smallest arena close to peak live plus the largest
single request.

## Safe-point compaction

**The rule in one sentence: the ordinary collector stays non-moving, and a sliding compaction runs only
at a point where no primitive is running, so every pointer into the arena sits in a slot the VM knows
about and can rewrite.**

### Why not an object table

Every reference except one kind is already precise:

| Who holds an arena pointer | Movable? |
|---|---|
| VM stack frames (`scan_vm_stack`, precise) | yes |
| C globals marked in `gc_collect`, `space_gc_trace`, the handle table (`src/handles.c`, already a C-boundary object table), temp roots | yes, each is a known slot |
| **C local variables inside a primitive that is running** | **no** |
| `identityHash` = address >> 4 (`src/dispatch.c` `PRIM_identityHash`); `Object>>hash` defaults to it | no |
| Frames point into the middle of the Stack | pin the Stack |

An object table would put an indirection on every access to fix one case. The safe point removes that case
for free: when no primitive is on the C stack, no C local holds a pointer.

### Where the safe point is

Where the event loop is about to sleep, and not on every sleep: only when the last sweep says compaction
would pay (degenerate trigger: the largest free block is smaller than some request seen since boot). A node
like the check-in app is idle most of the time, so there is plenty of opportunity.

### What it costs

- **`identityHash` must stop being the address.** Either store a hash (header bits; the header is 4 B since
  #37), or rehash the identity-keyed collections after each compaction. To decide with data: count how many
  objects ever get an identity hash.
- **A collection triggered inside a primitive still cannot compact.** An allocation that fails there kills
  the node as it does today. Compaction lowers how often that happens; it does not remove it.
- **Forwarding addresses** need somewhere to live during the slide. Candidates: Jonkers pointer threading (no
  extra space), or a per-chunk offset table computed from the live-object bitmap the arena already keeps. Not
  chosen here; the first build takes the simplest one that fits in the arena, and we measure.
- **Time**: one extra pass over the heap, at idle, when the trigger fires.

### What would falsify it

If the smallest working arena, with compaction on, is not close to peak live plus the largest request, the
scatter was not the cost, and this design is wrong.

## Not chosen

- **Compiling nsl ahead of time**: speed, not RAM (#477's first comment).
- **Escape analysis**: GC time, not arena (see the table above).
- **Size-segregated placement / cmpctmalloc**: measured, no gain (see above).
- **Moving inside every collection**: needs every primitive audited for C locals across allocations
  (18 allocation sites in `platform.c` alone), or an object table on every access.

## Addendum 2026-11-09 (later the same day): timing, and two corrections

- **The collector, timed** (host, `-m32 -O2`, CPU time; `CENSUS TIMING` in the census tooling): the check-in app
  takes 21.5 µs per collection on average and 35 µs at worst, at 32 KB. Mark plus roots is ~75 % of that. The
  CBOR spike measured the C6 at 125–160× this host for C code, which puts a collection at ~3 ms (~5 ms worst) on
  the C6. A board measurement is still owed (#421).
- **Correction: compaction costs ~6–10 ms on the C6**, not the ~0.7 ms first estimated, because it starts with
  this full mark. That is still ≤0.5 % of a 2 s check-in.
- **Correction: the host build also scans the C stack conservatively** (`nodus/oevm/docs/optimizations.md`, W1).
  An object found that way cannot be moved, so the host must pin it, or skip that scan at the safe point.
- **The encode scratch had a re-entry hazard**, now closed. A debugger stop inside an encode runs the held loop,
  which encodes. Only the outermost `encode:` takes the scratch now, and `ensure:` gives it back
  (`14_cbor/07_reentrant_encode.nsl`). With the guard, the check-in app passes at 26 and 24 KB and fails at 22 KB
  (on a 677 B request).

## Addendum 2026-11-10: lever 2 done, lever 1 measured on two boards

- **`frozenBytes` copies once.** It built an exact-size copy and then `freezeBytes:` copied that again. The prim
  is now `platform freezeBytes: raw size: n` (same id), one allocation of exactly `n`. The host check-in app
  passes at **22 KB** (it failed at 22 before) and fails at 20 KB: free 5.9 KB in 59 pieces, the largest 612 B,
  the request 677 B. Peak live is 15.9 KB with a 4 KB stack.
- **Stack high-water, exact** (`OEVM_STACK_HW=1`: the maximum taken at every frame push, printed through the
  console when it rises). C6 `PortaBridgeNodeC6`, bridge-health PASS: **1077 words**. MagTag (debug+profile
  recipe): **1053 words**, the same on a cold boot and on a strike wake. The host runs the same apps at 799–874.
  So a board needs roughly 200–250 words more than the host, and the host cannot size a board's stack.
- **Not yet measured**: a debugger stop (the held loop runs under the stopped frame), an OTA stage and activate,
  BLE. Until they are, the stack sizes stay as they are. The proposal is **1536 words** (measured maximum × 1.4).
  On the MagTag that frees 2 KB of a 34 KB arena. The margin is a guess. A stack overflow kills the node, and on
  the S2 it stays dead until someone presses RESET (no watchdog, no reset on a fatal error, #479).
- **Is compaction earned?** By this note's own test, yes on the host: the smallest arena (22 KB) is 6 KB above
  peak live, and running out of memory still looks like "enough free, no piece big enough". The falsifiable claim
  stands: with compaction the check-in app should run in about 16.6 KB (peak live plus the 677 B request).
  Building it is David's call; nothing in this addendum starts it.
