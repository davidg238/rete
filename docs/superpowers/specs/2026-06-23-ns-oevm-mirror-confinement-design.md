# ns-oevm Mirror tier — confining ambient reflection (the privileged-reflection capability)

**Status:** ratified design (brainstorm w/ David, 2026-06-23) — **RE-SEQUENCED as a follow-up.**
The confinement probe (run while planning this) found that `sender`/`home`/`regAt:`/`regAt:put:`
are **emitted by base-level code generation** (lexical closures + NLR), so they cannot be
confined until base-level execution is stratified off reflection first. That prerequisite is
its own step — `2026-06-23-ns-oevm-stratify-execution-off-reflection-design.md` — which must
land before this one. After it, this spec's **full** §3/§4 surface (including the stack-walk
trio) is confinable as written; **no design here is discarded, only re-ordered.** Until then,
treat §3/§4's `sender`/`home`/`regAt:`/`regAt:put:` rows as *blocked-on-de-entanglement*.
**Predecessor:** 3c-3b (Serial — the first *native* capability tier), merged + pushed
(master: ns-oevm `5c6c4b5`, oevm `b7c3554`, ns-tests `63eb4e2`; suite 63/0/3 both paths).
**Successor:** the in-image debugger and the `debugger_hook` → jast MCP wiring (the rest of
the debug story) build *on top of* the mirror capability this cycle stands up. Each is its
own spec → plan → SDD cycle.
**Specs cross-ref:** `2026-06-20-ns-oevm-stdlib-design.md` (capability spine; §2.1 three
tiers — platform / **mirror** / native), `2026-06-22-ns-oevm-stdlib-step3c3b-serial-design.md`
(the native-tier pattern — one capability = one C module + one receiver-identity hook —
that this cycle reuses), `2026-06-19-next-phase-stdlib-and-debug.md` (the debug story this
unblocks), `docs/NEWSPEAK_DIALECT.md` (divergence governance).

---

## 0. Scope decision — confinement only (ratified 2026-06-23)

The **mirror** is the third and final capability tier: privileged reflection (stack walk,
frame read/write, class introspection, single-step, hot-swap, break-hook registration),
the kept half of SOMns's vmMirror/platform split. The reflective C substrate **already
exists and works** (`../oevm/src/vm.c` prim bodies; `../oevm/tests/test_mirrors.c` already
demonstrates a C-level stack walk via `sender`/`pc`). The gap is **capability purity**:
today these prims are dispatched as **universal primitives keyed on selector name**
(`../oevm/src/dispatch.c` `switch(pid)`, ~lines 658–740) — *any* image code that sends
`sender` or `singleStep:` reaches them, regardless of receiver. That is ambient reflection.

**This increment delivers confinement, not a debugger.** Done = the reflective prims stop
being ambient and become reachable **only** by holding a `mirror` capability object, and a
test proves an ordinary app can no longer reflect. **No new debugger behaviour**, no
in-image debug module, no jast wiring — those are later steps built on this boundary.

**Why confinement is non-breaking:** **no `.ns` code anywhere uses these selectors today**
(verified: `lib/kernel.ns` and the whole `ns-tests` suite are clean of `sender` /
`singleStep` / `registerDebuggerHook` / `regAt:` / `bind:` / `vtable` as message sends).
Confining them removes a hole nobody is standing in.

**Grant policy this cycle: NO image grant.** We stand up the `mirror` singleton and route
reflection through it, but **no boot path hands a mirror to anyone** — so *no `.ns` code can
reflect at all yet*. The grant mechanism is deliberately designed later, **with** the
debugger (see §5). The intended eventual owner (David, 2026-06-23) is the **nodus-st
supervisor "kernel"** that orchestrates containers and responds to porta-server commands —
it will hold the mirror and initiate debug via command. This cycle wires none of that.

---

## 1. Goal & shape

Flip reflection from **ambient** (any receiver, by selector name) to **confined**
(reachable only by holding and naming the mirror), completing the three-tier capability
model. Mechanically this means flipping the protocol from **receiver-side** to
**mirror-mediated** — the SOMns vmMirror shape — because the VM has **no notion of caller
identity**: confinement can only key on **receiver identity** (you must hold the mirror
object and be able to name it). There is no other lever.

