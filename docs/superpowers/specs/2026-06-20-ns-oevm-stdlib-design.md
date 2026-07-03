# ns-oevm — standard library: the capability-pure primitive/library line

**Date:** 2026-06-20
**Status:** design — **§1–§3 ratified** (morning review 2026-06-20 complete):
architecture, primitive line, class scope, testing, and sequencing all decided;
GC feasibility verified. Ready for writing-plans (step 1).
**Repo note:** this repo (`ns-oevm`) holds the compiler/grammar/docs; the VM is a
**sibling repo at `../oevm`** (`../oevm/src/{dispatch,vm,gc,kernel_boot}.c`,
`oevm.h`). All `oevm/...` paths below are relative to that sibling.
**Depends on:** conformance suite green (47/47 on both image paths); source-map
emission done (`.stmap`, merged).
**Supersedes/extends:** `docs/superpowers/specs/2026-06-19-next-phase-stdlib-and-debug.md`
(item 1).

---

## 1. Context — where we are

ns-oevm has **no Newspeak-level standard library.** The "std lib" today is ~50 C
primitives in `oevm/src/dispatch.c`, dispatched by a linear `strcmp` scan over a
frozen `PRIM_NAMES[]` table keyed on the interned selector symbol
(`dispatch.c:507–537`, dispatch body `:579–1045`). Notably:

- **`Array` is a *mock class object*** synthesized in C by `PRIM_Array`
  (`dispatch.c:580–656`); `at:`, `at:put:`, `size`, `do:` are C primitives
  operating on that mock. There is no real `Object`, `Collection`, or class
  hierarchy in the image.
- The compiler lowers any unresolved capitalised name to `OP_SEND_IMPLICIT`
  (`ns_compile_oevm.py:370`, `resolve_name` → `("implicit_send", name)`), which
  the VM catches by selector string. The "injected platform namespace" the
  st-oevm template (§7) reserves as the seam exists **only as a reserved
  resolution rule** — there is no actual namespace object behind it.
- `oevm/src/builtins.def` confirms the minimal firmware surface: core vtables
  (Integer, Float, Symbol, closures, fibers, LargeInteger) + a few kernel
  symbols. **No Array/String/Object vtable.**

### Motivation (decided)

Of the candidate motivations, the driver is **A — a principled primitive/library
line.** Get off the C-primitive-mock model onto a clean line: keep the genuinely
VM-level primitives minimal, and write *everything expressible in Newspeak in
`.ns` source*, compiled into the image.

### Scope calibration (decided)

Calibrate breadth to **st-zephyr** — this is an *embedded scripting language on
Zephyr*, not a Newspeak research vehicle. Streams matter (reading from serial
ports). We subset SOMns library *bodies* where clean; we do **not** port SOMns
examples or aim for its full typed core-lib.

---

## 2. The decision: primitives are messages to a capability object

Three models were considered for marking the primitive line:

1. **`_`-prefixed primitive selectors** (st-zephyr-looking).
2. **`<primitive: N>` method pragmas** (classic Smalltalk).
3. **Primitives as messages to a capability object** (SOMns `vmMirror` /
   st-zephyr `_Gpio` receiver objects).

**Chosen: 3, fully capability-pure.** Rationale, grounded in the sibling code:

- **Safety.** `_`-prefix and `<primitive:N>` are both "security by please-don't"
  — a user can type either. A primitive reachable only as a method on an object
  you were *handed* cannot be grabbed by a module that was never given it. You
  can't call what you weren't passed.
- **One mechanism for core + native libraries.** st-zephyr's `_Gpio setPort:…`
  and `_Thread onChildAttached:…` are not `_`-prefixed *selectors* — `_Gpio`/
  `_Thread` are **primitive receiver objects** you send ordinary messages to
  (`st-berry/samples/lib/std.st:40,134`). Core primitives and native Zephyr
  modules are then the *same* mechanism: a message to a capability.
