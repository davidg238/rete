# ns-oevm — next phase: standard library + debug story

**Date:** 2026-06-19
**Status:** orientation / roadmap (pre-spec — informs the per-item plans that follow)
**Depends on:** the block-inlining track (complete) and the conformance suite
(47/47 green on both image paths).

## Where we are

The compute core and frontend breadth are done: OEOM dispatch, closures, NLR,
inline caching, field layout, super-sends, nested classes, class-side methods,
bignum, float literals, and the full block-inlining stack all work, and the
`ns-tests` suite is fully green on both Path A (solidified-C) and Path B
(`.oimg`). The remaining `oevm`-side item (`oevm#1`, closure representation) is
tracked in the sibling repo.

With conformance closed, the two big items for ns-oevm are no longer
language-feature work. They are:

1. **A nominal standard library** — today there is none.
2. **A debug story** — today there is none in ns-oevm, but the architecture is
   proven next door and the VM substrate already exists.

Neither is greenfield. The workspace already holds the template (`st-oevm`), a
proven LLM-agent-first debugger (`st-zephyr`), the VM substrate (`oevm`), and
library content/models (`st-berry`, `SOMns`). This doc grounds both items in
what exists and recommends a sequencing.

## The guiding design: st-oevm is the template

`st-oevm/docs/design/2026-06-15-st-oevm-design.md` is explicit that
st-oevm is "the **template** the future `ns-oevm` will be cut from." Its
north-star is **"clean, capability-safe, embedded, debug-first"**, with the
centrepiece workflow *edit a method, drop the stack frame, resume — over the
air* (§1, §8). Both items below are instances of what that design anticipated;
ns-oevm should inherit its decisions rather than reinvent them.

---

## Item 1 — Nominal standard library

### Current state

ns-oevm has **no Newspeak-level library**. The "std lib" is a set of C
primitives in `oevm` added ad-hoc as conformance tests demanded them:
`do:`, `at:`, `at:put:`, `size`, `printString`, `asString`, `asFloat`,
arithmetic, `bitXor:`, `out`/`println`, `new:`, `sqrt`. `Array>>do:` "landing"
meant adding a C primitive, not a library method. There is no Collection
hierarchy, no Object protocol, no streams in Newspeak source — and no prelude /
kernel `.ns` file at all.

### The seam: classlib-as-injected-platform

st-oevm §3 and §7 define the mechanism. A bare capitalised name that resolves to
no local/ivar is **not** a true global (there are none in the capability world);
it is a lookup in an **injected platform namespace** — "pass the class library
in as the Newspeak platform." This is the nominal-std-lib seam, and it is also
the seam that lets ST and NS eventually share one VM image.

The shape of that namespace capability is still an open question (st-oevm §11,
an M3-level concern). Resolving it is the first real design decision here.

### Direction

- Draw a clean **primitive / library line**: keep the C primitive set minimal
  (the genuinely-VM-level operations), and write everything expressible in
  Newspeak *in Newspeak source*, compiled into the image as the injected
  platform.
- **Content sources:** `st-berry/samples/lib/std.st` is the working model of a
  prepended source library; `SOMns` (sibling, and the origin of `ns-tests`) has
  a real SOM/Newspeak core library to port the hierarchy from.
- This is the larger and more open-ended of the two items; it wants its own
  brainstorming + design pass before any implementation plan.

---

## Item 2 — Debug story

### The proven architecture (st-zephyr)

`st-zephyr/docs/design/2026-04-10-remote-debug-design.md` is the
"modern, LLM-agent-first debugger" — the design literally states the debugger is
**LLM-agent-first**: the agent drives investigation using code, history, and
live device state; the human collaborates at the intent level; the VM is
invisible (the user thinks in the source language). Its stack:

```
Agent (Claude/MCP)  →  jast-gw gateway (source maps + SQLite sessions)  →  device debug hook
```

The agent-facing surface is a set of **MCP tools** (`jast`:
`debug_set_breakpoint`, `debug_continue`, `debug_step`, `debug_inspect`,
`debug_source`, …) — already available in this very session. A DAP/VS-Code
adapter is being added on top of the same gateway primitives
(`st-zephyr/.../2026-06-09-remote-debug-dap.md`).

That system is bound to the **Berry** VM (device-side debug hook in Berry,
`.stmap` source maps, the `jast-gw` Go gateway).

### The oevm substrate already exists

`oevm` — the VM ns-oevm targets — already has what the Berry VM lacked:

