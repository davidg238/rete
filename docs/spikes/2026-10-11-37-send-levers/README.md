# rete#37 spike — the send levers: fewer sends in the kernel collections and at Array call sites (2026-10-11)

> Code on branch `37-send-levers-spike` (on master after Opt #8, `7e3c4e59`). Not landed. The comparison
> images are `docs/results/2026-10-11-37-stic/` (before = `0d773612`, Opt #8 = `7e3c4e59`).
>
> **Landed as Opt #9** (`nodus/oevm/docs/optimizations.md`) with one change: the stored-hash probe compares with
> `=`, not `==`, since a user `hash` may answer a LargeInteger (test `nodus/tests/04_somns_basic/69_stored_hash_keys.nsl`).

**Question.** After Opt #8, STIC's array, dictionary and collection write rows still cost about one full send
per kernel collection operation: ~7 µs on the WeAct, ~9 µs on the ESP32. Counted from the kernel source:
arrayWrite does 1 send per store, collectionWrite ~50 per collection, dictionaryWrite ~300 per dictionary.
How much goes if the sends go?

**What the branch does** (four changes, each a general mechanism, none STIC-shaped):

1. **Kernel collections on the fast opcodes.** OrderedCollection, HashedCollection, Set and Dictionary use
   `basicAt:`/`basicAt:put:`/`basicSize` on their own Arrays (OP_AT/OP_AT_PUT, as `OrderedCollection>>do:`
   already did), and the one-line helpers (`ensureCapacity`, `indexFor:`, `capacity`, `growIfNeeded`) are folded
   into their callers.
2. **Array `at:`/`at:put:`/`size` at app call sites behind `IS_VT`.** AT/AT_PUT carry the selector, so an index out
   of bounds falls back to the send and its error, as today.
3. **`basicReplaceFrom:to:with:startingAt:`** (prim 163, one `memmove`, bounds-checked) for OrderedCollection's
   grow, in place of a bytecode copy loop.
4. **Stored hashes** in Set and Dictionary (`hashes` beside `table`, as CPython's entries keep theirs). A probe
   compares the stored hash before it sends `=`, and a grow or cluster rehash re-places from it with no `hash` and
   no `=`. The comparison is `==`, so it relies on every `hash` answering a SmallInteger, which the kernel's do.

Kept: the initial capacity of 8. Toit, MicroPython/CircuitPython and CPython grow at least as often for 10
elements (Toit 3 list / 7 map allocations, MicroPython 2 / 5, CPython 3 / 2, all read from their sources). The
difference is the cost of a grow, not how many there are, which is what 3 and 4 address. A 16-slot start measured
−26/−36 % on the host and was rejected as tuning to STIC's 10.

## Result — STIC on the WeAct RP2350B (seconds; three boots each, identical to within 1 ms)

| Test | before | Opt #8 | + levers | before → now |
|---|--:|--:|--:|--:|
| alloc | 18.0 | 18.1 | 16.8 | 1.1× |
| array write | 67.7 | 68.5 | **22.9** | **3.0×** |
| dictionary write | 19.4 | 18.1 | **7.5** | **2.6×** |
| float math | 26.3 | 25.9 | 25.9 | 1.0× |
| integer math | 4.7 | 4.8 | 4.8 | 1.0× |
| collection iterate | 100.8 | **15.9** | 15.8 | **6.4×** |
| collection write | 38.7 | 38.6 | **17.0** | **2.3×** |
| string compare | 3.0 | 3.1 | 3.1 | 1.0× |

Per lever on the board: 1 + 2 took arrayWrite 68.5 → 22.9, dictionaryWrite 18.1 → 8.8, and collectionWrite
38.6 → 19.5 (`logs/weact-stic-levers-*`). Then 3 took collectionWrite 19.5 → 17.0 and 4 took dictionaryWrite
8.8 → 7.5 (`logs/weact-stic-levers2-*`). alloc −7 % has no lever behind it; read it as placement until measured.

Gates on the branch (opti7040): the nsl suite **294 passed / 0 failed**, every aux gate that runs there PASS.

## Not done: cheaper sends

A host gprof of 10M monomorphic sends (`-O2 -pg`) puts ~62 % in the interpreter loop, ~19 % in `send_resolve`
(method lookup plus the access check, every send), ~8 % in `push_frame` and ~3 % in `lookup_method_cached`.
gprof first filed push_frame's calls under the neighbouring `default_millis`; a counting build showed 2 calls.
On the WeAct a send is ~1,050 cycles against the host's ~300 per iteration, so memory latency (XIP) is likely a
large share on the board. That needs a board-side measurement before any design.
