# Std-lib step 3c-3a — Streams (ReadStream + WriteStream) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `ReadStream` and `WriteStream` to the Kernel library — byte-granular,
String-focused streams (input parsing + an `io.Buffer`-style String builder) on the
immutable UTF-8 `String` from 3c-1.

**Architecture:** Near-pure-source. Two new sibling classes in
`lib/kernel.ns` (`Kernel usingPlatform: platform`), reached by apps via two new
accessors, plus **one** new platform primitive `platform string: anArray` (build a
`String` from an `Array` of byte-SMIs — the only path from bytes back to a `String`).
The primitive is one `case` on the platform-receiver dispatch path in `dispatch.c`,
shared by **both** image paths (Path A solidified-C, Path B `.oimg`) — no `.oimg`
format / builtin-registry change.

**Tech Stack:** C99 VM (`../oevm`), Python compiler (`ns-oevm/tools`), Newspeak
`.ns` (`lib/kernel.ns`), conformance `.ns` (`../ns-tests`).

**Spec:** `docs/superpowers/specs/2026-06-22-ns-oevm-stdlib-step3c3-streams-design.md`.

**Controller pre-verification (already done — anchors below are empirically
confirmed):** a throwaway probe with the exact `platform string:` C prim + minimal
`ReadStream`/`WriteStream` + a probe `.ns` ran **green on BOTH paths** (60/0). It
confirmed: the `platform string:` dispatch site & impl; `WriteStream`'s
`| bytes ::= OrderedCollection new. |` slot-init under `platform WriteStream new`;
class-side `self new` + a setter for `ReadStream on:`; the `'é'` byte round-trip; CR
strip; `platform` reachable inside stream instance methods. All probe artifacts were
reverted.

## Global Constraints

- **THREE sibling repos**, commit per-repo: **ns-oevm** (`lib/kernel.ns`, `docs/`,
  `tools/`) · **../oevm** (C VM: `src/dispatch.c`, committed default image) ·
  **../ns-tests** (conformance `.ns`). Create feature branch
  **`feat/stdlib-step3c3a`** in all three before Task 1.
- **Run conformance from the `ns-oevm` cwd:** `python3 tools/run_tests_oevm.py`
  (env `OEVM_PATH` defaults to `AB` = both paths). There is **no single-test filter** —
  run the full suite and read the summary line. Baseline now is **59 pass / 0 fail / 3
  skip**; target after this step is **61/0/3** (+`36_readstream`, +`37_writestream`).
- **The runner CLOBBERS `../oevm/src/solidified_image.c` and `…/.stmap`.** Before any
  `git` commit/push in `../oevm`, restore them: `git -C ../oevm checkout
  src/solidified_image.c src/solidified_image.stmap` — EXCEPT in Task 2 Step 9 where
  we deliberately regenerate the committed default image.
- **Inside `lib/kernel.ns` NEVER write `Array new: n`** (compiles to a fixed-slot
  `OP_NEW`, not an indexable). Allocate indexables via
  `platform basicNew: Array indexable: n`.