This design rests on Bracha & Ungar's three mirror principles (encapsulation,
stratification, ontological correspondence) and on the source-verified reflection models of
SOMns and PrimordialSoup. The full alignment — including how the mirror-mediated shape is
what later enables **remote debugging** and **post-mortem** reflection, and the one honest
gap (encapsulation) this confinement-only increment does not yet close — is in **§11**.

```newspeak
"Today (ambient): any receiver, anywhere"
ctx sender.                         "→ reaches primitive_method_context_sender"

"Confined (mirror-mediated): only a holder of `mirror` can name this"
mirror senderOf: ctx.               "→ same prim, reflected object as arg 0"
```

A module not handed a `mirror` has **no way to name one**, so every reflective operation is
a runtime DNU — exactly the confinement evidence `39_serial_confined.ns` established for
serial.

---

## 2. Architecture & mechanism — one capability = one C module + one receiver hook

Reuse the pattern serial (3c-3b) and platform (3c-3b extraction) proved:

- **New `../oevm/src/mirror.c`** (sibling to `platform.c`, `serial.c`): a static singleton
  `&MirrorObj`, a `mirror_object()` accessor, and `mirror_dispatch(selector, argc, args,
  &out)` with its own selector matching.
- **One early receiver-identity hook in `send()`** (`dispatch.c`), beside the existing
  serial gate (`dispatch.c:604`) / platform gate (`dispatch.c:648`):
  ```c
  if (receiver == mirror_object()) return mirror_dispatch(selector, argc, args);
  ```
  A confined app never names the mirror, so it never reaches `mirror_dispatch`.
- **`mirror_dispatch` shims to the existing `vm.c` prim bodies** — **no reflection logic is
  rewritten**. `mirror senderOf: aCtx` calls `primitive_method_context_sender(aCtx, 0,
  NULL)`; the reflected object simply moves from *receiver* to *argument 0*. The prim
  implementations in `vm.c` are untouched.
- **The reflective cases are removed from the universal `switch(pid)`** in `dispatch.c`.
  After removal, sending `sender` / `singleStep:` / etc. to any ordinary receiver falls
  through to normal vtable lookup → **DNU**. That DNU *is* the confinement.

**No GC root** for `&MirrorObj` (static BSS, like `&SerialObj` — `mark()` skips it,
`gc.c` `is_valid_heap_object`). **No `.oimg` format-version bump, no registry-version
bump:** the mirror adds no new builtin vtable and no persisted state — it is a plain C
singleton reached by identity, exactly like serial.

**Alternative considered (rejected):** gate each reflective case *in place* inside the
universal `switch(pid)` with `if (receiver == mirror_object())`. Rejected — it leaves
reflection scattered through the 66 KB `dispatch.c` and breaks the one-cap-one-module
pattern. Extraction into `mirror.c` matches serial/platform and isolates the surface for
the build-seam (§4).

### 2.1 Runtime cost — effectively zero

Confinement *removes* the reflective cases from the hot `switch(pid)` (shrinking it) and
*adds* one pointer compare (`receiver == mirror_object()`) per send, identical in kind to
the serial gate already present. A confined app sends no reflective selector, so it never
reaches `mirror_dispatch`. Net per-send cost is noise — one register compare against a
singleton, no allocation.

---

## 3. The mirror protocol surface

`mirror_dispatch` shims each selector to the existing prim body, moving the reflected
object from receiver to arg 0:

| Today (ambient, receiver-side) | Confined (mirror-mediated) | Shims to |
|---|---|---|
| `anObj vtable` | `mirror vtableOf: anObj` | `primitive_object_vtable` |
| `aVt delegate` | `mirror delegateOf: aVt` | `primitive_vtable_delegate` |
| `aVt bindings` | `mirror bindingsOf: aVt` | `primitive_vtable_bindings` |
| `aCtx sender` | `mirror senderOf: aCtx` | `primitive_method_context_sender` |
| `aCtx pc` | `mirror pcOf: aCtx` | `primitive_method_context_pc` |
| `aCtx regAt: i` | `mirror reg: aCtx at: i` | `primitive_method_context_reg_at` |
| `aCtx regAt: i put: v` | `mirror reg: aCtx at: i put: v` | `primitive_method_context_reg_at_put` |
| `aFiber singleStep: b` | `mirror fiber: aFiber singleStep: b` | `primitive_fiber_set_single_step` |
| `aVt bind: sel to: m` | `mirror vtable: aVt bind: sel to: m` | `primitive_vtable_bind` |
| `aMethod instructionAt: i put: x` | `mirror method: m instructionAt: i put: x` | `primitive_compiled_method_set_instruction_at` |
| `registerDebuggerHook: cl` | `mirror onBreak: cl` | `primitive_register_debugger_hook` |