- A live **`debugger_hook`**, dispatched inside the VM loop
  (`oevm/src/vm.c:151–166`, `:666`), GC-rooted (`oevm/src/gc.c:624–628`).
- ROADMAP-listed `singleStep:`, debugger-hook registration, and
  `primitive_inspect_stack`, exercised by `oevm/tests/test_debugger.c`.
- **Frame reification** and **Phase 8 mirror-based hot-code swapping** — the VM
  half of *edit a method, drop the frame, resume* (st-oevm §1, §8).

So the device/VM layer is not the gap.

### The ns-oevm-specific gap: source maps

The front-end's two obligations toward the debug-first loop (st-oevm §8) are:

1. **Per-method compilation granularity** — a method compilable in isolation
   against a live vtable, so one binding can be patched without rebuilding the
   world. ns-oevm already emits methods as discrete `Method_*` / vtable-bound
   `CompiledMethod` artifacts, so the structure is method-keyed; isolation
   against a *live* vtable for hot-swap is the further step.
2. **Faithful source maps** — every instruction must trace to a source span
   (the `.stmap` the jast debug viewer already consumes).

Status of (2) in ns-oevm today:

- ✅ The parser **already emits per-node spans** — `grammar/ns-parse.js`
  `span()` returns `{startOffset, endOffset, startLine, startCol}`, the **same
  shape** st-oevm §7 mandates for shared source-map machinery.
- ❌ The compiler **drops them**. `tools/ns_compile_oevm.py` never threads spans
  through codegen and emits **no `.stmap`** (verified: no source-map/line-info
  code in the tools).

So the concrete, high-leverage debug-story work in ns-oevm is:

1. ✅ **DONE** — Thread the existing source spans through `ns_compile_oevm.py` so each emitted
   instruction carries its origin span.
2. ✅ **DONE** — Emit a `.stmap` (`.ns` line ↔ bytecode offset) in the same format the jast
   debug viewer / `jast-gw` already consume.
3. **Next:** Wire `oevm`'s live `debugger_hook` + `inspect_stack` to the same
   agent-facing MCP surface jast already defines (reuse, don't reinvent).

---

## What already exists (don't rebuild)

| Need | Already exists | Where |
|---|---|---|
| Debug-first design for the OEVM rail | st-oevm design spec | `st-oevm/docs/.../2026-06-15-st-oevm-design.md` |
| LLM-agent-first debugger architecture | st-zephyr remote-debug | `st-zephyr/docs/.../2026-04-10-remote-debug-design.md` |
| Agent-facing debug interface | `jast` MCP tools | live in this session |
| VM debug hook + stack inspect + hot-swap | oevm `debugger_hook`, Phase 8 | `oevm/src/vm.c`, `oevm/docs/archive/ROADMAP.md` |
| Per-node source spans | ns-parse.js `span()` | `grammar/ns-parse.js:16` |
| Std-lib seam (injected platform) | st-oevm §3/§7 | design spec |
| Std-lib content/models | st-berry `lib/std.st`, SOMns core | sibling repos |

## Recommended sequencing

**Source-map emission is the highest-leverage single piece.** It is:

- the gating prerequisite for the entire debug story, and
- already mandated as non-negotiable by the st-oevm template, and
- already half-done (parser emits spans; only the compiler needs to carry them).

It is small, well-scoped, and unblocks reuse of an already-built agent-first
debugger. Do it first, as its own implementation plan.

The standard library is the larger, more open-ended effort; its first step is a
design pass on the primitive/library line and the injected-platform namespace
shape (st-oevm §11), not code. Sequence it after — or in parallel with — the
source-map work, but give it its own brainstorming + spec.

## Adjacent constraint: embedded / Zephyr

The whole north-star is embedded + debug-first on Zephyr (`ns-zephyr` exists;
oevm's GP0 "GC & embedded memory" is the Zephyr gate). If embedded is the real
target, the std lib must stay small and GC must land — that constrains both
items above and argues for a minimal primitive/library line.

## References

- `st-oevm/docs/design/2026-06-15-st-oevm-design.md` — the template.
- `st-zephyr/docs/design/2026-04-10-remote-debug-design.md` — agent-first debugger.
- `st-zephyr/docs/superpowers/plans/2026-06-09-remote-debug-dap.md` — DAP layer.
- `oevm/docs/archive/ROADMAP.md` — VM debug/hot-swap substrate (Phase 8, debugger primitives).
- `docs/superpowers/plans/block-inlining-status.md` — the now-complete prior track.
