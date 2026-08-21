# Primitive classification belongs to the Symbol, not to a cache

**Status:** built 2026-08-20, host-green and **target-green on the C6 2026-08-21** (§7).
**Issues:** rete#226 (this), closing out #222 = #192 = #213 (one bug).
**Branch:** `226-prim-id-on-symbol`.

## 1. What this replaces, and why it was never a cache problem

`selector_prim_id()` in `nodus/tuvm/src/dispatch.c` answered "does this selector name an
intercepted primitive, and which one" for **every send whose selector is a Symbol** —
`send_resolve` is `OEVM_HOT` and the classification ran ahead of vtable lookup. It answered
by strcmp against 154 names, memoised in a pointer-keyed open-addressed table.

The table never evicted, never grew, had no load-factor check and — until #222 — no probe
bound. Full table meant no NULL to stop at, and the next uncached selector walked it
forever: in C, below the interpreter, at 100% CPU, with no fault, no panic and no console
line, while the pulse mailbox filled behind it.

**`PRIM_CACHE_SIZE` was 256 on the C6 and 4096 on the host.** That single number is why
three separate deaths (#192 at tick 2-3, #213 at tick 6, #222 at tick 6) all read as
"target-only, mechanism unknown" for six weeks: no host suite has ever filled 4096, and a
real node filled 256 in seventeen seconds.

#222 bounded the probe. That stopped the hang and left the node permanently degraded —
every miss paying a linear scan of all 154 names and evicting a home slot, which two hot
selectors can thrash against each other indefinitely.

**The decision recorded here is that a cache was the wrong shape from the start.** Whether a
selector names a primitive is a pure function of its NAME, fixed for the life of the Symbol,
and a Symbol is created exactly once. So the answer is decided there and read as a field.

## 2. Three routes were considered

| Route | Why not / why |
|---|---|
| **Bytecode-side id** — emit the id beside each selector constant, pass it to `send_resolve` | Makes prim-ness a property of the CALL SITE. Every other route to a selector (`perform:`, `super`, the in-image debugger, a wire-decoded symbol) would still need the name scan, so it adds a fast path in front of the mechanism instead of deleting it. Also needs the name list inside the Python builder AND a per-constant table emitted by both image builders |
| **Vtable-first, prims on the miss path** | Deletes the cache with no ABI change, but inverts precedence: today a prim name beats a vtable method (`size` is the recorded collision). Safe only with a build- or boot-time collision gate, and the blast radius is the whole dispatch order |
| **A field on `struct Symbol`** — CHOSEN | Prim-ness is a property of the selector, so this is true for every route that can reach it, including symbols minted off the wire. One emitter per image model, one list, no second lookup anywhere |

## 3. It costs nothing in space, and that is what makes the ABI change safe to consider

`length` was a `size_t` holding a value under 256. Narrowing it to `uint16_t` pays for
`prim_id` with **the struct size and every offset unchanged** on a 32-bit target: no image
grows, and the T0.3 footprint ratchet — 75% spent — absorbs nothing.

The padding is load-bearing rather than tidy. `oimg_serialize.py` requires an inline payload
to begin exactly at `sizeof`, and a flexible array member does not round `sizeof` up, so a
64-bit host needs an explicit second pad word to keep `offsetof(name) == sizeof`.

## 4. The list moved, because baking ids needs it in two languages

Baking at image build means the Python builders need the name-to-id map. Restating 154 names
in Python would be a second copy of a list whose C side already carries a compile-time parity
guard against the enum — precisely because a drifted list misroutes every primitive after
the insertion point, silently, and only on the side that drifted.

So the names live in **`nodus/tuvm/src/prim_names.def`**, one `PRIM_NAME("...")` per line.
`dispatch.c` builds `PRIM_NAMES` from it by `#include`; `nsl/tools/prim_ids.py` reads it;
`solidify.py` (Model A) and `oimg_serialize.py` (Model B) bake from that. A missing `.def`
is an ERROR, not an empty map: baking zeros would yield an image with no primitives
intercepted at all.

**The order IS the id, so the file is append-only.** A prim id is a number baked into every
image built before the edit.

## 5. One constructor, because an invariant beats six reminders

Six sites open-coded allocate / set length / copy / NUL-terminate. They are now one
`symbol_new()`, which classifies. That is what makes "a Symbol knows its prim id" an
invariant rather than six things to remember — and the value of that was demonstrated
immediately: `tests/test_debugger.c` had a seventh, private copy in its own helper, so
`readLine` classified as not-a-primitive and fell through to `doesNotUnderstand` on the very
next suite run. Four more test helpers had the same shape.

**The symptom of an unclassified Symbol is a primitive quietly ceasing to be intercepted**,
not a crash at the point of the mistake. That is recorded in the code where the next person
is most likely to open-code one.

## 6. A stale layout is refused BY NAME

Narrowing in place is what keeps images the same size, and it is exactly why the existing
`offset == size` guard cannot see this change. A layout captured before `prim_id` would bake
no ids and write `length` a full word wide over the field — an image that loads, passes
`format_version`, and runs with every primitive unintercepted.

So `oimg_serialize.py` refuses a layout whose `Symbol` has no `prim_id`, names the field,
and says how to regenerate it. Tested against a synthetic stale layout, and confirmed
against the real `layout-esp32c6.json`.

This is the second time a captured-ABI artifact has gone stale under a struct change
(rete#190 stage 2 was the first, and cost a garbled selector on target). The general fix —
a VM-revision marker in the layout file, so the refusal names the revision gap rather than
one missing field — is proposed in **#227**, alongside the Xtensa layout that cannot be
regenerated at all.

## 7. What is verified, and what is not

**Host:** nsl suite 262/0 exit 0 with Path A and Path B green, every aux gate PASS,
`PORTA SYNC OK`, `tests/test_symbol_prim_id.c` (the id is decided at creation; ten thousand
distinct selectors are a non-event), and a serializer test that the refusal fires by name.

**Target: VERIFIED 2026-08-21 on c6spare.** `zephyr/layoutdump/` was flashed and its boot
JSON read out of the console collector's log (never a second handle on the port). The
regenerated `layout-esp32c6.json` differs from the 2026-07-20 capture by **five lines** —
`Symbol.prim_id` at offset 6 — with every struct size and every other offset byte-identical,
which is §3's prediction meeting silicon. The same log holds two independent captures
(esptool's reset and the driven one) and they compare equal.

Then `tests/target/run.sh bridge-health` — the #222 reproducer, same scenario and same
board — **PASS 3/3, observed 180 s past the verdict still progressing, no fault, 206 s**.
Two mornings earlier that gate's node died at tick 6 on eight consecutive boots.

**A caveat about how this was read, because it matters more than the green.** The absence of
`prim cache full` proves nothing here: #226 deleted the line that used to print it, so this
gate could not have shown it either way. What the run does establish is positive — the node
ran 206 s, twelve times past the ~17 s where the table used to fill, with `misses 0` and
`dropped 0` — and an image baked with no ids would not have booted at all, every primitive
having ceased to be intercepted.

**One defect surfaced on the way, and only a board could have found it.** `3ddda63` deleted
both `PRIM_CACHE_SIZE` blocks from `zephyr/CMakeLists.txt` and left their `endif()`s behind,
so the file stopped parsing (`Flow control statements are not properly nested`) — over a tree
that was simultaneously 262/0 with every aux gate green and `PORTA SYNC OK`, because nothing
on the host reads that file. Fixed in `208f28d`.

## 8. Promotion

- `review/program-sequence.md` — the rung (David's tree).
- This file is the historical "why"; the durable statement of the mechanism is the comment
  block above `classify_selector_name` in `dispatch.c` and the `struct Symbol` comment in
  `oevm.h`.
