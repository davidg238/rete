# The cost of a send and its frame: measure first, then cut (rete#37)

> Status: **DRAFT, answered by David 2026-10-12:** Q1 take L1 and L2 now (M follows with them, not
> before); Q2 visibility deferred today, but it is ultimately a mandatory language feature (so L1 is
> scoped below); Q3 not understood, no decision (explained below, still open); Q4 port Are We Fast
> Yet first, for the widest pool of data (`nodus/tests/00_perf/awfy.nsl`, `AWFY.md`), and compare
> against Primordial Soup on the host (`awfy-soup.py`). Queued on #37 (2026-10-12 comment) as a design step. Follows the
> 10-11 send-cost spike (`docs/spikes/2026-10-11-37-c6-send-cost/` on branch `37-c6-send-cost-spike`), Opt #10
> (`docs/results/2026-10-11-37-lean-dispatch/`) and Opt #11 (`docs/results/2026-10-11-37-dispatch-head/`).
> Questions for David are numbered at the end.

## Why

On c6spare after Opt #11 (`docs/results/2026-10-12-37-c6-cross-vm/`), nsl beats Toit on 5 of 8 STIC rows, but
**Toit is 5.9× faster on Richards (7.0 s against 1.18 s a run) and 7.1× faster on DeltaBlue.** Both benchmarks
are send-heavy and object-heavy. The 10-11 spike priced a monomorphic send on the C6 at **~707 cycles**: ~85 for
the lookup (`send_resolve`) and ~620 for the frame push, the callee's instructions and the return. That
reading predates Opt #10 and Opt #11, which took 12–24 % off every blockcost row, so **today's send cost is
unknown.** So is the share of Richards it accounts for.

## What a send does today (code read, master `f82ba961`)

`OP_SEND` → `send_resolve` → `push_frame` → the callee → `OP_RET` → `do_ret_deliver`. On Zephyr the debug hooks
are compiled out (`OEVM_NO_DEBUG_HOOKS`), so neither the loop head nor `step_hook` costs anything there.

