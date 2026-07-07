# review/lib — designed library surface (pre-implementation staging)

**Status:** designed 2026-07-03 against `language-and-library-spec.md` (this directory).
Staged here so implementation can start the moment the Milestone-A VM is green. Nothing
in here is loaded by any harness yet.

## How to read these files

- **`.ns` files are written in the nsl dialect, in final form**, but as *staging
  modules*: nsl has no open classes, so methods destined for existing kernel classes
  (`Collection`, `OrderedCollection`, `Array`) are grouped in a staging class with a
  `DESTINATION:` comment per method group. Landing them = pasting into
  `nsl-tuvm/lib/kernel.ns` at the marked class.
- Files that need **new VM primitives** carry a `REQUIRED PRIMITIVES` block at the top —
  that block is the C-side work order; everything below it is done nsl code.
- **`.md` files** are designs for things that are not nsl code: the error convention,
  the CBOR seam (C), and the Int64 boxing (VM).

## Contents, in landing order

| File | Spec § | Needs | Ready when |
|---|---|---|---|
| `errors.md` | L5 | nothing — a convention | now (adopt before stdlib grows) |
| `integers.md` | L7 | VM: boxed Int64 on 32-bit | decide now; build with Milestone B |
| `combinators.ns` | L1 | existing kernel only | Milestone A green |
| `messages.md` + `messages.ns` | spec §2 / Milestone B | VM: immutable list/map/record | with Milestone B |
| `bytes.ns` | L2 | VM: byte-indexable prims | after B (Bytes is algebra `bytes`) |
| `strings.ns` | L8 | VM: UTF-8 validation prim | with the stdlib String port |
| `fsm.ns` | L4 | Dictionary + channels API | **LANDED** — `nsl-tuvm/lib/kernel.ns` `class Protocol` / `class Transition` (~L659-768), Milestone C Task 5. One adaptation: the staged missing-table branch's escaping block declaring its own temp slot (`fsm.ns:48`) was replaced by reusing the method temp `table` (a real compiler/VM limitation on escaping blocks with fresh temp slots, isolated-probe confirmed, not a logic bug) — public surface unchanged (`state:on:do:then:`, `state:onEntry:`, `onUnhandled:`, `startIn:`, `trace:`, `currentState`, `snapshot`/`restore:`, `handle:`). |
| `timers.ns` | L3 | C seam: timer event source | Milestone D (host seam exists) |
| `cbor.md` | L6 | C seam codec | porta wire step (design frozen now) |
| `exceptions.md` | spec §8 | nothing — constraints only | read before Milestone D; full design at §8 time |

## Cross-cutting rules (from L5, applied throughout)

1. Expected absence/failure takes an explicit continuation keyword (`ifAbsent:`,
   `ifNone:`, `ifInvalid:`). No sentinel returns without an `if…:` variant.
2. Everything else crashes (`self error:`) — let-it-crash; the supervisor handles it.
3. No method in these files uses `^` inside a block passed to another method — the
   found-flag / combinator style is used exactly as the no-NLR dialect requires, so
   these files are also style exemplars.
