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
| malformed channel path at grant | `invalid channel path` |
| integer overflow past 2^63−1 | `integer overflow` |
