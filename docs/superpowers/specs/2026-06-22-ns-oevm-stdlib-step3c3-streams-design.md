# ns-oevm std-lib step 3c-3a — Streams (ReadStream + WriteStream)

**Status:** ratified design (brainstorm w/ David, 2026-06-22). Next: writing-plans → SDD.
**Predecessor:** 3c-2 (Dictionary + Set), merged + pushed.
**Successor:** 3c-3b (Serial — the first real *native* capability tier), a separate
spec → plan → SDD cycle after this one.
**Specs cross-ref:** `2026-06-20-ns-oevm-stdlib-design.md` (capability spine; §5 stream
scope), `2026-06-21-ns-oevm-stdlib-step3c1-string-booleans.md` (immutable UTF-8 String
substrate this builds on), `docs/NEWSPEAK_DIALECT.md` (divergence governance).

---

## 0. Scope decision (ratified 2026-06-22)

The original "3c-3 = Streams + Serial" item is **split**. This cycle (**3c-3a**)
delivers **Streams only**. **Serial** (3c-3b) introduces an entirely new mechanism —
the per-target native-capability tier with its own grant policy and a host mock — and
gets its own brainstorm → spec → plan → SDD cycle afterward. Rationale: Streams are
near-pure-source (one small primitive); Serial is a new capability tier. Keeping them
separate matches the prior step cadence and keeps each plan single-purpose.

Generic-collection streaming (SOMns/Smalltalk streams are generic over any
collection) is **deferred (reversible)** — 0 current suite files need it (same
YAGNI call as Option C / Array-not-Collection in 3b). This cycle is **String-focused**.

---

## 1. Goal

Add `ReadStream` and `WriteStream` to the Kernel library, calibrated to st-zephyr's
real use cases: **parsing input** (serial lines → Strings) and an **efficient String
builder** (the Toit `io.Buffer` analog — accumulate, emit one `String`, replacing
O(n²) `,`-chaining). Both are byte-granular streams over the immutable UTF-8 `String`
shipped in 3c-1.

---

## 2. Footprint: near-pure-source + ONE platform primitive

3c-2 was pure source. 3c-3a is pure source **plus a single new capability
primitive** on the platform-object dispatch path:

- **`platform string: anArray`** → a new immutable `String` whose bytes are the
  elements of `anArray` (a Kernel `Array` of byte-SMIs 0–255). This is Toit's
  `bytes.to-string`. Without it there is **no path in pure source from bytes back to a
  `String`** (the only String-producing ops today are literals, `,`-concat, and
  `printString`/`asString`) — so String-returning reads and the String builder are
  impossible without it.

Everything else is new `.ns` source in `lib/kernel.ns` (`ReadStream`, `WriteStream`,
two accessors) + tests + a dialect row.

### 2.1 Why the primitive is cheap (not a 3c-1-scale change)

`platform string:` is added to `dispatch.c`'s platform-receiver interception block
(the same place that handles `basicNew:indexable:`, `array:`, `print:`, …). Both
image paths — Path A (solidified C) and Path B (`.oimg`) — share the VM `dispatch.c`
binary and reach the platform object by receiver identity, so the primitive is
available on **both paths with no `.oimg` change, no `format_version` bump, and no
builtin-registry version bump** (those govern the *vtable/class* registry, not
platform selectors). Contrast 3c-1, which added a whole `StringVtable` builtin type
(registry 3→4, dump_layout, oimg records). This is one selector case.

### 2.2 Semantics of `platform string: anArray`

- `anArray` is a Kernel indexable (`slot0 = TAG_SMI(length)`, elems 1..n), as produced
  by `platform basicNew: Array indexable: n` or `OrderedCollection>>asArray`.
- Allocates a `String` of `length` bytes (via the existing `make_string`-style
  `gc_allocate(StringVtable, …)`), copying each element's low byte.
- **Validation:** each element must be an SMI in 0–255; otherwise `VM Error … abort`
  (consistent with the existing `String at:` bounds-abort style). No UTF-8 / rune
  validation — byte-granular, per dialect #10.
