# Explicit Nested-Class Access + Outer-Sends — Design

**Date:** 2026-06-18
**Status:** Proposed (design); awaiting approval.
**Roadmap:** P1 conformance — unblocks `06_outer_sends`, `01_deep_nesting`, and
(at least partly) `07_deltablue`.
**Approach chosen by user (2026-06-18):** runtime "bound-class" model, design-first.

## Problem

Two linked Newspeak features are unimplemented:

1. **Explicit-receiver nested-class access.** `o1 Outer2` (send a nested-class
   name to an instance of the enclosing class) raises DNU. Nested classes get
   *instance slots* (populated with their vtable at init, compiler lines
   1285-1292) but **no accessor methods**, so a plain `OP_SEND Outer2` to `o1`
   finds no binding. Implicit `Outer2 new` works only because the compiler
   resolves the bare name to a class statically (`resolve_class_ref`) and uses
   the compile-time construction path (lines 704-764).

2. **Outer-binding on runtime construction.** `o2:: o1 Outer2 new` must set
   `o2`'s `outer` (slot 0) to `o1`. The outer-setting + init machinery is
   entirely **compile-time** (lines 714-764), firing only when the class is
   statically named. With `o1 Outer2 new`, the `new` receiver is a runtime value.

3. **Outer-sends.** `outer C sel` (e.g. `outer Outer7 val7`) — the grammar has no
   special `outer` form (ns.ohm parses it as plain unary sends
   `((outer Outer7) val7)`), and `resolve_name` doesn't special-case `outer`
   (falls to `implicit_send` → DNU). `outer C` must yield the lexically-enclosing
   instance of class `C`.

## Why a runtime model (not compile-time)

Bare nested-class names are **not** program-unique: `06_outer_sends` has `A.C`,
`A.D`, `B.C`, `B.D`. The compiler can't statically know whether `recv C` means
`A.C` or `B.C` (it doesn't track `recv`'s type). **Normal runtime dispatch
resolves this for free:** a synthetic `C` accessor on `A`'s vtable and another on
`B`'s vtable each return their own nested class; `recv C` dispatches to whichever
matches `recv`'s actual vtable.

## Design

### 1. BoundClass — a runtime "class bound to its enclosing instance"

A `BoundClass` is an ordinary `ObjectInstance` with `vtable = BoundClassVtable`
and three slots:

| slot | holds |
|---|---|
| 0 | `nested_vtable` — the nested class's vtable (a class object) |
| 1 | `outer` — the enclosing instance to use as the new object's slot 0 |
| 2 | `slot_count` — SMI: the nested class's instance slot count |

No new opcode is needed: an accessor builds one with `OP_NEW(BoundClassVtable, 3)`
+ three `OP_ST_SLOT`s. `BoundClassVtable` is a new builtin vtable (register in
`kernel_boot.c` / `solidified_image.c`, and as a GC root + leaf in `gc.c`).

### 2. Synthetic nested-class accessors (compiler)

Alongside the existing public-slot getters (ns_compile_oevm.py:1166-1192), emit
one accessor per nested class on the enclosing class's vtable:

- **selector** = bare nested-class name (e.g. `Outer2`, `C`).
- **body**: `OP_NEW(BoundClassVtable, 3)` → `tmp`; `ST_SLOT 0 = LD_CONST
  VTABLE:<nested vt_id>`; `ST_SLOT 1 = self` (R0); `ST_SLOT 2 = LD_CONST
  SMI:<nested slot_count>`; `OP_RET tmp`.

`recv NestedClass` then resolves by normal dispatch to the correct accessor.

### 3. Construction protocol on BoundClass (VM/dispatch)

In `send()` (dispatch.c), **before** normal lookup, intercept receivers whose
vtable is `BoundClassVtable`. For ANY selector `S` (`new`, `a:b:c:`, …):

1. Read `nested_vtable`, `outer`, `slot_count` from the BoundClass slots.
2. Allocate `ObjectInstance(nested_vtable, slot_count)` (slots nil-initialised,
   matching OP_NEW).
3. Set `slot[0] = outer`.
4. Look up `init_<S>` on `nested_vtable` (string-keyed lookup, since the
   constructor selector is dynamic and a runtime-built symbol won't pointer-match
   the interned one) and invoke it with the call's args.
5. Return the new instance.

This mirrors the compile-time sequence (OP_NEW + set-outer + `OP_SEND init_<sig>`)
exactly, so initializers and `superInit` behave identically.

### 4. Outer-sends (compiler)

Special-case the identifier `outer` in the send path. When a unary/keyword send's
receiver chain begins with bare `outer` followed by a class-name message `C`:

- `outer C` compiles to: walk `hops` `outer` slots (slot 0) from `self` up to the
  enclosing instance **of class `C`** (reuse the existing outer-walk in the
  `nested_class` resolution, lines 466-478, which already computes `hops` for a
  container class). The result is that enclosing **instance**.
- The trailing message (`val7`, `bar`, …) is then a normal send to that instance.

`outer C` yields an **instance**, not a BoundClass — distinct from §2 (`recv C`).

## Scope / non-goals

- **In:** explicit nested-class access, runtime outer-binding via BoundClass,
  `outer C sel`. Targets: `01_deep_nesting`, `06_outer_sends`. `07_deltablue` is
  expected to advance but may surface further gaps (treat as a follow-on).
- **Out:** super-sends in class clauses (`07_super_sends`, parser work) and bignum
  (`04_integer_overflow`) — separate P1 items, separate specs.
- The compile-time construction path for statically-named classes is **unchanged**
  (fast path); this work is purely additive (new accessors + BoundClass intercept).

## Testing / gates

- New `.ns` behavioral coverage already exists: `01_deep_nesting` (expect `8`),
  `06_outer_sends` (expect `3 / 2 / 6 / 666`). Drive development against these.
- Add a focused C unit test for the BoundClass construction intercept if
  practical.
- **Gate:** conformance must improve (deep_nesting + outer_sends → PASS) with **no
  regressions** on the other 36, on Model A *and* B (`OEVM_PATH=AB`), and `-m32`
  still passes.