`mirror homeOf: aClosure` (→ `primitive_closure_home`) is a **candidate pending the §4
probe**.

**Naming principle:** read-side queries are `<thing>Of:` keyword sends; mutations are full
keyword messages naming the target first (`reg: ctx at: i put: v`). **No reflective
operation stays as a unary message on an arbitrary receiver** — that property is precisely
what makes it un-nameable without holding the mirror.

---

## 4. What moves vs what stays universal (the crux)

The `switch(pid)` mixes debugger-reflection with **core language runtime**. **Only the
genuinely-reflective subset moves behind the mirror; everything else stays universal.**
Confining a core prim breaks the whole suite, so each candidate is probed before it is
lifted — the same verify-first discipline that de-risked serial's boot machinery.

**Moves behind mirror** (the 11 in §3 — all debugger/introspection):
`vtable`, `delegate`, `bindings`, `sender`, `pc`, `regAt:`, `regAt:put:`, `singleStep:`,
`bind:to:`, `instructionAt:put:`, `registerDebuggerHook:`.

**Stays universal — must NOT be confined** (core language runtime, not reflection):
- `==` (`PRIM_eqeq` → `primitive_object_equals`) — used everywhere.
- Fiber / continuation: `fiberCreate`, `fiberSwitch`, `resumeWith:`, `executeCleanup` —
  these back concurrency and NLR / `ensure:` / `on:do:`; the kernel relies on them
  universally.
- `createClosure` — closure construction.
- All arithmetic / collection / boolean prims (`&`, `|`, `//`, `to:do:`, …).

**Probe-before-moving** (plausibly reflection, but possibly load-bearing — verify
empirically, do **not** assume):
- **`home` (`primitive_closure_home`)** — looks like introspection, but NLR may reach the
  home context via a `home` message. *Probe:* grep compiler emit + `lib/kernel.ns` for the
  `home` selector; lift it and run the full suite both paths. If NLR uses it, it stays
  universal (or is dual-listed — universal *and* mirror-reachable). Default if uncertain:
  **stays universal.**
- **`outer` (`primitive_object_outer`)** — module outer access. The compiler resolves outer
  refs at compile time (`OP_LD` / slot reads), so `outer`-as-a-message is *probably* pure
  reflection — but *probe:* confirm nothing emits the `outer` selector before lifting. (It
  is **not** in the §3 surface; only lift it if the probe shows it is safe and we want it.)

**The probe is mechanical and decisive:** lift a candidate, run the full suite both paths;
any regression means it is load-bearing and stays universal. The plan **front-loads** this
classification (its first task) so the set is locked before `mirror.c` is built.

---

## 5. Grant policy — none this cycle; designed later with the debugger

**This increment wires no grant.** No boot path constructs or hands a mirror; the `mirror`
singleton exists and the reflection prims route through it, but **no `.ns` code holds one,
so no `.ns` code can reflect.** The confinement test (§6) proves the app gets DNU; a
rewritten C-level `test_mirrors.c` proves the prims still work *through* the mirror object.

**Why defer the grant:** the grant target is genuinely a function of the debugger
architecture we deliberately scoped out. The intended eventual owner is the **nodus-st
supervisor "kernel"** (David, 2026-06-23) — it orchestrates containers, responds to
porta-server commands, and would hold the mirror and initiate debug via command. Picking a
grant mechanism now, before that consumer exists, would be guessing. The capability
boundary stood up here is exactly what lets the grant be added later as a small, additive
boot step (one more receiver threaded to the supervisor domain — the serial boot-threading
pattern), with **no change to the mirror module or the protocol**.

