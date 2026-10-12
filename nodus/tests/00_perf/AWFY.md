# Are We Fast Yet in nsl (rete#37)

`awfy.nsl` is the *Are We Fast Yet* suite (Marr, Daloze, Mössenböck, DLS 2016;
github.com/smarr/are-we-fast-yet, MIT) ported from its SOMns sources for rete#37: a cross-VM benchmark
set that is wider than Richards and DeltaBlue. One program holds AWFY's Core library, its harness and
all fourteen benchmarks, because an nsl program is one file and a board runs one image. Every row
runs in one boot.

AWFY's own rule holds: a benchmark uses only Core (Vector, Set, IdentitySet, Dictionary,
IdentityDictionary, Pair, Random) and Array, never the kernel's collections. A departure from the
SOMns source is allowed only where nsl's dialect forces it, and each one is listed below.

## Running

- **nsl, host:** compile with `nodus/nsl/tools/ns_compile_oevm.py awfy.nsl awfy.oimg --format=oimg
  --layout=<build>/oevm_layout.json`, then run `oevm_img awfy.oimg`. For timings, use an `-O2`
  `-DOEVM_NO_DEBUG_HOOKS` build of the image VM: the suite's `oevm_img` is unoptimised.
- **Primordial Soup, host:** `python3 nodus/tests/00_perf/awfy-soup.py [work-dir [source.nsl]]`. nsl
  is a subset of Newspeak, so the same source compiles on Soup with four substitutions, all in
  the driver: the clock (`millisecondsToRun:`); Json's substring read; every decimal literal
  wrapped in `asFloat`, because Soup reads `1.5` as an exact Fraction (`0.1 + 0.2` prints
  `(3/10)`), which made Mandelbrot iterate on 100,000-digit rationals; and two Smalltalk basics
  that AWFY's SOMns sources assume and Soup's Object lacks (`==`, `notNil`). The basics are added
  to a copy of Soup's kernel. Soup is built in `~/workspace/primordialsoup` (`scons -Q
  out/ReleaseHost/primordialsoup out/snapshots/CompilerApp.vfuel`).
- **Board:** like any perf program (`richards.nsl`): `run.sh --board <b>` with this file.

Output: one `<name>: <ms>` line per row, then `verified: <bool>` and `done`. The `inner:` counts in
`runAll` are sized for a board. A host comparison scales them up.

## Structure forced by the dialect

- **Classes before methods** in every class body: Soup's parser requires it. nsl does not care.
- **Module slots:** AWFY gives each benchmark its own module, with module-level slots and constants.
  Here there is one module (`Awfy`). CD's constants are slots of `Awfy`. Richards' and
  DeltaBlue's are slots of their benchmark class, with their helper classes nested inside it.
  NBody's are methods (`pi`, `daysPerYear`, `solarMass`), lower-cased because a capitalised
  implicit send resolves as a global in nsl.
- **Dictionary's entry classes** (`DictEntry`, `DictIdEntry`) are at module level: nsl's classes are
  statically bound (dialect row 1), so `IdentityDictionary` cannot name a class nested in
  `Dictionary`. The same reason puts DeltaBlue's `Plan` at module level.
- **Renamed to avoid collisions:** CD's `Node` and `Entry` are `CDNode` and `CDEntry`.

## Departures, by kind

| nsl lacks | AWFY uses | here |
|---|---|---|
| integer `/` (it answers a Float) | `a / b` on integers | `a // b` |
| float `//` (it floors) | SOMns `//` on Floats is float division | `/` (Mandelbrot, NBody, CD) |
| `%`, `abs`, `max:`, `min:` | `x % n`, `x abs` | `x \\ n`, module helpers `abs: x`, `max: a with: b` (still one send) |
| `Array new: n withAll:` | value or block | `newArray: n withValue: v`, `newArray: n withAll: aBlock` |
| `swap:with:`, `doIndexes:`, `first`, `last` on Array | Vector, DeltaBlue | `swap: a at: i with: j`, `to:do:`, `at: 1` / `at: n` |
| `<>`, `~=`, `&&`, `\|\|`, `downTo:do:` | throughout | `(a = b) not`, `&` or `and: [ ]`, `or: [ ]`, `to: 1 by: -1 do:` |
| `ifNil:`, `ifNotNil:`, `ifNil:ifNotNil:` | throughout | `isNil ifTrue:ifFalse:` (inlined, cheaper) |
| `and:` with a non-block argument | Queens, Richards | `&`, or a block |
| `value:with:` | sort blocks, Richards | `value:value:` (Soup has only this) |
| `println`, `print`, string `+` | report paths | `out`, `,`, `printString` |
| `subclassResponsibility`, exception classes | harness, Json | `self error:` (paths never taken) |
| `$c` character literals | Json | byte values with the character in a comment: `String>>at:` answers a byte on nsl and Soup |
| String `substringFrom:to:` / `copyFrom:to:` | Json | one kernel `ReadStream` per parse (`substringFrom:to:`); the Soup driver swaps in `copyFrom:to:` |
| `sin`, `cos`, `Float>>asInteger` | CD | module helpers: fdlibm polynomials, truncation by binary descent |
| a float divide by 0.0 answering ±inf | CD (a new aircraft's first frame) | `fdiv: a by: b` |
| `^` from a real closure (row 21) | early exits in `forEach:` blocks | `hasSome:`/`getOne:`, or a flag |

Per-benchmark notes:

- **Richards:** `and: holdCount = 9297` → `and: [ holdCount = 9297 ]`. The `trace:` and `asString`
  paths are dead code.
- **DeltaBlue:** `chainTest:`/`projectionTest:` moved from Planner's class side to DeltaBlue's
  instance side, and `addConstraint:` moved out of UnaryConstraint's initializer into its
  subclasses'. Both work around compiler or VM bugs (below).
- **Json:** NULL/TRUE/FALSE are parser slots (three objects per parse) rather than module slots.
  `idx <> 0 && (...)` became short-circuit (`names at: 0` is never evaluated; every key is found).
- **CD:** `inner:` is the number of aircraft (AWFY verifies 10 → 390 collisions). 10 needs about
  35–45 KB of object heap. 100 needs 400–640 KB and does not fit a board. Three SOMns bugs on the
  never-taken removal path of the red-black tree are kept as written, apart from one
  parenthesisation the nsl parser needs.
- **Mandelbrot:** `inner:` is the size; AWFY verifies 1, 500 and 750.
- **NBody:** `inner:` is the step count; AWFY verifies only 1 and 250000.
- **Havlak (not yet merged; the port is `awfy-havlak.part`):** host-only (David,
  2026-10-12). At AWFY's size (5213 nodes, verify 1605/5213) it needs ~3.9 MB live on a 32-bit part
  and ~7.9 MB on the host; the stock 16 MB host arena fails by fragmentation (the GC does not move
  objects) and it ran at 64 MB (16.4 s, debug VM). It needs a host-only row and a larger host heap.

## Host comparison against Soup

The record is `docs/results/2026-10-12-37-awfy-soup/` (`run_all.sh` scales the counts, builds an `-O2` VM and runs
both VMs). At rete `33cb0e16` on opti7040, median of three: nsl ÷ Soup is List 4.14, Storage 1.95, Json 1.77,
Towers 1.75, DeltaBlue 1.72, NBody 1.46, Permute 1.37, Queens 1.26, Richards 0.95, Sieve 0.87, Mandelbrot 0.63,
Bounce 0.62, CD 0.56. Geomean 1.26× slower.

## Found while porting: compiler and VM bugs (#420)

- **A.** A class-side method of a nested class has a broken outer chain: an implicit send to an
  enclosing method answers "an Object", and a slot initializer that reads an enclosing slot
  segfaults.
- **B.** `class X = Vector new: 15 ()()` then `X new` runs the superclass's class-side `new`, not
  X's own factory.
- **C.** A class nested in a class whose superclass is a module-level class (through a constructor
  clause) gets its inherited slots uninitialised.
- **D.** A superclass's initializer body never runs for a subclass instance (slot initializers do).