- **GC:** allocates a `String` (already a GC leaf in `trace_one` since 3c-1) and reads
  an ordinary indexable. **No new GC roots.** The argument Array is reachable on the
  C stack as `args[0]` across the single `gc_allocate` call.

---

## 3. Runes / UTF-8 (carries the 3c-1 decision forward)

Streams are **byte-granular and rune-unaware**, consistent with dialect #10
(byte-granularity permanent; runes deferred → GitHub issue #1). Concretely:

- `next` / `peek` return a byte SMI (0–255), exactly like `String at:` — *not* a rune.
- `platform string:` copies bytes verbatim: valid-UTF-8 bytes in → valid-UTF-8 String
  out; malformed bytes in → malformed String out (same contract as `String` itself).
- **Safety property (documented):** delimiter reads (`upTo:`, `nextLine`) are
  UTF-8-safe **whenever the delimiter byte is ASCII (< 128)**, because every byte of a
  UTF-8 multi-byte sequence is ≥ 0x80 — so an ASCII delimiter can never match *inside*
  a character, and chunk boundaries always fall on character boundaries. Newline,
  comma, space, tab, etc. are ASCII → safe by construction.
- `next: n` (byte-count read) *may* split a multi-byte character — documented
  byte-granular behavior (same as the user computing `String at:`), not a regression.

---

## 4. Validation against Toit `io` (the embedded reference, as for 3c-1)

Toit strings are immutable UTF-8 (we mirrored this in 3c-1); Toit separates raw bytes
(`ByteArray`) from text (`string`). We have no `ByteArray` — our `Array` of byte-SMIs
plays that role. The design is a Smalltalk-named subset of Toit's byte-oriented
`io.Reader` + `io.Buffer`:

| Our protocol (Smalltalk names) | Toit `io` equivalent | Returns |
|---|---|---|
| `ReadStream>>next` | `Reader>>read-byte` | byte SMI |
| `peek` | `Reader>>peek-byte 0` | byte SMI |
| `skip: n` | `Reader>>skip n` | — |
| `next: n` | `Reader>>read-string n` | **String** |
| `upTo: aByte` / `nextLine` | `Reader>>read-line` / `skip-up-to` | **String** |
| `upToEnd` | `Reader>>read-all` / `read-string --max-size` | **String** |
| `WriteStream>>nextPut: aByte` | `Buffer>>write-byte` | — |
| `nextPutAll: aString` | `Buffer>>write` | — |
| `contents` | `Buffer>>to-string` | **String** |

Decisive validation: Toit's `Buffer>>to-string` *is* `bytes.to-string` — it accumulates
into a backing byte array and converts the written slice to a `string` at the end.
That is exactly our `platform string: (bytes asArray)`. Toit's `read-byte`/`peek-byte`
return `int` (a byte) — exactly our `next`/`peek` byte SMI.

Source: `toitlang/toit` `lib/io/reader.toit`, `lib/io/buffer.toit` (verified
2026-06-22).

---

## 5. Class design (new source in `lib/kernel.ns`)

Both classes subclass `Object` (not `Collection`): they are a cursor and a builder,
with their own protocol. Both are nested in `Kernel usingPlatform: platform`, so
instance methods reach `platform` lexically through their outer chain — already proven
by `OrderedCollection` (which calls `platform basicNew: Array indexable:`).

### 5.1 `ReadStream` — cursor over an immutable `String`

Slots: `source` (a `String`), `position` (SMI, 0-based count of bytes consumed).

Class-side: `on: aString` → a new instance with that source, position 0.

| Method | Behavior | Returns |
|---|---|---|
| `next` | byte at `position+1`, advance; `atEnd` → `self error: 'at end'` | byte SMI |
| `peek` | byte at `position+1`, no advance; `atEnd` → error | byte SMI |
| `atEnd` | `position >= source size` | boolean |
| `skip: n` | `position:: position + n` | self |
| `next: n` | next `n` bytes as a String; **errors** if < n available | **String** |
| `upTo: aByte` | bytes up to (excluding) `aByte`, **consuming** it; rest if delimiter absent; **`nil` at EOF** | String or nil |
| `nextLine` | one line **without its terminator** (trailing `\r\n`/`\n` trimmed, `\n` consumed); **`nil` at EOF** | String or nil |
| `upToEnd` | all remaining bytes; **`nil` at EOF** | String or nil |
| `position` | current position | SMI |
| `reset` | `position:: 0` | self |
| `do: aBlock` | `aBlock value:` each remaining byte SMI | self |

String-returning reads collect bytes into a local `OrderedCollection`, then
`platform string: coll asArray`. (`next: n` knows its length and may allocate the
Array directly — an optional micro-optimization, not required.)

**End-of-stream convention (validated against Toit `io.Reader`, 2026-06-22).** Two
deliberate behaviors, matching Toit exactly:

- **Fixed-count reads** (`next`, `peek`, `next: n`) *demand* the bytes: short → `self
  error:`. (Toit `read-byte`/`read-string n`/`read-bytes n` throw
  `UNEXPECTED-END-OF-READER` — "at least n bytes must be available". Guard with
  `atEnd` / `position`.)
- **Open-ended reads** (`upTo:`, `nextLine`, `upToEnd`) return **`nil` at EOF**, never
  `''`. (Toit `read-line`/`read-all`/`read-string --max-size` return `null` at end.)
  This is a **correctness** requirement, not a style choice: a present-but-empty result
  — e.g. a *blank line* — is the valid String `''`, which must remain distinguishable
  from end-of-stream. Returning `''` for EOF would conflate the two.

`nextLine` **strips the line terminator** — it returns the line without a trailing
`\r\n` or `\n` (a `\r` is removed even though we are not on Windows), matching Toit
`read-line` and the common understanding of read-line; the `\n` is consumed. So
`nextLine` is *not* a bare `upTo: 10`: read up to `\n` (10), drop a trailing `\r` (13)
from the collected bytes, consume the `\n`.

**Nil-check idiom (3c-2 VM gap carry-forward):** an open-ended read may return a
`String`, and `String` DNU-crashes on `isNil`/`notNil` (the StringVtable fast-path gap,
oevm#6). So consumers test a read result with **`== nil`**, never `isNil` — e.g.
`line:: s nextLine. [line == nil] whileFalse: [ … line:: s nextLine ]`. Tests follow
this.

### 5.2 `WriteStream` — the `io.Buffer` String builder

Slot: `bytes` (an `OrderedCollection` of byte-SMIs — reuses 3b's growable backing; no
new growth logic).

Class-side: `new` → empty builder. (The default `OrderedCollection`-backed `new`
suffices; an explicit class-side `new` may be added if the inherited form does not
initialize the slot — verify in planning, cf. the 3c-2 bare-`= Super` slot-init note.)

| Method | Behavior | Returns |
|---|---|---|
| `nextPut: aByte` | append byte SMI; not 0–255 → `self error:` | aByte |
| `nextPutAll: aString` | append each byte (`1 to: aString size do: [...]`) | aString |
| `print: anObject` | `self nextPutAll: anObject printString` (`printString`, **not** `asString`: `asString` answers a `Symbol` for non-`String` receivers, which DNUs in `nextPutAll:`; `printString` answers a `String` and is the Smalltalk-canonical behavior for `print:`) | anObject |
| `nextPutLine: aString` | `nextPutAll:` then `nextPut: 10` | aString |
| `contents` | `platform string: bytes asArray` | **String** |
| `size` | `bytes size` | SMI |
| `reset` | empty the buffer | self |

This is the **O(n) replacement for O(n²) `,`-chaining**: write pieces, emit one
`String` at the end.

### 5.3 Accessors (Kernel bottom, alongside `Object`/`Array`/…/`Dictionary`)

```
public ReadStream  = ( ^ ReadStream )
public WriteStream = ( ^ WriteStream )
```

Apps: `platform ReadStream on: aString`, `platform WriteStream new` — parallels the
existing `platform Array new:` / `platform OrderedCollection new` pattern. Class-side
`on:` is reached by sending to the class object returned by the accessor.

---

## 6. Reachability (consistent with 3b spec §3)

- **stream method → platform primitive** (`platform string:`, `platform basicNew:`):
  lexically via the injected `platform`, through the instance's outer chain to Kernel.
  Proven by `OrderedCollection`.
- **stream method → other Kernel class** (`OrderedCollection`, `Array`): compile-time
  lexical sibling slot read / `OP_NEW`.
- **app → stream class**: runtime send to the `platform ReadStream` / `platform
  WriteStream` accessor, then a class-side / instance send.

No new reachability mechanism is introduced.

---

## 7. Testing (bar: green on `oevm_runner`, BOTH image paths)

- **Regression net:** the full `ns-tests` suite (currently 59/0/3) stays green on Path
  A and Path B.
- **`36_readstream.ns`** — parse a String: `on:`, `next`, `peek`, `atEnd`, `skip:`,
  `next:`, `upTo:`, `nextLine`, `upToEnd`, `reset`, `do:`. Plus the three
  Toit-validated semantics: (a) **UTF-8 safety** — a String with a multi-byte
  character split on an ASCII delimiter (e.g. comma) yields substrings with the
  character intact; (b) **nil-at-EOF vs blank line** — a source like `'a\n\nb'` read
  by `nextLine` yields `'a'`, `''` (blank line, *not* EOF), `'b'`, then **`nil`**
  (using `== nil`); (c) **CR strip** — `nextLine` on `'x\r\n'` returns `'x'` (the `\r`
  removed).
- **`37_writestream.ns`** — build a String: `nextPut:`, `nextPutAll:`, `print:`,
  `nextPutLine:`, `contents` equals a literal; a **multi-byte round-trip** (assemble
  the UTF-8 bytes of e.g. `'é'` via `nextPut:`, assert `contents = 'é'`) proving
  `platform string:` is correct byte-granular construction; a longer build to exercise
  `OrderedCollection` growth.
- Regenerate the committed default `solidified_image.c` if the chosen default sample
  `.ns` changes (per the established per-step practice).

**Verify-first items for the planning phase** (the SDD controller empirically confirms
these before writing tasks, per our pattern — locate edit sites by quoted code):
1. exact platform-selector dispatch site in `dispatch.c` (where `basicNew:indexable:`
   et al. are matched) so `string:` slots in identically;
2. reading a Kernel `Array`'s length/elements from C (reuse the `basicAt:`/`basicSize`
   internal access);
3. `platform` reachable inside a `ReadStream`/`WriteStream` *instance* method (expected
   — `OrderedCollection` proves it; confirm with a throwaway `.ns`);
4. class-side `on:` reachable via `platform ReadStream on:`; and whether `WriteStream`
   needs an explicit class-side `new` to initialize its `bytes` slot (the 3c-2
   bare-`= Super` slot-init caveat).

---

## 8. Dialect governance (`docs/NEWSPEAK_DIALECT.md`)

Append **row #12**: *Streams are byte-granular and String-focused.*
`ReadStream`/`WriteStream` operate on the immutable UTF-8 `String`; `next`/`peek`
return byte SMIs; substring reads build Strings via the new `platform string:`
byte→String constructor. Generic-collection streaming is **deferred (reversible)**.
Delimiter reads (`upTo:`/`nextLine`) are UTF-8-safe iff the delimiter is ASCII (< 128).
EOF/line conventions follow Toit `io.Reader`: fixed-count reads (`next`/`next:`) error
when short; open-ended reads (`upTo:`/`nextLine`/`upToEnd`) return `nil` at EOF (so a
blank line `''` stays distinct from end-of-stream); `nextLine` trims a trailing
`\r\n`/`\n`. New `platform string:` platform primitive recorded. Revisit trigger: a
sample needs generic-collection streaming, or rune-granular text handling (issue #1).

---

## 9. Deferred / out of scope (reversible)

- **Serial** (3c-3b) — the native-capability tier; its own cycle.
- **Generic-collection streams** (over `Array`/`OrderedCollection`) — YAGNI now.
- **`ReadStream>>contents`** (whole-source), `peek:`, `match:`, positionable seeks
  beyond `reset`/`skip:` — add when demanded.
- **`from:to:` substring fast path** for `platform string:` (avoid the intermediate
  Array on `ReadStream` substring reads) — micro-optimization, add if profiling warrants.
- **`ReadWriteStream`** — not planned.