**Confinement is therefore total this cycle:** even an app that *tries* to declare or name a
mirror gets nothing, because boot threads no mirror to any entry. There is no serial-style
"any entry that declares `mirror:` gets it" hole — the grant simply does not exist yet.

---

## 6. Build seam — confine now, removal ready-to-flip (ratified 2026-06-23)

Because reflection becomes a **self-contained module reachable only by holding the
mirror**, a production/embedded target that grants no mirror can **omit `mirror.c` at link
time** — the one `send()` hook compiles to nothing and the whole reflection surface drops
out of the binary: zero code, zero cost, smaller image. This is the `serial_backend.h`
link-time-seam philosophy applied to reflection, and it is impossible with the current
ambient prims (welded into the universal switch).

**This cycle structures for that removal but does not wire a production build:**
- `mirror.c` is self-contained; the `send()` hook is the single reference into it.
- The spec / `mirror.c` header documents the **exact flag and build-line edit** that would
  omit it (a future `OEVM_REFLECTION`-style guard around the one hook + the `mirror.c`
  compile-unit, mirroring how `$OEVM_TARGET` selects backends).
- We do **not** add the build flag, do **not** guard the `vm.c` prim bodies, and do **not**
  add a reflection-free build config this cycle. Removal becomes a trivial, well-documented
  follow-on the moment a target needs it.

---

## 7. Testing

- **`test_mirrors.c` rewrite (C level):** the existing test sends `sender`/`pc`
  *receiver-side*; under confinement those DNU. Rewrite it to go through the mirror object
  (`send(mirror_object(), senderOf:, 1, &frame)` etc.). This becomes the **proof the prims
  still work via the mirror** — the positive half of confinement. (Also update
  `test_debugger.c` if it sends reflective selectors receiver-side.)
- **New `ns-tests/04_somns_basic/40_mirror_confined.ns`** (`expect-runtime-fail:`, the
  `39_serial_confined.ns` shape): an app sends a reflective selector (`ctx sender` or a bare
  `mirror …`) and aborts with DNU — *the failure is the passing evidence* that reflection is
  no longer ambient and no grant exists.
- **Full conformance suite, both paths** (`python3 tools/run_tests_oevm.py`, `OEVM_PATH=AB`)
  must stay green — this is the guard that the §4 classification didn't confine a
  load-bearing prim. Target **64/0/3** (baseline 63 + the new confinement pin).
- **Regenerate the committed default `solidified_image.c`** as the final deliberate step
  (it is otherwise clobbered by the runner — `git checkout` it before oevm commits).

---

## 8. Dialect governance

Add **`docs/NEWSPEAK_DIALECT.md` #14** = *reflection is a confined capability, not ambient.*
Reflection (stack walk, frame read/write, class introspection, single-step, hot-swap,
break-hook registration) is reachable **only** through a `mirror` capability, in
mirror-mediated form (`mirror senderOf: ctx`, not `ctx sender`); there is **no ambient
reflection protocol on Object**. Revisit trigger: if read-only introspection (e.g. `class`)
is ever wanted as universal Object protocol, carve out a documented read-only subset then
(reversible — re-add specific universal cases).

---

## 9. Risks & open probes (front-loaded into the plan)

1. **Mis-classification (highest risk):** confining a load-bearing prim (`home`, or anything
   in the "stays" list) breaks the suite. *Mitigation:* §4 probe is the plan's first task;
   default-to-universal when uncertain; the full suite both paths is the decisive gate.
2. **Selector collision:** a reflective selector name (`sender`, `pc`, `home`, `bind`,
   `delegate`, `bindings`, `vtable`) might also be a legitimate user/kernel *method* name,
   in which case removing the universal case is correct (the method wins via vtable) but
   must be confirmed not to change behaviour. *Probe:* grep compiler emit + suite + kernel.
3. **C-test breakage is expected, not a regression:** `test_mirrors.c` / `test_debugger.c`
   sending reflective selectors receiver-side will DNU after confinement — that is the
   signal to rewrite them mirror-mediated (§7), not a bug.
4. **Two-path parity:** the change is pure C dispatch (`mirror.c` + `dispatch.c` hook +
   switch removal) shared by both image paths — **no `.oimg`/loader/serializer change**
   (much smaller than serial). Both paths must stay green.