- **Nil-check stream results with `== nil`, never `isNil`** — `String` DNU-crashes on
  `isNil`/`notNil` (the StringVtable fast-path gap, oevm#6); open-ended reads can
  return a `String`.
- **Commit trailer required** on every commit:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **Push only on explicit say-so** (commit freely).
- Conformance test harness facts: the first `(* … *)` comment holds an `expect:` block;
  **blank output lines are filtered** before comparison, so never assert an empty
  line — assert `size`/`= 'x'`/`== nil` instead. Entry class shape:
  `class X packageUsing: manifest = () ( public main: platform args: args = ( … ) ) : ( )`.

---

### Task 1: `platform string:` primitive + `WriteStream`

**Files:**
- Modify: `../oevm/src/dispatch.c` (three edits: PRIM enum, PRIM_NAMES, switch case)
- Modify: `lib/kernel.ns` (add `WriteStream` class + accessor)
- Modify: `docs/NEWSPEAK_DIALECT.md` (add row #12)
- Test: `../ns-tests/04_somns_basic/37_writestream.ns`

**Interfaces:**
- Produces (VM): `platform string: anArray` → a new immutable `String` whose bytes are
  the elements of `anArray` (a Kernel indexable `Array` of byte-SMIs 0..255). Aborts
  if the arg is not an indexable or an element is not an SMI in 0..255. Reachable only
  inside Kernel methods via the injected `platform`.
- Produces (`.ns`): `WriteStream` with class-side `new`; instance `nextPut: aByte`,
  `nextPutAll: aString`, `print: anObject`, `nextPutLine: aString`, `contents`
  (→`String`), `size`, `reset`. Accessor `platform WriteStream`.

- [ ] **Step 1: Write the failing test** — `../ns-tests/04_somns_basic/37_writestream.ns`

```
(*
WriteStream — io.Buffer-style String builder (std-lib 3c-3a).
expect:
hello
5
true
42
3
true
0
true
*)
class WriteStreamTest packageUsing: manifest = () (
public main: platform args: args = (
    | ws |
    ws:: platform WriteStream new.
    ws nextPutAll: 'hel'. ws nextPut: 108. ws nextPut: 111.
    (ws contents) println.            (* hello *)
    (ws contents) size println.       (* 5 *)

    ws:: platform WriteStream new.
    ws nextPut: 195. ws nextPut: 169.
    ((ws contents) = 'é') println.    (* true — bytes of é round-trip *)

    ws:: platform WriteStream new.
    ws print: 42.
    (ws contents) println.            (* 42 *)

    ws:: platform WriteStream new.
    ws nextPutLine: 'ab'.
    (ws size) println.                (* 3 — 'ab' + LF *)
    (((ws contents) at: 3) = 10) println.  (* true — trailing LF *)

    ws reset.
    (ws size) println.                (* 0 *)

    ws:: platform WriteStream new.
    1 to: 100 do: [ :i | ws nextPut: 97 ].
    ((ws contents) size = 100) println.    (* true — OrderedCollection growth *)
)
) : (
)
```

- [ ] **Step 2: Run the suite to verify the new test FAILS**

Run: `python3 tools/run_tests_oevm.py`
Expected: `37_writestream.ns` FAILs (DNU / no `WriteStream` accessor — `platform
WriteStream` is unknown). The other 59 still pass.

- [ ] **Step 3: Add the `PRIM_pf_string` enum id** in `../oevm/src/dispatch.c`

Find:
```c
    /* --- std-lib step 3b: capability-pure indexable allocation --- */
    PRIM_pf_basicNewIndexable,
};
```
Replace with:
```c
    /* --- std-lib step 3b: capability-pure indexable allocation --- */
    PRIM_pf_basicNewIndexable,
    /* --- std-lib step 3c-3a: byte-Array -> String constructor --- */
    PRIM_pf_string,
};
```

- [ ] **Step 4: Add the selector name** (keep parallel to the enum) in `dispatch.c`

Find:
```c
    "object:identicalTo:", "objHashcode:", "print:", "printNewline:",
    "basicNew:indexable:",
};
```
Replace with:
```c
    "object:identicalTo:", "objHashcode:", "print:", "printNewline:",
    "basicNew:indexable:",
    "string:",
};
```

- [ ] **Step 5: Add the `case PRIM_pf_string`** in the platform-receiver switch in `dispatch.c`

Find:
```c
                    inst->slots[0] = TAG_SMI(n);
                    for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
                    return a;
                }
                default:
                    break;
```
Replace with:
```c
                    inst->slots[0] = TAG_SMI(n);
                    for (intptr_t i = 1; i <= n; i++) inst->slots[i] = NULL;
                    return a;
                }
                case PRIM_pf_string: {
                    /* `platform string: anArray` — build an immutable String from a
                       Kernel indexable Array of byte-SMIs (0..255). The Toit
                       bytes.to-string analog. Byte-granular: no UTF-8 validation
                       (dialect #10/#12). GC is non-moving (cf. PRIM_comma), so reading
                       `ix` after the single gc_allocate is safe. */
                    extern oop gc_allocate(oop vtable, size_t size);
                    oop arr = args[0];
                    if (arr == NULL || IS_SMI(arr) ||
                        !IS_SMI(instance_slot((struct ObjectInstance *)arr, 0))) {
                        fprintf(stderr, "VM Error: platform string: expects an indexable Array\n");
                        abort();
                    }
                    struct ObjectInstance *ix = (struct ObjectInstance *)arr;
                    intptr_t n = UNTAG_SMI(instance_slot(ix, 0));
                    oop s = gc_allocate(StringVtable, sizeof(struct String) + (size_t)n);
                    ((struct String *)s)->length = (size_t)n;
                    for (intptr_t i = 1; i <= n; i++) {
                        oop e = instance_slot(ix, (size_t)i);
                        if (!IS_SMI(e)) {
                            fprintf(stderr, "VM Error: platform string: element %ld is not a byte SMI\n", (long)i);
                            abort();
                        }
                        intptr_t b = UNTAG_SMI(e);
                        if (b < 0 || b > 255) {
                            fprintf(stderr, "VM Error: platform string: byte %ld out of range 0..255\n", (long)b);
                            abort();
                        }
                        ((struct String *)s)->bytes[i - 1] = (uint8_t)b;
                    }
                    return s;
                }
                default:
                    break;
```

- [ ] **Step 6: Add the `WriteStream` class** in `lib/kernel.ns`

Find (the accessor block start):
```
  public Object            = ( ^ Object )
```
Insert immediately BEFORE that line:
```
  class WriteStream = ( | bytes ::= OrderedCollection new. | ) (
    (* io.Buffer-style String builder: accumulate bytes, emit one String via
       `platform string:` — the O(n) replacement for O(n^2) `,` chaining. *)
    public nextPut: aByte = ( (aByte < 0 or: [ aByte > 255 ]) ifTrue: [ ^ self error: 'byte out of range 0..255' ]. bytes add: aByte. ^ aByte )
    public nextPutAll: aString = ( 1 to: aString size do: [ :i | bytes add: (aString at: i) ]. ^ aString )
    public print: anObject = ( ^ self nextPutAll: anObject asString )
    public nextPutLine: aString = ( self nextPutAll: aString. self nextPut: 10. ^ aString )
    public contents = ( ^ platform string: bytes asArray )
    public size = ( ^ bytes size )
    public reset = ( bytes:: OrderedCollection new. ^ self )
  ) : (
  )

```

- [ ] **Step 7: Add the `WriteStream` accessor** in `lib/kernel.ns`

Find:
```
  public Dictionary        = ( ^ Dictionary )
```
Replace with:
```
  public Dictionary        = ( ^ Dictionary )
  public WriteStream       = ( ^ WriteStream )
```

- [ ] **Step 8: Add dialect row #12** in `docs/NEWSPEAK_DIALECT.md`

Find:
```
| 11 | **`Dictionary`/`Set` are open-addressing `HashedCollection`s with a reduced protocol** (std-lib step 3c-2) | `Dictionary` stores `Association`s; `Set`/`Dictionary` carry rich protocol; `select:` answers same-species. | Open-addressing tables over a `platform`-allocated `Array` (linear probe, load factor 0.75, cluster-rehash deletion). **No `Association` class** (`associationsDo:`/`associations` absent — keys and values live in parallel arrays, no per-entry object); **`nil` is forbidden as a key/element** (it is the empty-slot sentinel — insertion raises `error:`); `Dictionary>>do:` iterates **values**; inherited `collect:`/`select:` answer an `OrderedCollection`, not a `Dictionary`; `keys`/`values` answer an `Array`. | Parallel arrays + open addressing avoid per-entry allocation/fragmentation at embedded scope, and the reduced protocol is the YAGNI subset st-zephyr scripting needs. | A real `Association` / same-species `select:` / nil-key need. |
```
Insert immediately AFTER that line:
```
| 12 | **Streams are byte-granular, String-focused `ReadStream`/`WriteStream`** (std-lib step 3c-3a) | Streams are generic over any collection; a `Stream` hierarchy with rich positionable protocol; reads/writes are element-granular. | `ReadStream`/`WriteStream` operate only on the immutable UTF-8 `String` (subclass `Object`, not `Collection`). `next`/`peek` answer a **byte** SMI (0..255); substring reads (`next:`/`upTo:`/`nextLine`/`upToEnd`) and `WriteStream>>contents` build a `String` via the new **`platform string:`** byte-Array→String primitive (the only path from bytes back to a `String`). Fixed-count reads (`next`/`next:`) **`error:`** when short; open-ended reads (`upTo:`/`nextLine`/`upToEnd`) answer **`nil` at EOF** (so a blank line `''` stays distinct from end-of-stream); `nextLine` trims a trailing `\r\n`/`\n`. Delimiter reads are UTF-8-safe iff the delimiter is ASCII (<128). No generic-collection streaming. | Serial parsing + string building are the embedded use cases; byte-granularity matches #10 and the immutable-`String` substrate. Toit `io.Reader`/`io.Buffer` validate the byte-oriented, null-at-EOF, terminator-trimming semantics. | A generic-collection streaming need, or rune-granular text (issue #1). Reversible by giving the indexable an `outer` header / wrapper repr as in #3. |
```

- [ ] **Step 9: Run the suite to verify it PASSES on both paths**

Run: `python3 tools/run_tests_oevm.py`
Expected: `37_writestream.ns` PASS; summary **60 pass / 0 fail / 3 skip** (baseline +1).
If a FAIL prints `A:… B:…`, both paths must be green before proceeding.

- [ ] **Step 10: Restore the clobbered image, then commit per-repo**

```bash
git -C ../oevm checkout src/solidified_image.c src/solidified_image.stmap

git -C ../oevm add src/dispatch.c
git -C ../oevm commit -m "feat(stdlib): platform string: byte-Array->String prim (3c-3a task 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

git -C ../ns-tests add 04_somns_basic/37_writestream.ns
git -C ../ns-tests commit -m "test(stdlib): WriteStream String builder (3c-3a task 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

git add lib/kernel.ns docs/NEWSPEAK_DIALECT.md
git commit -m "feat(stdlib): WriteStream + dialect #12 (3c-3a task 1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `ReadStream`

**Files:**
- Modify: `lib/kernel.ns` (add `ReadStream` class + accessor)
- Test: `../ns-tests/04_somns_basic/36_readstream.ns`
- Modify: `../oevm/src/solidified_image.c` + `.stmap` (regenerate committed default)

**Interfaces:**
- Consumes: `platform string:` (Task 1), `platform basicNew: Array indexable:`,
  `OrderedCollection`, `WriteStream` (Task 1, used by the test to build inputs
  containing control bytes).
- Produces (`.ns`): `ReadStream` with class-side `on: aString`; instance `next`,
  `peek`, `atEnd`, `skip: n`, `next: n` (→`String`), `upTo: aByte` (→`String`/`nil`),
  `nextLine` (→`String`/`nil`), `upToEnd` (→`String`/`nil`), `position`, `reset`,
  `do: aBlock`. Accessor `platform ReadStream`.

- [ ] **Step 1: Write the failing test** — `../ns-tests/04_somns_basic/36_readstream.ns`

```
(*
ReadStream — byte-granular String cursor (std-lib 3c-3a).
Uses WriteStream (task 1) to build inputs containing control bytes (LF=10, CR=13).
expect:
a
b
88
88
89
true
99
hel
true
x
true
0
true
true
true
*)
class ReadStreamTest packageUsing: manifest = () (
public main: platform args: args = (
    | rs ws mb crlf nl |
    rs:: platform ReadStream on: 'a,b'.
    (rs upTo: 44) println.            (* a  — comma=44, consumed *)
    (rs upToEnd) println.             (* b *)

    rs:: platform ReadStream on: 'XY'.
    (rs peek) println.                (* 88 — 'X' *)
    (rs next) println.                (* 88 *)
    (rs next) println.                (* 89 — 'Y' *)
    (rs atEnd) println.               (* true *)

    rs:: platform ReadStream on: 'abcd'.
    rs skip: 2.
    (rs next) println.                (* 99 — 'c' *)

    rs:: platform ReadStream on: 'hello'.
    (rs next: 3) println.             (* hel *)

    (* UTF-8 safety: ASCII delimiter never splits a multi-byte char.
       build 'é,x' = bytes 195 169 44 120 *)
    ws:: platform WriteStream new.
    ws nextPut: 195. ws nextPut: 169. ws nextPut: 44. ws nextPut: 120.
    mb:: ws contents.
    rs:: platform ReadStream on: mb.
    ((rs upTo: 44) = 'é') println.    (* true — 'é' intact across the comma split *)
    (rs upToEnd) println.             (* x *)

    (* nil-at-EOF vs blank line: build 'a' LF LF 'b' = bytes 97 10 10 98 *)
    ws:: platform WriteStream new.
    ws nextPut: 97. ws nextPut: 10. ws nextPut: 10. ws nextPut: 98.
    nl:: ws contents.
    rs:: platform ReadStream on: nl.
    ((rs nextLine) = 'a') println.    (* true *)
    (rs nextLine) size println.       (* 0 — blank line, NOT EOF *)
    ((rs nextLine) = 'b') println.    (* true *)
    ((rs nextLine) == nil) println.   (* true — EOF (use == nil, not isNil) *)

    (* CR strip: build 'x' CR LF = bytes 120 13 10 *)
    ws:: platform WriteStream new.
    ws nextPut: 120. ws nextPut: 13. ws nextPut: 10.
    crlf:: ws contents.
    rs:: platform ReadStream on: crlf.
    ((rs nextLine) = 'x') println.    (* true — trailing CR stripped *)
)
) : (
)
```

- [ ] **Step 2: Run the suite to verify the new test FAILS**

Run: `python3 tools/run_tests_oevm.py`
Expected: `36_readstream.ns` FAILs (`platform ReadStream` unknown). 60 others pass.

- [ ] **Step 3: Add the `ReadStream` class** in `lib/kernel.ns`

Find (the `WriteStream` class added in Task 1):
```
  class WriteStream = ( | bytes ::= OrderedCollection new. | ) (
```
Insert immediately BEFORE that line:
```
  class ReadStream = ( | source position | ) (
    (* Byte-granular cursor over an immutable String. `next`/`peek` answer a byte
       SMI; substring reads build Strings via `platform string:`. Open-ended reads
       answer nil at EOF so a blank line ('') stays distinct from end-of-stream. *)
    public next = ( self atEnd ifTrue: [ ^ self error: 'next past end' ]. position:: position + 1. ^ source at: position )
    public peek = ( self atEnd ifTrue: [ ^ self error: 'peek past end' ]. ^ source at: position + 1 )
    public atEnd = ( ^ position >= source size )
    public skip: n = ( position:: position + n. ^ self )
    public position = ( ^ position )
    public reset = ( position:: 0. ^ self )
    public next: n = ( | a |
      (position + n) > source size ifTrue: [ ^ self error: 'next: beyond end' ].
      a:: platform basicNew: Array indexable: n.
      1 to: n do: [ :i | a at: i put: (source at: position + i) ].
      position:: position + n.
      ^ platform string: a )
    public upTo: aByte = ( | r b |
      self atEnd ifTrue: [ ^ nil ].
      r:: OrderedCollection new.
      [ self atEnd ] whileFalse: [
        b:: self next.
        b = aByte ifTrue: [ ^ platform string: r asArray ].
        r add: b ].
      ^ platform string: r asArray )
    public nextLine = ( | s n |
      self atEnd ifTrue: [ ^ nil ].
      s:: self upTo: 10.
      n:: s size.
      (n > 0 and: [ (s at: n) = 13 ]) ifTrue: [ ^ self prefixOf: s upTo: n - 1 ].
      ^ s )
    public upToEnd = ( | r |
      self atEnd ifTrue: [ ^ nil ].
      r:: OrderedCollection new.
      [ self atEnd ] whileFalse: [ r add: self next ].
      ^ platform string: r asArray )
    public do: aBlock = ( [ self atEnd ] whileFalse: [ aBlock value: self next ]. ^ self )
    prefixOf: s upTo: k = ( | a | a:: platform basicNew: Array indexable: k. 1 to: k do: [ :i | a at: i put: (s at: i) ]. ^ platform string: a )
    setSource: aString = ( source:: aString. position:: 0. ^ self )
  ) : (
    public on: aString = ( ^ self new setSource: aString )
  )

```

- [ ] **Step 4: Add the `ReadStream` accessor** in `lib/kernel.ns`

Find (added in Task 1):
```
  public Dictionary        = ( ^ Dictionary )
  public WriteStream       = ( ^ WriteStream )
```
Replace with:
```
  public Dictionary        = ( ^ Dictionary )
  public ReadStream        = ( ^ ReadStream )
  public WriteStream       = ( ^ WriteStream )
```

- [ ] **Step 5: Run the suite to verify it PASSES on both paths**

Run: `python3 tools/run_tests_oevm.py`
Expected: `36_readstream.ns` PASS; summary **61 pass / 0 fail / 3 skip**. Both paths
green (no `A:… B:…` mismatch line).

- [ ] **Step 6: Regenerate the committed default solidified image — MUST be last, after the verifying run**

The runner clobbers `solidified_image.c`/`.stmap` on every test, so do this AFTER
Step 5 and do **not** run the suite again afterward (it would overwrite the default).
`ns_compile_oevm.py` writes BOTH the `.c` and its `.stmap` sidecar in one call:

```bash
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/36_readstream.ns ../oevm/src/solidified_image.c
```

This rewrites `../oevm/src/solidified_image.c` + `../oevm/src/solidified_image.stmap`
from the ReadStream test (a sample exercising the full 3c-3a kernel). This is the exact
same Path A image the runner just built and passed in Step 5, so it is already
validated — no re-run needed (and a re-run would clobber it).

- [ ] **Step 7: Commit per-repo**

```bash
git -C ../ns-tests add 04_somns_basic/36_readstream.ns
git -C ../ns-tests commit -m "test(stdlib): ReadStream String cursor (3c-3a task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

git -C ../oevm add src/solidified_image.c src/solidified_image.stmap
git -C ../oevm commit -m "chore(stdlib): refresh default image to 3c-3a kernel (3c-3a task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

git add lib/kernel.ns
git commit -m "feat(stdlib): ReadStream (3c-3a task 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

Sanity-check the working tree before the `../oevm` commit: `git -C ../oevm diff --stat`
should show only `src/solidified_image.c` + `.stmap`, and `src/dispatch.c` should be
unchanged here (it was committed in Task 1).

---

## Done criteria

- `python3 tools/run_tests_oevm.py` → **61 pass / 0 fail / 3 skip** on **both** paths.
- `ReadStream`/`WriteStream` reachable via `platform ReadStream` / `platform
  WriteStream`; `platform string:` builds Strings byte-granularly on both paths.
- Dialect row #12 recorded.
- All three repos committed (per-repo), branches `feat/stdlib-step3c3a`; **not** pushed
  until David says so. Final whole-branch opus review before merge (per the established
  SDD close-out).

## Out of scope (this cycle)

Serial (3c-3b, the native-capability tier) · generic-collection streams · `from:to:`
substring fast path · `ReadStream>>contents`/`peek:`/seeks · `ReadWriteStream`. See
spec §9.