| step | where | what it costs per send |
|---|---|---|
| send setup | `vm.c` `OP_SEND` | selector from the constant pool, `saved_pc` store, 7-argument out-of-line call with three out-pointers (forced to memory) |
| `send_resolve` preamble | `dispatch.c:1282` | before the cache: the `main:args:` guard, the BoundClass test, the `Symbol =` test, three capability pointer compares, `prim_id` (a call), the BytecodeClosure `value` test |
| lookup | `lookup_method_cached` | hash, one probe, two compares (cheap: the spike's ~85 cycles covers the whole of `send_resolve`) |
| access check | `check_access_allowed`, every bytecode target | a call and a load. **Vestigial:** the compiler emits every method as `ACCESS_PUBLIC` (dialect row 25). It is never refused. |
| frame push | `vm.c:825` `push_frame` | `closure_entry` (an `is_in_blob` branch), stack bound check through `g_active_stack`, six header stores, `++current_generation_tag` (a global load and store), arguments copied, **every other register stored NULL**, `current_frame` stored |
| after the push | `OP_SEND` | `ctx = current_frame` reloaded, `closure_entry` **again**, `cm_instructions` |
| return | `do_ret_deliver` | decode the caller link, store `vm_stack_ptr` and `current_frame`, `closure_entry` a third time, reload `saved_pc`, re-decode the caller's send instruction for its destination register |

`vm_stack`, `vm_stack_ptr` and `current_frame` are globals, so on RV32 every touch is an address load plus the
access. A send reads or writes them about ten times. No single row above explains 620 cycles. The spike's own
conclusion stands: **count before cutting.**

## Plan

**M — the measurement (one build, one c6spare session, before any lever).**

1. **A counting build** (`OEVM_COUNT=1`, never shipped): three counters (instructions dispatched, sends that
   pushed a frame, block invokes) printed by the bench. Run blockcost, Richards and DeltaBlue on the host and on
   c6spare at master. This gives **cycles per instruction** and **instructions per send**, both measured, and
   **sends per Richards run**. With the per-send cost, that last number says what share of Richards' 7 s is the
   send. If it is under about a third, the send is not Toit's lead, and this design stops and points elsewhere
   (slot access through `instance_slot`'s `is_in_blob` test, `ifTrue:`, allocation).
2. **A cycle split** (`rdcycle`, a throwaway build): around `send_resolve`, `push_frame` and `do_ret_deliver`
   on the `methodSend` row. This apportions the per-send cycles to the steps in the table above.

**L — the levers, ranked by what they remove for what they cost.** The data from M picks the order; each lever
is one ledger row, measured alone on c6spare (blockcost + Richards + DeltaBlue) and on the WeAct STIC.

- **L1 — delete the per-send access check.** It never refuses (dialect row 25), so this removes a
  half-mechanism, not an optimization trade. Visibility itself stays: David (Q2) wants it eventually as a
  mandatory language feature, so the grammar keyword stays. When visibility is built, it gets its own design,
  and that design should look for a check that costs nothing per send: at compile time where the receiver is
  `self` or implicit, and at cache fill otherwise (a private method fills the cache only for a self-send).
  Removes a call per send.
- **L2 — one `push_frame`, inlined into the loop, the method computed once.** `push_frame` returns the method
  it already resolved, so the send site and the return site stop calling `closure_entry` again.
  `run_interpreter`'s copy of the frame push is folded into the same helper (it duplicates `push_frame` today).
- **L3 — the frame state lives in locals.** `vm_stack`, `vm_stack_ptr` and `current_frame` are kept in the
  loop's registers and written back to the globals only before a call that can read them (a primitive, GC, a
  nested interpreter). The risk is a missed write-back. The rule is one macro at every call-out, and
  `GC_STRESS` is the test.
- **L4 — a smaller frame.** Three header words have cheaper forms. (a) `vtable` is NULL until the frame is
  reified, which the debugger and `thisContext` do. (b) A method frame's `home_ctx` encodes itself; NULL could
  mean "self" at the `home_ctx_frame` decode point. (c) `gen_tag` costs a global increment per frame and exists
  to catch stale homes (Contexts, `make_closure`). Only (c) needs a design decision (Q3). The registers past the
  arguments are stored NULL because the GC scans them precisely. That store stays. It is a cost per register, so
  M should also record `num_registers` per frame.
- **L5 — the `send_resolve` preamble moved behind the cache.** The special cases (`main:args:`, BoundClass,
  `Symbol =`, capabilities, prims) are tested on every send before the probe. Most of them could become
  vtable or selector entries the cache returns. This is bounded by the ~85 cycles the lookup costs in total, so
  it ranks last unless M says otherwise.
- **Not now: a Lua-style overlapping frame** (the callee's registers alias the caller's argument registers, so
  arguments are never copied). It needs a compiler convention and a new frame layout. It earns a spike only if M
  shows the argument copy and the NULL fill are a large share.

**Gates per lever:** the nsl suite (every aux gate), the Go tests, PORTA SYNC OK, `GC_STRESS` on the host, and
the MCU footprint (a lever that grows it says so in its row).

## Questions for David

1. **M before L?** One counting build and one c6spare session, then this doc is revised with the numbers and the
   lever order. Or skip M and take L1 + L2 now, since both are cheap, and measure those?
2. **Access modifiers (dialect row 25):** delete the VM check only (L1), or also the grammar's `public`/
   `protected`/`private` on methods? The keyword still decides slot accessors (private slots get none).
3. **`gen_tag` (still open; David: not understood).** In plain terms: every frame gets a fresh serial number
   when it is pushed. A closure or captured-variable box made in that frame copies the number. The stack slot a
   frame used is reused by the next call, so a closure that outlives its frame could find a *different* frame at
   its home's address; comparing serial numbers is how the VM would notice. The cost is a global load, add and
   store on every call. The question is whether anything still reads the number to refuse a stale home (nsl has
   no non-local return since Milestone A, which was the main reader). If nothing does, it is dead weight. This
   is a code question for me to answer, not a decision for David; I will check before L4.
   **Answered (2026-10-12, L4):** nothing reads it. Every reference is a write (the stamp in `push_frame` and
   `run_interpreter`, the copies in `make_closure` and `OP_MK_CONTEXT`, the image layout and serializer). The
   stale-home guard it was meant to be was never written, so removing it changes no behaviour; that passes the
   Milestone A plan's own test ("only written, removing it is safe"). L4(c) deletes the frame word, both
   `home_generation` fields and the global.
4. **Bench (answered: AWFY first).** The per-lever gate becomes AWFY on c6spare and on the host against Soup,
   with blockcost for the per-mechanism split.

## Addendum, 2026-10-12 (end of session): the AWFY baseline

AWFY is ported (`nodus/tests/00_perf/awfy.nsl`, 13 of 14 rows; Havlak host-only, not yet merged) and
runs on Primordial Soup through `awfy-soup.py`. Host, nsl ÷ Soup: geomean ~1.31×; the worst rows are
the call- and allocation-bound ones (List 4.95×, Storage 2.38×, Permute/Towers ~1.65×); nsl leads on
Mandelbrot, CD, Bounce and Sieve and ties Richards (`AWFY.md`, #37 comment). This is the baseline L1
and L2 are measured against. David (2026-10-12): Havlak host-only; next session commits the results
record and takes L1 + L2; String `copyFrom:to:`, `sin`/`cos` and Float→Integer become VM prims.

The record (2026-10-12, next session): `docs/results/2026-10-12-37-awfy-soup/`, `run_all.sh`, median of
three at `33cb0e16`. Geomean 1.26×; List 4.14×, Storage 1.95×, Json 1.77×, Towers 1.75×, DeltaBlue
1.72×. Soup's runs vary up to ±25 % and nsl's ~10 %, so a lever needs more than one run to count.

## Addendum, 2026-10-12 (later): L1 lands, L2 does not

Measured on the host and on three boards (`docs/results/2026-10-12-37-send-frame/`, optimizations.md Opt #12).
**L1** (the whole access mechanism deleted, David's choice over the run-time path only): AWFY C6 0.936, WeAct 0.961,
host 0.963; STIC C6 0.986, WeAct 0.997; MCU text −754 B. Kept. **L2** (one frame push that answers its method):
host 0.983 when forced inline, but on the boards it loses in both forms (STIC +2 to +3.5 %, WeAct AWFY +2.4 %), because
reshaping `run_interpreter` costs the boards' gcc 12 register allocation across the whole loop. Reverted.

What this changes in the plan: **a lever that touches the loop's body is measured on a board first**. The host
ranks levers that take work out (L1). It does not rank levers that move code around in the loop (L2, Opt #11's
inlined slow path). The board A/B now takes about 15 minutes for four revisions (`board_build.sh`, `board_ab.sh`), so
board-first is cheap. L3 (frame state in locals) is exactly a loop-body change, so it goes to the board first. M (the
counting build) still decides whether the frame or something else is Toit's lead.


## Addendum, 2026-10-12 (M, step 1): the counts

`docs/results/2026-10-12-37-M-counts/` (the `OEVM_COUNT` build, AWFY's board profile counted on the host and paired
with c6spare's L1 times). **A frame costs ~520–730 C6 cycles and is 61–72 % of Richards, Towers, DeltaBlue and List**
(Richards: 1.10 M frames, one every 9.3 instructions, ~640 cycles each, 66 %). That is over this design's one-third
bar, so the send stays the first target and the lever order stands: L3 (board-first), then L4 and L5, which step 2
(the `rdcycle` split) will rank. Two corrections to the plan's picture: registers per frame are only 4–7, so the NULL
fill is small and the overlapping frame stays "not now"; and **the frame is not all of Toit's lead**, since Richards'
instructions alone cost ~2.3 s at the base 36 cycles each, twice Toit's whole run. Dispatch cost (fewer and cheaper
instructions, and where the loop executes from on the C6) is the second front. The implicit send's linear method scan
was found by the counts and is negligible (under 1 %).

**Then, the frameless candidates** (same record, after reading the Toit VM, which runs field-accessor sends without a
frame): **56 % of Richards' frames, 46 % of DeltaBlue's and 64-93 % of CD's, Towers' and List's run a method that is a
single slot access or constant.** That is up to ~2.5 s of Richards' 6.7 s, so a new lever ranks first, ahead of L3:
**L6, frameless accessor sends**, where a getter, setter or constant method is recognised once (at load or cache fill)
and run inside the send with no frame. Board-first, like L3.

## Addendum, 2026-10-12: L6 lands (Opt #13)

`docs/results/2026-10-12-37-l6/`. Frameless accessor sends: WeAct AWFY 0.941 (Richards 0.866), C6 0.988. The gain is
about 40 % of the M upper bound (Richards' accessor frames were priced at the average frame, ~640 cycles; they save
~230 each on the C6), because the accessor's own frame is the cheapest kind and the `send_resolve` call stays. On the C6, rows
that L6 does not touch move 2–8 %, which is code placement in flash. **Next:** the C6 hot path in SRAM (the
`OEVM_HOT_SECTION_IRAM` mechanism the ESP32/S2 use, unset for the C6 in run.sh), which stabilises every later C6
measurement and tests how much of the 36-cycle base instruction is cache misses. Then L3 and L4.

## Addendum, 2026-10-12: L4(c) lands (Opt #15), L3 does not

`docs/results/2026-10-12-37-l4/`. **L4(c)**, `gen_tag` deleted (Q3 above): AWFY C6 0.988, WeAct 0.981; STIC C6 0.995,
WeAct 0.978. **L3** was built only in a reduced form, L3-lite: the frame globals read once into locals in `push_frame`
and the return, `ctx` taken from `push_frame`. The full form was not built, because the write-back it needs at every
call-out falls on nearly every send (`send_resolve` is out of line, and allocation can reach the GC). L3-lite is level
on the C6 and loses 2–4 % on the WeAct, so it is dropped, and L3 with it: the frame state stays in the globals. L4(a)
(`vtable` left unset) and L4(b) (`home_ctx` NULL for a method frame) remain untried: each removes one store per frame
and adds a test where the field is read. The destination register stored at send time trades the return's load and shift for a store
at the send, so it was not tried. **Next: L5**, the `send_resolve` preamble behind the cache.

## Addendum, 2026-10-12: L5 lands (Opt #16)

`docs/results/2026-10-12-37-l5/`. Two parts of the preamble came out: the per-send `main:args:` test (the Kernel
instance is set by the launch sites) and the repeated vtable reads (one, at the top). AWFY C6 0.985, WeAct 0.963. The
capability compares, the `Symbol =` gate and the primitive id load remain ahead of the probe; folding them into
entries the cache returns, as this design proposed, is the rest of L5 and is not started. A finding to keep: the first
cut's +844 B came from `-Os` copying the primitive block for a known receiver vtable, and that copy was itself worth
~1.5 %, which says a primitive test placed after the probe could be worth more than its instructions suggest.
Later the same evening (David): the 844 B were given to the ratchet, and the SMI arm went back to the top: L5 stands at
AWFY C6 0.978, WeAct 0.949.

## Addendum, 2026-10-12 (night): the rest of L5 (Opt #18)

`docs/results/2026-10-12-37-l5-rest/`. Not by cache entries, as proposed above: each remaining test went where its
case already lives. `Symbol =` into the `=` prim's case; the capabilities to the lookup miss (their vtables are
empty); the selector's Symbol-ness proved at load, leaving one `prim_id` load ahead of the probe. AWFY C6 0.918, WeAct
0.926 against Opt #16; the selector check was the large part, because `obj_vtable` on an image literal is a blob test
and an `oimg_resolve`, not a load. The ~85-cycle bound this design put on L5 was the lookup's; the preamble ahead of
it was not inside that bound.

## Addendum, 2026-10-12 (night): L4(b) lands, L4(a) does not; the ladder closes for 0.1

`docs/results/2026-10-12-37-l4b/`. **L4(b)** (Opt #19): a method frame stores its closure's NULL home and `homeOf:`
answers the frame itself; speed level (the geomean's 0.992 on the C6 is placement: Richards 1.007, arrayWrite 0.928),
kept for one encode site fewer. **L4(a)** is not built: a frame `vtable` left unset can carry a stale
`MethodContextVtable`, which `primitive_restart_frame` reads as "stopped". The destination register stored at send
time stays untried. With L1, L4(c), L5 and L6 kept and L2, L3 dropped, the ladder is done; David closed #37 for 0.1 as
"good enough", no hacks for benchmarks. The fronts left (dispatch, the allocator, per-site caches) are listed with
their evidence at the foot of the ledger in `nodus/oevm/docs/optimizations.md`.