---

## 10. Task decomposition (outline for writing-plans)

1. **Classify & probe** — empirically lock the move/stay/probe sets (§4); resolve `home`
   and `outer`; produce the authoritative selector list. Suite stays 63/0/3 (no behaviour
   change yet — probe only).
2. **`mirror.c` module** — `&MirrorObj` singleton + `mirror_object()` + `mirror_dispatch`
   shimming to the existing `vm.c` prims (§2, §3). Wired into the build (both Path A and
   Path B link the new compile unit). Not yet hooked into `send()`.
3. **Confine** — add the `send()` receiver-identity hook; **remove** the reflective cases
   from the universal `switch(pid)`. Reflection is now mirror-only.
4. **C-test rewrite** — `test_mirrors.c` (+ `test_debugger.c` if needed) go mirror-mediated;
   prove the prims still work through the mirror object.
5. **Confinement pin + dialect + default image** — `40_mirror_confined.ns`
   (`expect-runtime-fail:` DNU), `NEWSPEAK_DIALECT.md` #14, regenerate committed
   `solidified_image.c`. Target **64/0/3 both paths**. Document the §6 build-seam removal
   recipe in `mirror.c`'s header.

---

## 11. Prior art & alignment (Bracha–Ungar *Mirrors*; SOMns; PrimordialSoup)