- **Maximises SOMns reuse.** SOMns has **no `<primitive:>` pragmas**; its
  primitive line is a capability object `vmMirror` passed into each module —
  `class Kernel vmMirror: vmMirror = …` then `vmMirror printNewline:`,
  `vmMirror object: self identicalTo: other`, `vmMirror objHashcode: self`
  (`SOMns/core-lib/Kernel.ns:21–80`). Adopting the capability model makes
  SOMns's `Streams.ns` (1184 ll) / `Collections.ns` (344 ll) bodies
  largely copy-and-subset; inventing prefixes/pragmas would force a rewrite of
  every `vmMirror …` send.

The user's framing: *"why build NS if we hack the capability model at its
core?"* Going capability-pure is less long-term technical debt and realises the
seam that lets ST and NS eventually share one VM image.

### 2.1 Three capability tiers (RATIFIED 2026-06-20)

Capabilities are not one undifferentiated object — they split into **three tiers
by who is handed what**, each a distinct seam:

| Capability | Power | Handed to | Axis |
|---|---|---|---|
| **`platform`** | allocation, indexed access, identity, `print:` | every module | ambient library primitives |
| **`mirror`** | inspect stack, read/write frame regs, `classOf:`, hot-swap | **only the debug subsystem** | privileged reflection |
| **`Serial`/`Gpio`/`Timer`/…** | hardware | modules that need them | per-target portability |

This split was driven by a stated requirement — **remote debug on the embedded
targets** — plus capability-purity consistency. Rationale per tier:

- **`platform` (ambient library).** Allocation/indexed/IO/identity — every module
  needs to make objects, so this is handed broadly. This is the SOMns `vmMirror`
  *primitive* role, minus the module-graph machinery (which we drop).
- **`mirror` (privileged reflection) — the kept half of SOMns's split.** We do
  **not** collapse reflection into `platform`. The on-device debug hook is
  **image-level code** (`../oevm/src/vm.c:170–185` invokes `debugger_hook` as a
  *Closure* via `run_interpreter` on breakpoint), so it must introspect the
  paused program. Under the capability-purity already ratified, image code reaches
  primitives only through handed capabilities — so the reflective/debug primitives
  **must** be a capability object (making them ambient C the hook just "calls"
  would be the very "hacking the capability model at its core" the user rejected).
  Keeping that capability *separate* from `platform` is **confinement**: an
  embedded script you run gets `platform` and cannot introspect or mutate the live
  runtime; only the debug subsystem is handed `mirror`. This is the Bracha/Ungar
  mirror principle, justified by a practical embedded need rather than research
  completeness. **De-risk:** the reflective C primitives already exist
  (`oevm.h:279–296`) — the mirror tier is largely *binding existing prims onto a
  capability vtable*, not inventing them.
- **Native modules (per-target portability).** `Serial`/`Gpio`/`Timer`/`Thread`:
  one target-agnostic Newspeak protocol, a thin per-target C backend selected at
  firmware-build time (host/POSIX, Zephyr UART on nRF52840, ESP-IDF UART on ESP32).
  **This — not the `vmMirror`/`platform` split — is the hardware-portability seam.**
  Caveat: `../oevm/src` is **host-only today**; these backends are the *model*
  inherited from st-berry/st-zephyr, to be built as targets land.

**The mechanism is the same for all three** — send a message to an object you were
handed. Only the grant policy differs.

---

## 3. §1 Architecture — the capability spine (RATIFIED)

**The platform capability object.** At boot the VM constructs one real object,
`platform`. Its vtable's methods *are* the genuinely-VM-level primitives,
implemented in C. (SOMns splits a raw `vmMirror` from a module-graph `platform`;
for our minimal embedded scope we collapse to a single `platform`, revisiting the
split only if it earns its keep.) This object is the **only** way to reach a
primitive.

**Injection & threading.** The injection seam **already exists** — the entry path
constructs the root module and runs it with a `platform` argument, today stubbed
as a fake SMI:

