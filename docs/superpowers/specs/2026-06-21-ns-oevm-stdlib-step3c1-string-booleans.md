# ns-oevm std-lib step 3c-1 — String + the universal value protocol (hash / = / printString), incl. booleans

> **Status:** design ratified 2026-06-21 (brainstorm with David). Next: writing-plans → SDD.
> **Predecessor:** [step 3b](2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md) (Kernel module + Collection/OrderedCollection). **Parent design:** [stdlib capability design](2026-06-20-ns-oevm-stdlib-design.md).
> **Tracking:** GitHub issue [#1](https://github.com/davidg238/ns-oevm/issues/1) (deferred rune protocol).

## 1. Goal & scope

The kickoff "3c" bundle (`String`, `Dictionary`, `Set`, boolean `hash`/`=`/`printString`, Streams + `Serial`) is **decomposed** — it is larger than 3a or 3b as one spec. This spec is **3c-1**, the foundation the rest build on:

- **In scope:** a real **`String`** type (distinct, immutable, packed **UTF-8** bytes) replacing the "string literal == interned Symbol" status quo; and a **completed universal value protocol** — `printString` returns a real `String` for *every* value, a brand-new `hash` message across all types, and `String`'s content-based `=`/`hash`. **Booleans** fall out of this protocol (closing the gap in dialect #5).
- **Deferred to later sub-steps:** **3c-2** = `Dictionary` + `Set` (consume this step's `hash`/`=`); **3c-3** = Streams (`ReadStream`/`WriteStream`) + the `Serial` native capability (consume `String`).
- **Deferred within String (issue #1):** the rune (Unicode code-point) protocol — `runeAt:`/`runesDo:`/`runeSize`/`fromRune:`. 3c-1 is **byte-granularity only**.
- **Explicitly NOT in 3c-1 (YAGNI, no suite use):** String ordering (`<`/`<=`/`>`/`>=`), `copyFrom:to:`/substring, `asSymbol`. Add when a consumer appears.

The bar is unchanged: **green on `oevm_runner`, both image paths (Path A solidified-C, Path B `.oimg`)** — not "parses."

## 2. ARCHITECTURAL HIGHLIGHT — String is a *bare, immutable, non-interned* UTF-8 byte indexable

**This is the load-bearing decision of 3c-1; read it first.** Every downstream choice (not-a-Collection, `,`-as-prim, content-hash, build-via-WriteStream) follows from it.

> **A `String` is the thin primitive byte indexable — `slot0 = byte length`, then packed UTF-8 bytes, with NO `outer`→Kernel link — exactly mirroring `Array` (Option C, dialect #3). It is immutable to all public callers and NOT interned.**

Three properties, each with a consequence:

1. **Bare indexable (no `outer`→Kernel link).** Like `Array`, a `String` instance cannot reach `platform` from an instance method, so:
   - **`String` subclasses `Object`, NOT `Collection`** — inherited `Collection` folds that allocate siblings (`collect:`) or reach `platform` cannot run on a `String` receiver. `aString collect:` is a `doesNotUnderstand`, same as `anArray collect:`. (Dialect #3 generalizes: *Array **and** String*.)
   - **String construction is native-prim-mediated**, not platform-threaded: literals (static emission), `,` (the existing `PRIM_comma`, now producing a `String`), and `printString` (the existing prim, now producing a `String`). This is **unchanged capability posture** — `,`/`printString` were already prims in 3a/3b (they produced Symbols); 3c-1 only changes *what they allocate*. The platform-threaded byte allocator (`platform basicNew: String bytes: n`) is a **3c-3** concern (WriteStream); 3c-1 needs none of it.

2. **Immutable (public).** `String` publishes no `at:put:`. Immutability is a *public-protocol policy*, not separate machinery — the same packed-byte shape is what 3c-3's `WriteStream` will mutate internally before freezing a `String` via `contents`. (No mutation surface ships in 3c-1.)

3. **Non-interned.** Interning is right only for a **bounded** set (Symbols: selectors, mostly compile-time) — identity `=`/`hash` is the payoff. **Strings are unbounded user data** (every `,`, every parsed serial line); interning them means a global, ever-growing intern table (a GC root, churn, fragmentation) — the exact embedded failure mode we avoid. So two equal-content strings are **distinct objects** ⇒ `==` (identity) is false ⇒ **`String` needs content-based `=` and `hash`** (equal content must hash equal, or Dictionary lookups miss). Toit makes the same call (strings are not interned). The interning decision *drives* the hash strategy: **Symbol interned → identity hash; String non-interned → content hash.**

This is consistent with 3b's static-bound, capability-pure substrate: the bare-indexable repr is *why* both Array and String sit outside `Collection`, and *why* their construction is native rather than capability-threaded.

## 3. The `String` representation & VM additions

### 3.1 Heap layout (a new *leaf* object shape)

```
struct String {            // ../oevm/src/oevm.h, beside struct Symbol (:47)
    struct Object base;    // vtable = StringVtable
    size_t length;         // byte length
    // uint8_t bytes[length] inline  (flexible payload; one allocation)
};
```

Allocated `gc_allocate(StringVtable, sizeof(struct String) + length)` — bytes **inline** in the GC arena (unlike today's `PRIM_comma`, which `malloc`s a separate non-GC'd `name` buffer for its result Symbol — a wart this avoids for String).

### 3.2 GC — String is a leaf (THE load-bearing VM change)

`trace_object` scans an object's payload **at word granularity** as candidate oops (`../oevm/src/gc.c:367`), with a special-case for **leaf** objects whose payload is raw data, not pointers (`FloatVtable`, `gc.c:411`/`:426`). **`StringVtable` must join that leaf special-case** — otherwise packed bytes that happen to look like live arena addresses would be misread as oops. (Symbols escape this today only because their two non-oop fields — a `malloc` `const char*` and a `size_t` — don't resemble arena payload starts; inline bytes have no such luck.) `StringVtable` is also registered as a GC root like the other builtin vtables (`gc.c:592`-on).

### 3.3 Allocation paths in 3c-1

| Path | Mechanism |
|---|---|
| **Literals** `'abc'` | static `String` constants — see §3.4 |
| **`,` concat** | `PRIM_comma` (`dispatch.c`) — now allocates a `String` (inline bytes) for String⊕String; today it produces a Symbol |
| **`printString`** | `PRIM_printString` (`dispatch.c`) — now allocates a `String` for all numeric/nil/Symbol receivers; today it produces a Symbol |

No library (`.ns`) code allocates strings in 3c-1.

### 3.4 String literals — compiler & both image paths

The AST **already distinguishes** string from symbol literals: `tools/ns_compile_oevm.py:453` is `elif kind == "string" or kind == "symbol":` — both currently `add_constant(str(val))` (→ interned Symbol). 3c-1 **splits** them: `kind == "string"` → a **String** constant; `kind == "symbol"` → a Symbol constant (unchanged). Localized change at the one site.

- **Path A (solidified C):** emit `StaticString_<sanitized>` static structs (StringVtable + length + inline byte literal), mirroring the existing `StaticSymbol_` machinery (`ns_compile_oevm.py:~1001–1017`) and `UPDATE_LIMITS`/root registration so the constants survive GC.
- **Path B (`.oimg`):** add a **String constant kind** to the image constant encoding (the loader is `../oevm/src/oimg_loader.c`; format is `format_version 3` from 3b). David has ratified format bumps freely ("don't need backward compat") → bump to **`format_version 4`** if the constant table needs it. This is the Path-B soft spot (cf. 3a's by-name Array resolution); sequence it explicitly.

## 4. `String` protocol (byte-granularity)

`String` subclasses `Object`. Public protocol — all **byte**-granularity (dialect #10):

| Selector | Behavior | Impl |
|---|---|---|
| `size` | byte length | prim (read `length`) |
| `at: i` | byte at `i` → SMI `0..255` (never nil) | prim |
| `do: aBlock` | yields each byte as a SMI | `.ns` over `size`/`at:` (pure, no platform) — or prim |
| `= other` | **content** equality: `other` is a String with identical bytes | prim (`memcmp`) |
| `hash` | **content** hash, FNV-1a over bytes, masked to the tagged-SMI range | prim |
| `, other` | concat → new `String` | `PRIM_comma` |
| `isEmpty` | `size = 0` | `.ns` |
| `printString` / `asString` | `^ self` (a String prints as its own content) | prim (String receiver → return self) |
| `isNil`/`notNil`/`ifNil:`/`ifNotNil:` | from `Object` | inherited |

`==` stays identity (two equal-content strings are distinct ⇒ `'a','b' == 'ab'` is false; `=` is true). `'abc' = #abc` is **false** (different species); the Symbol's `=` (identity) likewise returns false against a String. **Deferred (issue #1 / YAGNI):** rune protocol, ordering, substring, `asSymbol`.

## 5. The universal value protocol — `printString`, `hash`, `=`

3c-1 completes a coherent value protocol so every value can be displayed and used as a (future) Dictionary key.

### 5.1 `printString` returns a `String` for everything

`PRIM_printString` (`dispatch.c`) today `gc_allocate`s **Symbols** for SMI/Float/LargeInteger/nil, returns the receiver for Symbols, and falls through to `Object.ns` for heap objects. 3c-1 changes it to allocate **Strings**, and fixes the Symbol/boolean cases:

| Receiver | `printString` |
|---|---|
| SMI / Float / LargeInteger | `String` of the numeral |
| nil | `String` `'nil'` |
| **boolean** (`true`/`false` singletons) | `String` `'true'` / `'false'` — **special-cased** (pointer check; the boolean test already exists at `dispatch.c:212`) |
| Symbol (non-boolean) | `String` with a **`#` prefix** (`#foo printString` → `'#foo'`) |
| String | `^ self` |
| heap object | fall through to `Kernel.Object>>printString` (unchanged) |

**Why the `#` prefix for genuine Symbols:** without it, `#foo printString` and `'foo' printString` would render *identically* yet `#foo = 'foo'` is false — a "looks-equal-isn't" footgun. The prefix is exactly why Smalltalk does this; we keep it, and special-case the two boolean singletons so they read `'true'`/`'false'`. (Residual, acceptable, same as Smalltalk: a String whose *content* is `"#foo"` prints like `#foo printString`.) `asString` stays `= printString` uniformly in 3c-1; a no-`#` `displayString` is a later nicety.

### 5.2 `hash` — a new message, identity-vs-content by interning

There is **no `hash` message today** (only `platform objHashcode:`, the prim `PRIM_pf_hashcode`). 3c-1 adds a `hash` message that **mirrors `printString`'s structure** — a prim answers for intrinsics; genuine heap objects fall through to the vtable so a class can override `hash` (keeping the `=`/`hash` contract overridable in lockstep with `=`):

| Receiver | `hash` |
|---|---|
| SMI | its value (already a tagged int) |
| Float / LargeInteger | value-based |
| **String** | **content** FNV-1a (the crux — equal content ⇒ equal hash) |
| Symbol / boolean | **identity** hashcode (interned ⇒ identity is stable & distinct; `true`/`false` are distinct objects) |
| nil | a fixed constant |
| heap object | fall through → `Kernel.Object>>hash` ⇒ `^ self identityHash` (overridable) |

`identityHash` is the universal pointer-hash prim (the existing `objHashcode:` mechanism, exposed as a universal selector like `==`). `Kernel.Object` defines `hash = ( ^ self identityHash )` so the default is identity yet overridable. **The contract:** `a = b ⇒ a hash = b hash`. It holds because intrinsic `=`/`hash` are defined together (SMI value/value, String content/content, Symbol & boolean identity/identity), and for heap objects both default to identity via `Object` (override both together to change it).

### 5.3 `=`

Mostly already present: `Object>>=` → `==` (3a, overridable); SMI `=` prim; Symbol/boolean `=` → identity (`dispatch.c:507`). 3c-1 adds only **String `=`** (content, §4). No other `=` changes.

## 6. Booleans — closing the dialect #5 gap

`true`/`false` are, and **stay**, intrinsic **Symbol singletons** (`kernel_boot.c:61`-`62`; no `True`/`False` classes — dialect #5 persists). They were the headline gap in the value protocol; they now answer it **for free** via §5:

- `true = false` → false (identity prim, already works); `true = true` → true.
- `true hash` / `false hash` → distinct stable identity hashes (§5.2).
- `true printString` → `String` `'true'`; `false printString` → `'false'` (special-cased, §5.1).

This **closes the #5 gap** (booleans now have full `hash`/`=`/`printString`); the deviation itself (intrinsic singletons, not `True`/`False` classes) remains.

## 7. Migration & staying green

The suite (**54 pass / 0 fail / 3 skip, both paths**) is the safety net — every program's *observable* behavior must be preserved across three semantic shifts:

1. **`printString` now yields `String`, not `Symbol`.** Consumers concatenate/print the result (`('arrayWrite: ' , p arrayWrite printString) out`); a `String⊕String` `,` and `String`-printing keep observable output identical. Any test asserting a printString result *is a specific interned Symbol* would break — none are expected; the suite confirms.
2. **String literals are now `String`, not interned `Symbol`.** Code using `'abc'` as a value (concat operands, `out` arguments) is unaffected. Code that relied on a string literal being `==`-identical to a Symbol would change — not expected in the suite.
3. **String `=` moves from identity to content.** `stringCompare` (`00_perf/perfstex.ns:74`) does `'…' = '…'` 50 000× — passes today via interned-Symbol identity, must still pass via **content** `=` (distinct literal objects, equal bytes → true). This is the explicit `=`-semantics regression pin.

**Risk posture / sequencing:** the `String` type + GC leaf + literal emission (both paths) is the foundation and where a regression would surface (GC misreading bytes; Path-B constant decode) — so the plan lands **`String` + its `=`/`hash`/`printString` + the GC leaf change first and alone**, verified green on both paths, before (or alongside) the `hash`/`printString` retarget for the *other* types. Mirrors 3a/3b putting the foundational VM change first.

## 8. Testing

**Regression net:** suite stays **54/0/3 both paths**; `stringCompare` pins the `=`-semantics change (#7.3); deltablue/richards/queens prove the printString retarget preserved object protocol.

**New protocol tests (both Path A and Path B):**
- **String identity/content:** two distinct literals of equal content are `=` (true) but may be `==`-distinct; `'ab' , 'c'` `=` `'abc'` (true) and `('ab','c') hash = 'abc' hash`; `'abc' = #abc` is false.
- **String byte protocol:** `size` (bytes — assert a multi-byte UTF-8 literal's `size` is its **byte** count, pinning dialect #10), `at:` returns a SMI `0..255`, `do:` visits each byte; `'' isEmpty`.
- **String is NOT a Collection (Option C):** `aString collect:`/`select:`/`asArray` are `doesNotUnderstand` (mirrors the Array test, dialect #3).
- **`printString` → String, all types:** SMI/Float/LargeInteger/nil/String render correctly; `#foo printString` → `'#foo'` (prefix), `true printString` → `'true'` / `false printString` → `'false'` (no prefix); the result answers `String` protocol (e.g. `,`).
- **`hash` contract:** `a = b ⇒ a hash = b hash` across SMI, String (content), Symbol & boolean (identity); `true hash ~= false hash`; `nil hash` stable.
- **Booleans (#5 closed):** `true`/`false` answer `=`/`hash`/`printString` as above.

**Bar per step:** green on `oevm_runner`, both paths.

## 9. `docs/NEWSPEAK_DIALECT.md` changes

- **ADD #10 — String is a UTF-8 byte sequence with a byte-granularity protocol; no `Character` type.** Chars are code-point ints (à la Toit); `size`/`at:`/`do:` operate on **bytes** (`at:` → 0–255, never nil); Strings are **non-interned** (content `=`/`hash`). **Why:** embedded subset — UTF-8 is wire-correct and byte-level concat/`=`/`hash`/serial are correct for free; rune decoding is deferrable, additive work; chars-as-ints matches Toit; interning unbounded user strings is an embedded failure mode. **Revisit trigger:** a need for character indexing/counting, `fromRune:`, non-ASCII case-mapping, or boundary-safe slicing → add an additive `rune*` protocol over the same storage (`at:`/`size` stay byte-granularity). **Tracked:** issue #1.
- **AMEND #3** — title/body generalize to "**`Array` and `String` are bare indexables, not `Collection`s**" (same repr rationale: slot0 = length, no `outer`→Kernel link).
- **AMEND #5** — booleans now answer the **full value protocol (`hash`/`=`/`printString`)** as of 3c-1; the deviation persists (intrinsic singletons, no `True`/`False` classes), but the protocol gap is closed.

## 10. Open items / deferred

- **3c-2:** `Dictionary` (open-addressing over `platform basicNew: Array`, keyed by this step's `hash`/`=`) + `Set` (Dictionary value = key). Its own brainstorm (collision strategy, layout, Collection membership — Dictionary/Set *are* normal objects with an `outer`→Kernel link, so they **can** be `Collection`s, unlike Array/String).
- **3c-3:** `ReadStream`/`WriteStream` (WriteStream owns a private growable byte buffer; `contents` materializes one `String` via `platform basicNew: String bytes: n` — no O(n²)) + the `Serial` native capability (per-target byte sink).
- **String rune protocol** (issue #1): `runeAt:`/`runesDo:`/`runeSize`/`fromRune:`/`nextPutRune:`, boundary-safe slicing — additive over the same UTF-8 storage.
- **String ordering / substring / `asSymbol`** — deferred until a consumer appears (no suite use).
- **`displayString`** (no-`#` symbol rendering) — later nicety; `asString = printString` for now.
- **Custom-class `hash` override** works via `Object>>hash` (§5.2); value-equality-keyed user classes in Dictionary land naturally in 3c-2.
- **Carried-forward step-1 minors** (still deferred): `fflush(stdout)` in platform `print:`/`printNewline:`; `indexable_new` prototype in `oevm.h`.

## References
- Predecessor: `docs/superpowers/specs/2026-06-20-ns-oevm-stdlib-step3b-kernel-module-collections.md`
- Parent: `docs/superpowers/specs/2026-06-20-ns-oevm-stdlib-design.md` (§9 Symbol-vs-String question)
- Dialect: `docs/NEWSPEAK_DIALECT.md` (#3 bare indexable, #5 booleans, #7 universal prims, new #10)
- Issue: https://github.com/davidg238/ns-oevm/issues/1 (deferred rune protocol)
- Compiler: `tools/ns_compile_oevm.py` (literal kind split :453; `StaticSymbol_` emission ~:1001–1017)
- VM (sibling `../oevm`): `src/oevm.h:47` (`struct Symbol`, add `struct String`); `src/dispatch.c` (`PRIM_printString`, `PRIM_comma`, `PRIM_pf_hashcode`, boolean test :212, `=`/`==` :507); `src/gc.c:367`/`:411`/`:426` (payload scan + Float leaf special-case → add String); `src/oimg_loader.c` (Path B constants, `format_version` bump)
- Toit reference (validated v1.64.0): immutable UTF-8 `string` (`[] → int?` rune, `size` = bytes), separate mutable `ByteArray`, `io.Buffer` growable string builder — our String / (3c-3 buffer) / WriteStream map 1:1