All three reference systems were **source-verified** for this design: the Bracha & Ungar
paper read in full (OOPSLA 2004, *"Mirrors: Design Principles for Meta-level Facilities of
Object-Oriented Programming Languages"*; Most Influential Paper, OOPSLA 2014), and the SOMns
(`/home/david/workspace/SOMns`) and PrimordialSoup (`/home/david/workspace/primordialsoup`)
reflection implementations read from their actual sources.

### 11.1 The three mirror principles vs. this design

| Principle (Bracha–Ungar) | Definition | ns-oevm (this increment) |
|---|---|---|
| **Encapsulation** | Meta-level interface divorced from any one implementation (abstract `Mirror`, not concrete `Class`). Their villain: `aCar.getClass() == Car.class` breaks under an alternate `Car` impl. | **Partial.** Mirror-mediated form is right, but we return **raw VM oops** (`Vtable`, `MethodContext`), not abstract mirror-wrapped results. Full compliance needs the deferred `.ns` `ObjectMirror`/`ClassMirror` **facade** layer (SOMns `Mirrors.ns` / PrimordialSoup `ObjectMirror reflecting:` analog). Honest gap; see §11.4. |
| **Stratification** | Meta-level separated from base so it imposes no cost unused and *"reflection can be removed"* when *"the application makes no reference to entry point(s) associated with reflection."* | **Strongest alignment — validates §6.** Capability injection makes "no reference to the entry point" **structurally enforced**, not statically inferred (no grant → no module can name a mirror → provably unreferenced). Cleaner than Strongtalk's analysis-based removal. Their footprint motivation (*"mobile phones, PDAs, smart cards or other embedded systems"*) is st-zephyr's exactly. |
| **Ontological correspondence** (structural + temporal) | Every construct mirrored; *"mirroring code and mirroring computation should be separable modules."* Strongtalk splits high-level `Mirror` (HLL semantics) from low-level `VMMirror` (integers/arrays/classes, *"the call stack and individual stack frames (activation records)"*). | Our `mirror` **is the VMMirror (low-level) stratum** — `vtableOf:`/`senderOf:`/`pcOf:`/`regAt:`/`instructionAt:put:` reflect VM structures, not HLL Newspeak concepts. A future HLL facade is the `Mirror` layer atop it. The paper names and blesses this exact layering. |

### 11.2 SOMns and PrimordialSoup — source-verified comparison

| | **SOMns** | **PrimordialSoup** | **ns-oevm (this design)** |
|---|---|---|---|
| Reflection access | `vmMirror objClass: obj` (msg to mirror) | `ObjectMirror reflecting: obj` facade → private kernel prims | `mirror senderOf: ctx` (msg to mirror) |
| Confined or ambient? | Capability (lexical `usingVmMirror:` injection) | Capability (private kernel methods + injected `Mirrors` module) | Capability (receiver-identity + boot grant; **no grant this cycle**) |
| Reflective prims at the C/VM level | **Segregated** — separate `vmMirrorPrimitives` map → own primitive class | **Intermixed** — flat numbered table (130 `instVarAt`, 164 `currentActivation`…), confined only at the NS layer | **Segregated** — lifted out of the universal `switch(pid)` into `mirror_dispatch` (matches SOMns) |
| Scope of the mirror object | Broad — vmMirror also carries *all* language prims (even `int:add:`) | Narrow facade over kernel prims | **Reflection-only** — ambient-language prims stay universal; finer platform/mirror/native split |

Two takeaways: (1) ns-oevm's pre-confinement state is **more ambient than PrimordialSoup**
(ours are callable on any receiver; PS hides them behind private kernel methods) — this
increment moves us to SOMns's **segregated** model. (2) ns-oevm's three-way split is **finer**
than SOMns's two-object (Platform + vmMirror) model: we keep ambient-language prims universal
and move only debugger-reflection, consistent with the existing `platform` capability.

### 11.3 Remote debugging & post-mortem — future, *enabled-not-built*

This increment is **in-process and builds no debugger**, but the mirror-mediated shape is the
single decision that keeps both futures reachable. Recorded here so the shape is not
accidentally regressed later.

- **Remote (the paper's core case study, §2.1).** Java core reflection cannot do remote
  (encapsulation violation — `Class` is concrete, no proxy impl); **JDI** can because it is
  mirror-based and *"designed with distributed use in mind"* — *"one can construct different
  implementations of the interface VirtualMachine… reflection on either the current (local)
  process or remote processes."* The reflectee is an **argument** to the mirror, so the mirror
  can be a **proxy** to another address space. Receiver-side `ctx sender` cannot — the object
  must be live and local to answer. **Therefore `mirror senderOf: ctx` (not `ctx sender`) is
  load-bearing for the eventual remote story, even though we ship in-process.** The paper's
  deadlock warning — *"Debugging the same virtual machine… is not recommended… substantial
  risk of deadlock"* — is a direct argument for the **nodus-st supervisor holding the mirror
  out-of-process** and driving the debuggee container via porta commands (§5's named grant
  target). The paper's *Future Work* envisions precisely *"an IDE remotely manipulating a small
  footprint VM that does not include a full implementation of reflection, such as one found on
  a PDA or mobile phone"* — jast/porta driving oevm-on-zephyr.
- **Post-mortem (temporal correspondence, §4.1).** The paper's **Klein** result: a VM with *no*
  reflective API is debugged from a separate process over sockets — *"No changes were needed to
  Self's mirror API — only a new implementation was needed."* Applied to ns-oevm: a crashed or
  snapshotted **`.oimg`** is a dead object graph that cannot execute a bytecode, but because the
  reflectee is a handle passed to the mirror, the **same** `mirror senderOf:` / `mirror reg:at:`
  protocol can walk frames in a loaded snapshot via a *new mirror implementation* (handle →
  snapshot resolver), not a new protocol — a post-mortem stack walk. This rides the code/
  computation split (*"mirroring code and mirroring computation should be separable modules"*):
  `.oimg` is code+state; a frozen fiber is computation.

### 11.4 Honest gaps & what they imply for sequencing

- **Encapsulation is only at the gateway.** Returning raw oops is acceptable for a low-level
  VMMirror stratum and for confinement, but a Bracha-compliant *public* reflection API needs the
  HLL **facade** (`ObjectMirror reflecting:` etc.). That facade is the natural first task of the
  **deferred in-image debugger** step, built in `.ns` atop this gateway — not part of
  confinement.
- **Remote/post-mortem are enabled, not implemented.** Both require a *second mirror
  implementation* (a remote proxy / a snapshot resolver). Nothing here builds one; the design
  obligation is only to keep the protocol reflectee-as-argument so that second implementation
  needs no protocol change — the Klein lesson.
- **Intercession is out of scope.** The paper's third capability (intercession — altering
  language semantics from within) is not in this tier; our hot-swap (`bind:to:`,
  `instructionAt:put:`) is self-modification, not intercession. No change intended.
