# L5 — Error convention (adopt before the stdlib grows)

**Status:** design frozen 2026-07-03. A convention, not code — but every `.ns` file in
this directory follows it, and every stdlib method landed from now on must.

## The two-tier rule

**Tier 1 — expected absence or failure** is part of a method's contract and takes an
explicit continuation block, Smalltalk-style:

| Situation | Selector shape | Example |
|---|---|---|
| lookup may miss | `…ifAbsent:` | `map at: k ifAbsent: [ 0 ]` |
| search may fail | `…ifNone:` | `col detect: [:e | e > 9] ifNone: [ nil ]` |
| decode may fail | `…ifInvalid:` | `bytes asStringIfInvalid: [ '' ]` |
| channel may be empty | returns nil (pinned) | `chan take: 'path'` |

Rules:
- The block runs **only** on the failure path (lazy).
- A convenience form without the block MAY exist (`detect:`) and **crashes** on failure —
  it is defined as `detect: b = ( ^ self detect: b ifNone: [ self error: 'not found' ] )`.
- Never return a sentinel (nil, -1, false) from a selector that lacks an `if…:` variant.
  `take:` is the single sanctioned nil-return, because "empty now" is its meaning, not a
  failure.

**Tier 2 — everything else crashes.** Bounds violations, contract violations, type
confusion, impossible states: `self error: '<what>'`. No stdlib method "recovers"
internally, retries, or logs-and-continues. The supervisor is the handler; the crash
boundary (Milestone D) contains it; restart policy decides what happens next.

## Why this is the right shape for nsl-tuvm

- It needs **zero VM machinery** — no exceptions required — so the whole stdlib can be
  written before `on:do:` exists (spec §8 defers it).
- It keeps failure **local and explicit** at every call site, which is what a reviewer
  (or an LLM writing node scripts) can check by reading one line.
- When full exceptions land, they slot in *above* this convention: supervisor-level and
  coarse job-level `on:do:` for the let-it-crash boundary — never as a replacement for
  `ifAbsent:`-style contracts. Stdlib APIs do not change on that day.

## Message text convention

`self error:` strings are short, lowercase, and name the violated contract, not the
implementation: `'index out of bounds'`, `'not found'`, `'invalid utf-8'`,
`'message depth exceeded'`, `'mutation of immutable value'`, `'outside capability root'`.
Tests assert on these exact strings (they are API), so choose once, here:

| Condition | Exact string |
|---|---|
| index outside 1..size | `index out of bounds` |
| detect:/at: convenience miss | `not found` |
| byte value outside 0..255 | `byte out of range` |
| splitting a UTF-8 code point / bad bytes | `invalid utf-8` |
| message tree deeper than bound | `message depth exceeded` |
| mutable object placed in a message | `not a value` |
| mutating an algebra value | `immutable` |
| channel path outside capability root | `outside capability root` |
| malformed channel path at grant | `bad channel path` |
| integer arithmetic exceeds ratified L7 range ±(2^64−1) | `integer overflow` |
| CBOR decode: `−2^64` or bignum tag (2/3) — outside the ±(2^64−1) range | `integer out of range` |
| CBOR decode: indefinite length, unknown tag (≠27/39), undefined (`0xf7`), duplicate map key, NaN float, truncated/malformed input | `not a value` |
| CBOR decode: message tree deeper than bound (32) | `message depth exceeded` |
| CBOR decode: malformed UTF-8 in a text item | `invalid utf-8` |

## Exceptions — the boundary above the convention (landed Milestone E, 2026-07-07)

Full `on:do:`/`ensure:` now sit **above** the two-tier convention exactly as the section above
anticipated — they do NOT replace `ifAbsent:`-style contracts. The four verbs:

| Verb | Shape | Semantics |
|---|---|---|
| `self error: 'text'` | Tier-2 crash | raises a crash `Record { #tag -> #error, #text -> 'text' }`. The default `#error` tag is the image-**interned** literal, so it is catchable by `[ :e | e tag = #error ]` (identity match). |
| `text error: aTag` (`error:tag:`) | tagged crash | raises `Record { #tag -> aTag, #text -> text }`. |
| `e raise` | re-raise | re-raises the receiver condition unchanged (same tag/text) to the next enclosing handler. |
| `[ prot ] on: [ :e | pred ] do: [ :e | body ]` | predicate-filtered catch | runs `prot`; on a raised condition, if `pred value: e` is truthy runs `body` (its value is the result), else re-raises (cascades frame-by-frame). |
| `[ prot ] ensure: [ cleanup ]` | always-run cleanup | runs `cleanup` on BOTH clean completion and a fault; on a fault, re-raises **after** cleanup. |

Rules:
- Handlers/predicates/cleanups are real (escaping) closures — `^` inside them is a **compile error**
  (no-NLR, `07_no_nlr`); capture the `on:do:` result and `^` it from the method body instead.
- Terminate-only: no `resume:`, no `retry`. The handler runs **after** the stack unwinds.
- A crash never crosses a dispatch boundary (C5): a reaction's raise reaches at most its own reactor
  frame → `sys/crash` → the supervisor. One reaction's `on:do:` can never catch another's crash.
- The in-flight error value is the crash `Record` (`#tag`, `#text`); `#channel` is attached only at
  the reactor boundary (`postCrash:channel:`). Handlers read `e tag` / `e at: #text ifAbsent: [ … ]`
  (there is no `Object>>text`).
- `sys/crash` carries the **raise tag** (Milestone E decision (a); the Milestone D `#crash` tag
  retired — the supervisor matches jobs by `#channel`, never the tag, so this is safe).

Proven by `nodus/tests/12_exceptions/01..14`. See `docs/MILESTONE_E.md`.
