# Allocation attribution — the profiler's question, asked about bytes

**Date:** 2026-08-12
**Status:** design, opened. D1–D9 taken; stage 2's sizing is deliberately left to a
measurement stage 1 exists to produce.
**Refs:** #190, #199, #152, #151, #164, #165, #166, #173, #53, #163

---

## 1. The question this answers

David, 2026-08-12: *"what tooling/probes enable us to find A? it is not a profiler or a
debugger? is it an allocator watcher? what invocation (which we already know) is
**allocating** ... another bucket counter, like profiling?"*

**Yes — and it is the same bucket counter, keyed on the same thing.** The per-method
profiler (#164/#165) answers *which method is invoking*. This answers *which method is
allocating*, on the same key, in the same table shape, resolved through the same sidecar.
It is not a new instrument class. It is the second question you can ask of a structure that
already exists.

## 2. What "cluster A" actually is, and which part this covers

Cluster A is the group of failures where **the node dies while every instrument reports
healthy**. It is not one problem. Enumerated honestly, because scoping this design to the
part it covers is more useful than a claim that it covers the cluster:

| failure | what the node reported | what would have caught it |
|---|---|---|
| **#190** — `asSymbol` mallocs per decoded wire Symbol | `sys/cheap` showed outstanding blocks climbing: a leak exists, **not who** | **this design** |
| **#199** — arena OOM in 2 m 37 s under gateway rejection | `tick N misses 0` — healthy to the panic | a counter on the `err` branch; nothing here |
| **#199's mechanism** — fragmentation, not exhaustion (`3c8c8d1`) | `used 127680 of 131072` | a size-class histogram; nothing here |
| **#152** — dead vs quiet | nothing, by construction | off-node stall detection; nothing here |

**This design covers row 1 only.** The through-line of rows 2 and 3 is that #199 and #151
both died on *"no counter on that branch"* — a discard path that logs nothing — which is a
rule and a sweep, not a tool. Row 4 is porta-side. All three are worth doing and none of
them is this.

**The aggregate gauge and the attribution table answer different halves of one question.**
`sys/cheap` (#190) already says *a leak exists*: `alc`, `fre`, `outHigh`, `outBytes`,
`fails`. It cannot say *who*, and that is the entire remaining gap for row 1.

## 3. The mechanism, and why it costs almost nothing

### D1 — The key already exists, and it needs no new field

`push_frame` (`nodus/tuvm/src/vm.c:623`) loads
`struct CompiledMethod *m = closure_entry(cl)` for `num_registers`, and #164's hook keys on
exactly that pointer:

```c
#ifdef OEVM_METHOD_PROFILE
    mprof_bump((const void *)m);
#endif
```

At **any** allocation site, `current_frame->closure` is live — offset 4 of
`struct MethodContext` (`oevm.h:144`, *"The compiled closure being executed"*) — so
`closure_entry(current_frame->closure)` yields **the identical pointer**.

This matters more than it looks: `MethodContext`'s layout is **pinned** (the 32/64-bit
`gen_tag` union at offset 5 exists precisely to hold the offsets identical across word
sizes). Deriving the key rather than storing it is what keeps this design out of that
struct.

### D2 — Cumulative, not outstanding

The table accumulates **bytes allocated** and **allocation count** per method. It does
**not** track outstanding bytes (alloc − free attributed to the allocator).

Rejected because the price is asymmetric and one half is unaffordable:

- **C heap** — `union CHeapBlock` (`gc.c:75`) is already a header, so an owner key is one
  more word per block. Tractable.
- **Arena** — a "free" is a GC sweep, not a call. Attribution would need a per-object owner
  word, changing object layout across the whole arena.

**What cumulative cannot do, stated plainly:** a method that allocates 10 MB and frees all
of it is indistinguishable from one that leaks 10 KB. This is a real limitation and not a
detail. It is acceptable **because the aggregate gauge already supplies the missing bit** —
`sys/cheap` says a leak exists, and the table then says who is churning at the top of the
list. For #190 that composition is sufficient: `asSymbol` would have ranked first on the
C-heap column while `outBytes` climbed.

If a future failure defeats that composition, outstanding-on-the-C-heap-only is the
escalation, and D2 should be revisited rather than worked around.

### D3 — Both heaps

Two independent column pairs, because they fail differently and a single number would hide
which one moved:

| column pair | bumped at |
|---|---|
| `cheapBytes` / `cheapCount` | the C-heap seam, `gc.c:81` |
| `arenaBytes` / `arenaCount` | `gc_allocate`, `gc.c:290` |

### D4 — The cost projects to near-free, and the projection is the thing stage 1 checks

Allocations are far rarer than activations. Two measurements already on record:

- #197's gate: `c6-bench` did **+140 132 C-heap allocs over 607 check-ins** ≈ **231 per
  check-in**.
- #163: a single 425 B frame encode costs **9 624 activations**.

So the allocation path runs roughly **two orders of magnitude** less often than
`push_frame`. If #164's hook costs 0.90%, the same hook on the allocation path projects to
**~0.02%**.

**This is a projection, not a measurement, and it covers the C heap only** —
`gc_allocate`'s call frequency is a number nobody has. Stage 1 produces both.

## 4. The two constraints that shape stage 2

### D5 — "Always on in every build" cannot mean every target

`vm.c:383` already states the ceiling: *"(#53) and this table is 2 KB."* The mprof table is
`OEVM_MPROF_SLOTS 256` × 8 B = **2 KB**, and it is flag-gated partly *because* of #53 — the
WROOM-32's dram0 sits at **99.58%, 816 B spare**.

So: always-on where headroom exists (C6 at 44% SRAM spare, nRF52840), compiled out where it
does not (WROOM-32, MagTag S2).

**There is no target-capability logic to hang that on, and this design does not invent
one.** `zephyr/CMakeLists.txt:260` gates mprof on an environment variable —
`OEVM_METHOD_PROFILE=1`, OFF by default, with the #53 reasoning written into the comment
above it. Selection is manual today and is one of the port's several env knobs.

The symmetric answer is therefore the same shape with the **default inverted**: an env knob
in the same block, **ON unless explicitly disabled**, so a target that cannot afford it opts
out where its other build settings already live. That adds no new mechanism — it reuses the
one pattern this build already has for exactly this decision — while making the affordable
case the default, which is the whole point of D8.

Deriving the default from a board signal instead would be genuinely better and is genuinely
new logic. Not now: it is a second decision riding on a first, and #53's target list is four
boards long.

### D6 — Overflow is not hypothetical for an always-on table

#165 measured `methods=224 overflow=0` against 256 slots — load factor **0.875** — and
`vm.c:389` warns that above ~0.7 the linear probe degrades. **That was a 10-second window.**
An always-on table accumulates the union of every path taken over hours, so it will see more
distinct methods than any window did.

The image holds **740 `CompiledMethod`s** (`grep -c "^static const struct CompiledMethod
Method_.*_compiled;" src/solidified_image.c`). The key space is therefore **bounded at build
time**, which is the property that makes a no-overflow table *possible* — a 1024-slot table
cannot overflow. Whether it is *affordable* is the open question, and it is what forces the
staging below.

**Overflow must stay counted, never absorbed.** `vm.c:390` already gives the reason: a full
table must not read as "this method never allocated". That failure mode is precisely the
cluster-A shape this design exists to attack, and it would be self-inflicted.

## 5. The two stages

### D7 — Stage 1 is a spike inside the existing table

Add **one column pair per heap** to the *existing*, flag-gated mprof table. No new table, no
RAM decision, no always-on cost, no change to `dbg/req` or to sidecar resolution.

Run it on `c6spare` under steady-state bridging on the real mesh and read off the two
numbers nobody has:

1. **How many distinct methods allocate** — the number that sizes stage 2's table.
2. **How bytes distribute across them** — whether the top of the list is a handful of
   methods (a small table is right) or a long tail (a different design).

And two secondary numbers that fall out: `gc_allocate`'s real call frequency, and the
measured cost of the hook against D4's ~0.02% projection.

**Stage 1 is independently useful.** It names `asSymbol`-class leaks the moment a window can
be run — #190's diagnosis without the detective work.

**What stage 1 explicitly does not do:** it needs a profile build and a reflash, so it gives
nothing on a node that has already died unreproducibly. It buys the number that makes stage
2 correct instead of guessed.

### D8 — Stage 2 is the always-on table, sized by stage 1

A **separate** table from mprof. Separate rather than shared for three reasons, all of which
only became visible once D5/D6 were settled:

1. **Different gating.** Activations stay behind `OEVM_METHOD_PROFILE`; bytes are always-on.
   Sharing one table means a production build carries the key array and byte columns but no
   activation counter.
2. **Different key populations.** 740 methods exist; only some subset ever allocates. Sizing
   one table for the activation population spends byte-counter RAM on rows that read zero
   forever.
3. **The always-on half must be small** to survive D5's ceiling at all.

Sizing is **deliberately not decided here**. If stage 1 says ~40 allocating methods, a
64-slot table costs well under 1 KB and #166's displacement question never arises for this
table. If it says ~400, that is a different design, and the point of the staging is to know
which one before committing.

### D9 — It leaves over the route that already exists

Rows leave over #165's `dbg/req` → console tee: **not** through the space, **not** over
`dbg/resp`. In-blob methods report as blob-relative offsets (`key - oimg_blob_base`,
`vm.c:499`), which the `.oimg.methods.json` sidecar already resolves, and #74 closed the
matching hazard — `platform buildId` names the running build, so the sidecar cannot be
picked wrong.

If stage 1's columns follow mprof's mirror-confined, selector-named pattern, **no prim id is
consumed**.

## 6. Gates

- `cd nodus/nsl && python3 tools/run_tests_oevm.py` — 258 passed, 0 failed, **and the aux
  gate line read**, not the count alone (#63).
- `cd nodus/tuvm && bash tests/porta_sync_host.sh` — `PORTA SYNC OK`.
- A **target** gate under `nodus/tuvm/tests/target/`, because the host cannot witness this:
  the arena and C-heap pressure that make attribution interesting are target-side, and
  #56's harness exists for exactly this shape. A host-only gate would be a green that
  tested nothing (see `hardware-gate-false-greens`).
- The stage-1 measurement is written to `docs/results/` as a dated result, in the pattern of
  `2026-08-09-cbor-encode-shape.md`, because stage 2's sizing cites it.

## 7. What this does not decide

- **The other three rows of §2.** The uncounted-branch rule (#199/#151), the size-class
  histogram, and off-node stall detection (#152) are each separate work. None is blocked by
  this and this is not blocked by them.
- **Stage 2's table size**, by construction — D8.
- **Whether outstanding attribution is ever needed** — D2's escalation path, revisited only
  if a real failure defeats the gauge-plus-table composition.
- **Displacement / Space-Saving (#166) and per-method scoping (#173).** Both are open
  against mprof's table. If stage 1 reports a small allocating population, stage 2's table
  sidesteps them; it does not resolve them for mprof.
- **Anything about the arena's *shape*.** Attribution says who allocated, never whether the
  arena is fragmented. #199's OOM was fragmentation (`3c8c8d1`), and this design would not
  have explained it — see `tuvm-vm-memory-model`: read `largest_free_block`, and a repeating
  fatal size is a constant.

## 8. Risks

- **The projection in D4 is one number, unmeasured, and covering one heap.** If
  `gc_allocate` turns out to run at activation-like frequency, always-on becomes a real cost
  and D5's target split may not be enough. Stage 1 is what falsifies this before stage 2
  commits.
- **Cumulative attribution can rank the wrong method.** A hot allocator that frees
  everything outranks a cold one that leaks. The gauge supplies the missing bit, but a
  reader who consults the table alone will be misled — the report should say so on its face,
  not only here.
- **`current_frame` at an allocation site may not be what the reader expects.** Allocations
  made during boot, inside prims, or from the interpreter's own paths (`vm.c:1228`,
  `vm.c:1484`) attribute to whatever frame is current, which may be a caller rather than the
  apparent allocator — and allocations with **no** current frame need a defined bucket
  rather than a crash. Stage 1 must report that bucket's size; if it dominates, the
  attribution is not yet answering the question.

---

## 9. ADDENDUM (2026-08-12, same day) — what stage 1 measured, and what it refuted

Stage 1 is built and gated (`814b99e`, `49c08c6`, `4fa7dd5`). Results:
`docs/results/2026-08-12-allocation-attribution-host.md` and
`docs/results/2026-08-12-allocation-attribution-c6.md`. Stage 2 is **#203**.
**Stage 1 is NOT complete** — §9.3.

### 9.1 D8's sizing input, and D7 vindicated

| window | distinct allocating methods | overflow |
|---|---|---|
| host | **64** of 190 active | 0 |
| C6 boot | **80** of 223 active | 0 |

**The host figure was not the bound**, which is the whole argument for D7's staging:
sizing stage 2 from the cheap measurement alone would have been 25% low. 80 of 740
image methods still lands where D8 hoped — a 128-slot table, not a 1024-slot one.

### 9.2 D4 is REFUTED as stated, and must not be carried forward

D4 projected ~0.02% from "allocations are roughly two orders of magnitude rarer
than activations". Measured arena allocations per activation:

| window | ratio |
|---|---|
| host | 1 per **10 300** |
| C6 boot | 1 per **5.6** |

Three orders of magnitude apart. D4's *reasoning* was sound for the host window
and simply does not generalise; the projection it produced is not a number anyone
may quote. **No cost figure exists.** #165's `OEVM_MPROF_RUNTIME_GATE` OFF/ON
machinery is the way to get one, and #203 carries it.

D4 said "this is a projection, not a measurement" and named stage 1 as the thing
that would settle it. It did — against the projection. That is the deferral
working, not failing.

### 9.3 What §6's gates could not see, and the one that mattered

The suite gates all passed — 260/0, aux green, `mprof-tools-py` 12→19 — and the
target run still could not complete, because the node wedged. §6's instinct that a
**target** gate was required was right; what it did not anticipate is that the
blocking failure would be in neither the host nor the binary.

**The cause was #180: a `porta put` wrote `dbg/req` as a CELL onto a path
established as a STREAM.** Four builds hung identically at the same tick,
including one containing no profiling code at all, which is what put the cause in
the store. Table capacity was the leading hypothesis and a 1024-slot build refuted
it, clearing **#166** of any part in this.

Two things this design should carry forward:

- **A data-side cause cannot be found by a binary-side bisect.** Three A/B builds
  were spent before the store was even a candidate. The decisive test was building
  with the feature *absent* — see `hardware-gate-false-greens`.
- **`OEVM_MPROF_SLOTS` is now build-settable** (`04df05c`), added for the capacity
  experiment and kept because 256 is genuinely tight for this app (223 active).

### 9.4 An unanticipated finding that outranks the sizing number

Only **two** methods allocate on the C heap, on host and on target alike:
`Kernel>>valueCompare:with:` and `Kernel_Cbor>>encodeSymbol:into:`, both on the
map-key sort path. The C heap on a bridging node is scratch for one sort.

That is the capability §2 promised — `sys/cheap` could say a leak existed, never
who — demonstrated on the first real run, and it opened a question the design
never asked: **why are the keys sorted at all?** Filed as **#204**.