```c
// ns_compile_oevm.py:1841–1852  (mirrors run_solidified_entry_point, oevm_main.c)
oop fake_platform = TAG_SMI(0);
oop init_args[2] = { fake_platform, fake_args };
send(entry_inst, init_<ctor>:, 2, init_args);
send(entry_inst, "main:args:", 2, main_args);
```

Going capability-pure means `kernel_boot`/`run_solidified_entry_point` constructs
a **real** `platform` object and passes it where `fake_platform` is now. The root
module becomes `class Main usingPlatform: platform = (…)` and threads the
capability down to sub-modules exactly as SOMns's `Platform.ns` does
(`Collections usingPlatform: platform`, `Streams usingPlatform: platform`). No
module can touch a primitive it was not handed — capability-pure by construction.

**Grammar already supports the header.** `ns.ohm` has
`ClassDef = visibility? classKw identifier CtorSig "=" SuperClause? ClassBody …`
with `CtorSig = (keyword BinarySend)+`, so `class Kernel usingPlatform: platform =
Super (slots)(members)` parses today. (`<:` Value/type annotations are **not**
supported and are subset away — fine, we don't want them.)

**The primitive/library line becomes physical:**

- **C side (shrinks):** the `platform` vtable's primitive methods + native-module
  capabilities (`Gpio`, `Serial`, `Timer`, `Thread`), all the same mechanism. The
  `PRIM_NAMES[]` string-interception of *public* selectors in `dispatch.c` goes
  away; public selectors stop being shadowed.
- **`.ns` source side (grows):** `Object`, `Array`, `Collection`, `Stream`, …
  real Newspeak classes calling the capability. Subset from SOMns, swapping the
  `vmMirror` receiver for our `platform`.

**The one substantive compiler change.** Today an unresolved capitalised name
lowers to `OP_SEND_IMPLICIT` caught by C string-match. In the capability model it
must resolve through the lexically-threaded `platform` / module-namespace slot —
real name resolution against the injected namespace, not a VM string-match. The
grammar header form already parses; this resolution rewrite is the core work.

**Net:** `Array`/`do:`/`at:` become real source methods; the VM's job shrinks to
the irreducible kernel + native modules; the st-oevm injected-platform seam is
*realised* rather than reserved.

### 3.1 Per-target library-variant selection (RATIFIED 2026-06-20)

Because capitalised names resolve **through the handed `platform` namespace** (the
core resolution change above), the *same* injection seam doubles as a per-target
**implementation** selector. Boot each target with a `platform` whose
`OrderedCollection`/`Dictionary`/`Array` bindings point at **different
implementation classes that share one protocol** — e.g. an aggressively-growable
`OrderedCollection` on Linux vs a bounded/preallocated one on Zephyr (no realloc
churn → no fragmentation; see §3.2). The application `.ns` source is **byte-identical
across targets**; only the booted `platform` differs.

Bounds on the technique:
- Selection is at **image-build / boot time**, not runtime.
- Variants must **share protocol** (same selectors) to stay source-compatible.
- Only classes reached **by name through the namespace** are swappable; the
  primitive indexable *storage* underneath and compiler-inlined control flow are
  shared, not swapped. You vary the *strategy class*, not the VM primitive.

**One seam, three wins:** the injected `platform`/`mirror`/native trio gives
capability confinement (§2.1), hardware portability (§2.1 native tier), *and* this
per-target library-policy selection — all from "hand a different object per target."

### 3.2 GC feasibility (VERIFIED 2026-06-20 — no VM-GC changes required)

Checked against `../oevm/src/gc.c` (mark-and-sweep, non-moving, segregated
free-bins + bump, fixed arena `OEVM_HEAP_SIZE`, `-D` override for embedded):

- **Variable-length indexable objects are already handled.** `trace_object`
  (`gc.c:366`) scans an object's payload word-by-word over `header->size`, so a
  large indexable traces like any block. Store the length as a **tagged SMI** so
  the mark scan skips it (`mark()` early-rejects `IS_SMI`), or add a vtable-cased
  branch in `trace_one` (as `Vtable`/`Fiber` already do). No allocator change.
- **Growable collections** = allocate-bigger-backing + copy + drop-old; the old
  backing is reclaimed by the normal sweep. Standard, no GC change.
- **Watch-item (gate, not blocker):** non-moving + non-coalescing bins
  ("tolerates fragmentation for O(1) alloc", `gc.c:132`). Heavy *unbounded* growth
  on a small embedded arena can fragment — mitigated precisely by the §3.1
  per-target bounded-collection variant on Zephyr.
- **Boot wiring:** the `platform`/`mirror` objects and every library-class vtable
  must be GC roots — mark them exactly as `debugger_hook` is marked
  (`gc.c:626–629`), via the static root list or `gc_additional_roots_hook`.

---

## 4. §2 The primitive/library line — content (RATIFIED 2026-06-20)

A clean split across the three tiers of §2.1:

**(a) Receiver-intrinsic primitives — stay as vtable primitives, NOT capabilities.**
These are an object's own intrinsic behaviour, not "globals," and many are
already compiler-inlined. Keep on Integer/Float/Block/Symbol/MethodContext:
arithmetic (`+ - * /`, already `OP_ADD…` fast paths), comparison (`< > <= >= =`),
`==` identity fast path, block `value`/`value:`/`createClosure:`/`home`/`regAt:`,
the already-inlined control flow (`ifTrue:`, `whileTrue:`, `to:do:`, `and:`/`or:`
with literal blocks — `ns_compile_oevm.py` Phase B–D). Unchanged.

**Booleans (DECIDED): stay intrinsic + gain three Object-protocol prims.** Keep
`true`/`false` as primitive singletons with the existing `ifTrue:`/`and:`/`or:`
inlining (the hot path — untouched). Add only `hash`, `=`, and `printString` as
primitive methods on the boolean tag so booleans satisfy `Object` protocol where
they must behave as ordinary objects — printed in a collection, or used as
**Dictionary keys** (newly live, see §5). Full `True`/`False`/`Boolean` *classes*
are rejected: they buy only reflective uniformity at the cost of metaclass
plumbing an embedded scripting language doesn't need. Revisit only if reflective
uniformity becomes a real requirement.

**(b) `platform` capability primitives — the ambient-library tier (§2.1).** Replace
the global-name string interceptions and the allocation/indexed/identity/IO surface:

- Allocation: `basicNew:` (fixed-slot instance), indexable allocation
  `array:`/`basicNew:indexable:`.
- Raw indexed access: `basicAt:`, `basicAt:put:`, `basicSize`.
  *(Key VM work: a **real indexable object representation** must replace the
  `PRIM_Array` mock so a `.ns` `Array` can be backed by VM storage. Store the
  length as a tagged SMI — see §3.2 — so no GC change is needed.)*
- Identity (minimal): `object:identicalTo:`, `objClassName:`, `objHashcode:`.
- Raw I/O: `print:` (string → console), `printNewline:`. Serial via the native
  `Serial` capability (tier c).

**(b′) `mirror` capability primitives — the privileged-reflection tier (§2.1).**
Handed **only to the debug subsystem**, never to ordinary modules. Largely a
capability wrapper over reflective C prims that **already exist** (`oevm.h:279–296`):
stack walk (`method_context_sender`/`pc`), frame read/write
(`method_context_reg_at:`/`reg_at_put:`), class/vtable inspect
(`object_vtable`/`vtable_delegate`/`vtable_bindings`), closure home, single-step
(`fiber_set_single_step`), and hot-swap (`vtable_bind`,
`compiled_method_set_instruction_at`). This is what the image-level `debugger_hook`
(`../oevm/src/vm.c:170–185`) is handed so remote debug works capability-purely.

**(c) Native-module capabilities — the per-target portability tier (§2.1).**
`Serial` (for streams-from-serial), and the existing st-zephyr wrappers ported as
needed: `Gpio`, `Button`, `Timer`, `Thread` (`st-berry/samples/lib/std.st` is the
port source). Same Newspeak protocol, thin per-target C backend.

**Still-soft within §4 (settle during step-1 planning, not blockers):** whether
`String` is indexable-backed in `.ns` or keeps a primitive core; exact `platform`
method names (`basicNew:` vs `instantiate:` — default to the SOMns-style
`basicNew:`/`basicAt:` names already used above).

---

## 5. §3 Class scope — calibrated to st-zephyr (RATIFIED 2026-06-20)

**Core kernel library (`.ns`):**

- `Object` (root protocol): `isNil`, `notNil`, `ifNil:`/`ifNotNil:`, `=`, `~=`,
  `printString`, `asString`, `println`, `error:`, `hash`. (`==` via `platform`
  identity.) Subset SOMns `Kernel.ns` `Thing`/`Object`.
- `Array`: indexed protocol (`at:`, `at:put:`, `size`, `do:`, `collect:`, …)
  over `platform` indexable.
- `Collection` (abstract): `do:` (subclassResponsibility) + derived `collect:`,
  `select:`, `reject:`, `detect:`/`detect:ifNone:`, `inject:into:`, `includes:`,
  `isEmpty`, `size`, `asArray`.
- `OrderedCollection`: growable — `add:`/`addLast:`/`removeFirst:`/`removeLast:`,
  Array-backed with reallocation via `platform`.
- Stream family: `Stream` (abstract) → `ReadStream`/`WriteStream` over a
  collection, plus serial-backed read/write. Subset SOMns `Streams.ns`
  (`BasicInputStream` protocol: `next`, `peek`, `atEnd`, `upTo:`, `next:`,
  `do:`, `skip:` — `Streams.ns:35–175`).
- `String`: minimal — `,`, `size`, `do:`, `=`, `asString`, `printString`.
- `Dictionary` (DECIDED — in the first cut): `at:`, `at:put:`, `at:ifAbsent:`,
  `removeKey:`, `keysAndValuesDo:`, `includesKey:`, `size`. Open-addressing (or
  bucketed) table over the new indexable storage, keyed by `hash`/`=`.
  **Forcing function:** this makes the indexable repr design *key-capable from the
  start* (hashing + the boolean/SMI/Symbol `hash` prims of §4) rather than a
  retrofit — deliberately exercising the repr against its hardest client early.

**Follows trivially once Dictionary lands:** `Set` (a Dictionary with value=key);
add when first needed. **Other stretch:** none planned for the first cut.

**Native (as samples demand):** `Serial` first (justifies streams). `Gpio`,
`Button`, `Timer`, `Thread` ported from `std.st`.

---

## 6. SOMns reuse strategy

- **Copy-and-subset bodies**, swap `vmMirror` → `platform`, drop type annotations
  (`<Boolean>`, `^ <Vector[E]>`), drop `<:` Value, drop `usingPlatform:`-graph
  machinery beyond simple capability threading.
- Sources: `SOMns/core-lib/Kernel.ns` (957 ll), `Collections.ns` (344 ll),
  `Streams.ns` (1184 ll). Take protocol, leave the research surface.

---

## 7. Testing (RATIFIED 2026-06-20)

- **Regression net:** the existing `ns-tests` suite (47/47) must **stay green on
  both image paths** (Path A solidified-C, Path B `.oimg`). The refactor moves
  `Array`/`do:`/`printString` from C into `.ns` but their observable behaviour is
  preserved — the conformance suite is the safety net for the migration.
- **New protocol tests:** Collection protocol, OrderedCollection growth,
  **Dictionary** (insert/lookup/collision/grow, boolean+SMI+Symbol keys), Stream
  read/write, a serial round-trip (host-mockable `Serial`).
- **Mirror/debug tests:** extend `../oevm/tests/test_debugger.c` to drive the
  `mirror` capability (stack walk, frame read, hot-swap) through the image-level
  hook, not just the raw C prims.
- **Bar per milestone:** green on `oevm_runner`, both paths — not "parses."

---

## 8. Sequencing (RATIFIED 2026-06-20)

1. **VM:** real indexable object representation (length as tagged SMI, §3.2) +
   `platform` capability object with allocation/indexed/identity/I-O primitives;
   retire the `PRIM_Array` mock. Register `platform` (and library vtables) as GC
   roots (§3.2). **← step 1, goes to writing-plans first.**
2. **Compiler:** thread `platform` through module construction; resolve
   capitalised names via the injected namespace instead of `OP_SEND_IMPLICIT`
   string-match.
3. **`.ns` kernel:** `Object`, `Array`, `Collection`, `OrderedCollection`,
   **`Dictionary`** (+ the boolean `hash`/`=`/`printString` prims of §4).
4. **Streams + `Serial`** native module.
5. **`mirror` capability + remote debug:** bind the existing reflective C prims
   (`oevm.h:279–296`) onto a `mirror` capability handed only to the debug hook;
   wire to the `jast` MCP surface (next-phase debug item 2.3). Per-target backends
   (§2.1 native tier) land here as targets come online.
6. **Shrink `dispatch.c`:** remove public-selector interception from
   `PRIM_NAMES[]`; keep only receiver-intrinsic + capability primitives.
7. Keep `ns-tests` green throughout; add protocol tests at each step.

Each step gets its own implementation plan via the writing-plans flow.

---

## 9. Open questions — RESOLVED in the 2026-06-20 review

- **Booleans** → stay intrinsic + three Object-protocol prims (`hash`/`=`/
  `printString`); no `True`/`False` classes. (§4)
- **Class ceiling** → `Dictionary` **is** in the first cut (forces a key-capable
  indexable repr early); `Set` follows trivially; `OrderedCollection`+`Stream`+
  `Dictionary` is the ceiling. (§5)
- **`platform` split** → **not** collapsed: keep a `platform`/`mirror`/native
  three-tier split (§2.1). The mirror tier is kept for **remote-debug confinement**,
  not SOMns module-graph machinery (which is dropped). Portability lives in the
  native tier (§2.1), not the mirror split; per-target library variants ride the
  `platform` namespace (§3.1).
- **GC** → verified: no VM-GC changes; length-as-SMI handles variable-length
  objects; fragmentation on unbounded growth is the only gate, mitigated by
  per-target bounded variants (§3.2).

**Remaining soft items (settle during step-1 planning):** `String` repr
(indexable-backed vs primitive core); exact `platform` method names (default:
SOMns-style `basicNew:`/`basicAt:`).

## References

- `../oevm/src/dispatch.c:507–1045` — current C primitive surface + `PRIM_Array` mock.
- `../oevm/src/oevm.h:279–296` — existing reflective/debug C prims (the `mirror` tier's body).
- `../oevm/src/vm.c:170–185` — image-level `debugger_hook` invocation (why the mirror is a capability).
- `../oevm/src/gc.c:366` (`trace_object`), `:132` (fragmentation note), `:626–629` (root-marking pattern).
- `../oevm/src/builtins.def` — minimal firmware builtins (no Array/String/Object).
- `tools/ns_compile_oevm.py:370` (resolve fallback), `:1841–1852` (entry seam).
- `grammar/ns.ohm:5,18–25` — `ClassDef`/`CtorSig` (capability header parses).
- `SOMns/core-lib/Kernel.ns`, `Collections.ns`, `Streams.ns` — reuse source.
- `st-berry/samples/lib/std.st` — native-module (`_Gpio`/`_Thread`) port source.
- `st-oevm/docs/superpowers/specs/2026-06-15-st-oevm-design.md` §3/§7/§8/§11 — the
  template (injected-platform seam, debug-first obligations).
