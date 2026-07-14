# ns-oevm std-lib step 3c-3b — Serial (the first native capability tier)

**Status:** ratified design (brainstorm w/ David, 2026-06-22). Next: writing-plans → SDD.
**Predecessor:** 3c-3a (Streams: ReadStream + WriteStream), merged + pushed.
**Successor:** the remaining native capabilities (`Gpio`/`Timer`/`Thread`/`Net`…) reuse
the per-module + link-time-backend pattern this cycle establishes; each is its own
spec → plan → SDD cycle.
**Specs cross-ref:** `2026-06-20-ns-oevm-stdlib-design.md` (capability spine; §2.1 three
tiers; §5 native scope; §7 host-mockable Serial), `2026-06-22-ns-oevm-stdlib-step3c3-streams-design.md`
(the String-only stream substrate this builds beside), `2026-06-21-ns-oevm-stdlib-step3c1-string-booleans.md`
(immutable UTF-8 String + `platform string:`), `docs/NEWSPEAK_DIALECT.md` (divergence governance).

---

## 0. Scope decision — Part 1 of 2 (ratified 2026-06-22)

`Serial` is **the first real native capability tier**: a confined (non-ambient)
capability reaching a per-target C backend. The VM (`../oevm/src`) is **host-only
today** — no Zephyr/ESP-IDF UART backend exists — so this feature is genuinely **two
parts**:

- **Part 1 (this cycle, 3c-3b):** design the native-tier capability — its **grant
  policy**, its Newspeak **protocol**, and a **per-target C backend interface** — and
  implement exactly **one** backend: the **host** one (an in-memory loopback). The host
  backend is a real backend, not a throwaway; it is the first implementation of the seam.
- **Part 2 (future, when a firmware/cross-compile target lands):** write the
  Zephyr/ESP-IDF UART backend *behind the same seam*. That is where **"works on device"**
  is checked off. This cycle does **not** stand up a cross-compile/firmware path.

**Definition of done for Part 1 (the host-provable proxy for "will work on device"):**
the backend boundary is clean enough that adding a device target is **purely additive** —
a new backend file implementing a fixed, documented C interface — with **zero host-isms
leaking** into the Newspeak protocol, the dispatch glue, or the grant mechanism. Proven
on host with a serial round-trip test, **both image paths**. If a hypothetical device
backend would force a change to the protocol or the grant glue, Part 1 failed its job.

Generic-collection streaming and rune-granular text remain deferred (carried from
dialect #3/#10/#12). This cycle is **byte/String-focused**, matching the stream substrate.

---

## 1. Goal & motivating use case

Add a `Serial` capability so embedded scripts can do line-oriented serial I/O — the
use case that justified streams in the first place (master design §1/§5: "Streams matter
(reading from serial)"). Concretely:

```newspeak
class Main usingPlatform: platform serial: serial = (
  run = (
    serial write: 'AT'.            "send a command"
    serial writeByte: 13.          "CR — no String constructor needed"
    | line |
    line := serial readLine.       "answer a String, nil at EOF"
    ^line
  )
)
```

A module that does **not** declare `serial:` in its capability header has **no way to
name** the serial capability — the confinement that distinguishes the native tier from
ambient `platform` (master design §2.1).

---

## 2. Prior art (source-verified 2026-06-22)

Three comparable Smalltalk/Newspeak-lineage VMs span a spectrum from a flat primitive
monolith to per-domain modules. This cycle moves ns-oevm deliberately along that
spectrum, toward Toit's structure.

### 2.1 PrimordialSoup — the flat numbered-primitive monolith (≈ today's `dispatch.c`)
Newspeak VM, C++ (repo **`rmacnak/primordialsoup`**, branch `master`). At the VM level
**all** primitives — arithmetic, strings, arrays, closures, file/process/terminal I/O,
JS interop, Zircon — live in **one** `vm/primitives.cc` (~4,167 lines), declared via a
flat X-macro `PRIMITIVE_LIST(V)` mapping a **global primitive number** to a function,
filled into `primitive_table_[kNumPrimitives /* =512 */]` at startup. Dispatch is a
direct numeric array index (`Primitives::Invoke(..., prim)` in `vm/primitives.h`), with
an inline getter/setter fast-path keyed on `prim & 512 / & 1024` in
`Interpreter::Activate` (`vm/interpreter.cc`). Capabilities/"platform" structuring is
**purely image-level**: the binding lives in `newspeak/KernelForPrimordialSoup.ns` via
`(* :pragma: primitive: N *)` annotations whose integers are exactly the VM's flat
numbers. There is **no VM-level notion of a platform capability object** — which is
exactly the structure ns-oevm is trying not to keep as native modules multiply.
- `vm/primitives.cc` https://github.com/rmacnak/primordialsoup/blob/master/vm/primitives.cc
- `vm/primitives.h` https://github.com/rmacnak/primordialsoup/blob/master/vm/primitives.h
- `vm/interpreter.cc` https://github.com/rmacnak/primordialsoup/blob/master/vm/interpreter.cc
- `newspeak/KernelForPrimordialSoup.ns` https://github.com/rmacnak/primordialsoup/blob/master/newspeak/KernelForPrimordialSoup.ns

### 2.2 SOMns — per-domain primitive node classes, annotation-registered (Truffle)
Newspeak-on-Truffle, Java (repo **`smarr/SOMns`**, branch `release`). Each primitive is a
**Truffle expression node class** in a per-domain file/package under
`src/som/primitives/` (`StringPrims.java`, `IntegerPrims.java`, `arithmetic/`, `arrays/`,
`reflection/`, `actors/`, `threading/`, …), carrying `@GenerateNodeFactory` +
`@Primitive(primitive = "string:concat:", selector = …, receiverType = …)`. The registry
`src/som/vm/Primitives.java` (`extends PrimitiveLoader` from the shared `bd.primitives`
"black-diamonds" lib) reads those annotations **reflectively** and installs each as an
image-callable invokable. Dispatch is Truffle **node-per-primitive** with DSL
specialization — not a numeric table or `strcmp` switch. The **vmMirror/platform split is
image-level**: `core-lib/Platform.ns` (`class Platform usingVmMirror: vmMirror`) wires up
subsystems, and `core-lib/Kernel.ns` methods delegate to `vmMirror` with selectors
(`vmMirror string: self concat: argument`) that are the *join key* to the Java node's
`@Primitive(primitive=…)`. Modular and capability-clean — but Truffle-AST-shaped, not
directly portable to a JIT-less C bytecode VM.
- `src/som/primitives/StringPrims.java` https://github.com/smarr/SOMns/blob/release/src/som/primitives/StringPrims.java
- `src/som/vm/Primitives.java` https://github.com/smarr/SOMns/blob/release/src/som/vm/Primitives.java
- `core-lib/Platform.ns` https://github.com/smarr/SOMns/blob/release/core-lib/Platform.ns
- `core-lib/Kernel.ns` https://github.com/smarr/SOMns/blob/release/core-lib/Kernel.ns
- `@Primitive` (black-diamonds) https://github.com/SOM-st/black-diamonds/blob/master/src/bd/primitives/Primitive.java

### 2.3 Toit — per-domain primitive modules + Resource/ResourceGroup + host/device split (the model)
Embedded language VM, C++ (repo **`toitlang/toit`**, branch `master`). Primitives are
split into **per-domain files** — `src/primitive_core.cc`, `primitive_crypto.cc`,
`primitive_font.cc`, `primitive_math.cc`, `primitive_file_non_win.cc`, … — each declared a
**module** via `MODULE_IMPLEMENTATION(name, MODULE_<NAME>)` over an X-macro list of
`(name, arity)` pairs, with bodies written `PRIMITIVE(name) { ARGS(...) ... }`
(`src/primitive.h`; the `MODULE_TYPES` macro guessed earlier does **not** exist — argument
typing is in-primitive via an `ARGS(...)` macro). Dispatch is **two-level numeric**: the
`PRIMITIVE` bytecode carries a **1-byte module index** + a **16-bit primitive index**
(`Interpreter::run` in `src/interpreter_run.cc`); module tables are registered via
**weak symbols** so unlinked modules resolve to null (`Primitive::set_up` in
`src/primitive.cc`). Crucially for us, **hardware/peripheral primitives do not live in
`primitive_*.cc`** — they live in **`src/resources/`** as `Resource` / `ResourceGroup`
subclasses (`src/resource.h`), and the **host-vs-device backend split is per-platform
files gated by `#ifdef`**: `src/resources/uart_esp32.cc` (`#ifdef TOIT_ESP32`),
`src/resources/uart_posix.cc` (`#ifdef TOIT_POSIX`), `src/resources/uart_win.cc`; GPIO/I2C/SPI
follow the same `*_esp32.cc` / `*_linux.cc` convention.
- `src/primitive.h` https://github.com/toitlang/toit/blob/master/src/primitive.h
- `src/primitive.cc` https://github.com/toitlang/toit/blob/master/src/primitive.cc
- `src/interpreter_run.cc` https://github.com/toitlang/toit/blob/master/src/interpreter_run.cc
- `src/resource.h` https://github.com/toitlang/toit/blob/master/src/resource.h
- `src/resources/uart_posix.cc` https://github.com/toitlang/toit/blob/master/src/resources/uart_posix.cc
- `src/resources/uart_esp32.cc` https://github.com/toitlang/toit/blob/master/src/resources/uart_esp32.cc

### 2.4 Where ns-oevm sits, and the direction of travel
Today ns-oevm is **PrimordialSoup-shaped**: one `dispatch.c` with a flat `PRIM_NAMES`
table and receiver-gated blocks (`if (receiver == &PlatformObj) switch(pid)`). This cycle
adopts **Toit's structure, simplified for our VM**:

| Aspect | Toit | ns-oevm 3c-3b |
|---|---|---|
| Per-domain module | `src/primitive_serial`/`src/resources/uart_*.cc` | `src/serial.c` |
| Host/device backend split | `uart_posix.cc` vs `uart_esp32.cc`, `#ifdef` | `serial_host.c` vs (later) `serial_zephyr.c` behind `serial_backend.h` |
| Capability dispatch | two-level numeric table | receiver-identity hook + module-internal `strcmp` (few selectors) |
| Peripheral handle | `Resource` / `ResourceGroup` | single `&SerialObj` singleton (one port) |
| Capability object | n/a at VM level | confined `.ns` `Serial` wrapper handed by boot |

The two places we stay simpler than Toit — **numeric two-level dispatch** and
**multi-instance resource groups** — are **reversible upgrades**: adopt them when Net or
multiple UARTs need them. SOMns's reflective annotation registry and PrimordialSoup's flat
monolith are explicitly **not** adopted (the former needs Truffle; the latter is what we
are migrating away from).

---

## 3. Architecture

### 3.1 Two-layer representation — native singleton + `.ns` wrapper
Mirroring 3c-3a (logic in `.ns`, minimal C), the capability is two layers:

- **`&SerialObj` — a C native singleton (the raw byte port).** A static singleton like
  `&PlatformObj`. Its surface is **three** receiver-gated prims, fired only when the send's
  receiver **is** `&SerialObj`:
  - `basicReadByte` → a `SmallInteger` 0..255, or **nil** when the port is empty.
  - `basicWriteByte:` (an SMI 0..255) → self.
  - `basicAtEnd` → boolean (no bytes currently available).
  Beneath these three prims sits the link-time `serial_backend.h` seam (§3.4). `&SerialObj`
  holds **no heap oops** (the host FIFO is a static C buffer), so GC `mark()` skips it like
  it skips `&PlatformObj`/`.rodata` statics — **no new GC root expected** (empirical-verify
  item §9).

- **`Serial` — a thin Kernel `.ns` class (the public capability object).** A Kernel-library
  class holding `&SerialObj` in a slot and supplying the public byte/line protocol (§4) in
  Newspeak. Its substring/line methods build `String`s via `platform string:` (the 3c-3a
  byte-Array→String primitive). **The object handed to a module is this wrapper.**

Confinement is real: the `.ns` `Serial` wrapper — and the `&SerialObj` it privately holds —
is reachable only by modules explicitly handed `serial:`. You cannot *name* `&SerialObj`
without holding the wrapper.

### 3.2 Grant policy — boot-injected sibling capability (Newspeak-canonical confinement)
`platform` is ambient (every module is handed it). `Serial` is **not**: capabilities flow
from the root and a parent grants to a child explicitly.

- Boot constructs the `Serial` wrapper and hands it to the **entry module** as an
  **additional** header argument, parallel to `platform`:
  `class Main usingPlatform: platform serial: serial = (...)`.
- A module that wants serial **declares** `serial:` in its capability header; a parent
  threads it to each child it chooses: `Logger usingPlatform: platform serial: serial`.
- A module declaring only `usingPlatform: platform` has **no reference** to serial — it
  literally cannot reach it. This is the confinement of master design §2.1.

The grammar already parses multi-keyword ctor signatures (`CtorSig = (keyword
BinarySend)+`, master design feasibility finding), so `usingPlatform: platform serial:
serial` parses with no grammar change.

### 3.3 Native-capability C composition — one capability = one module + one dispatch hook
To avoid native modules accreting into the 66 KB `dispatch.c` monolith (the
PrimordialSoup shape) as Gpio/Timer/Thread/Net follow, each native capability becomes a
**self-contained C module**, dispatched by a **receiver-identity hook** placed early in
`send()` (before the flat `pid` path), so the capability's selectors stay **out of** the
global `PRIM_NAMES`/pid cache entirely:

```c
/* in send(), after the BoundClass check, before selector_prim_id: */
if (receiver == serial_object()) return serial_handle(receiver, sym, argc, args);
/* later capabilities add one line each:
   if (receiver == net_object())  return net_handle(receiver, sym, argc, args); */
```

New files in `../oevm/src/`:

| File | Role |
|---|---|
| `serial.h` | capability API: `serial_object()`, `serial_boot()`, `serial_handle()`, GC hook if needed |
| `serial.c` | the `&SerialObj` singleton + its **own** selector dispatch (`basicReadByte`/`basicWriteByte:`/`basicAtEnd`) calling the backend |
| `serial_backend.h` | the link-time seam (§3.4) |
| `serial_host.c` | the host loopback-FIFO backend |

`serial_handle` does its **own** `strcmp` over its 3 selectors (cheap; optional per-module
cache later — empirical-verify §9), so `dispatch.c`'s flat `PRIM_NAMES` array gains **zero
entries**. One capability = one module = one hook line + one backend seam. (Net later:
`net.c` owns `connect:`/`bind:`/…; **IPv4 vs IPv6 is entirely a `net_backend.h` detail**,
invisible above the seam — the same shape as host-vs-Zephyr for Serial.)

**Extract `platform.c` this cycle too (ratified).** Move the existing
platform-receiver-gated block out of `dispatch.c` into a `platform.{c,h}` module behind the
same `if (receiver == platform_object()) return platform_handle(...)` hook, so the pattern
is proven on **two** capabilities and `dispatch.c` shrinks now rather than later. This is
churn on working alloc/boot code; it is **behaviour-preserving** (the conformance suite,
both paths, is the safety net — same role it played for the 3a Array migration). The
platform prims keep their existing names/semantics; only their *home* moves.

### 3.4 The link-time backend seam (host now, device later — purely additive)
A fixed C interface; the firmware build links **exactly one** implementation. This is the
embedded norm and Toit's model (`uart_posix.cc` vs `uart_esp32.cc`).

```c
/* serial_backend.h — the fixed per-target seam */
void serial_open(void);        /* idempotent init at boot */
int  serial_read_byte(void);   /* next byte 0..255, or -1 when empty */
void serial_write_byte(int b); /* b in 0..255 */
bool serial_at_end(void);      /* true when no byte currently available */
```

- **`serial_host.c` (this cycle)** implements the seam as an **in-memory loopback FIFO**: a
  single static byte queue; `serial_write_byte` appends, `serial_read_byte` consumes the
  same queue (TX→RX echo), `serial_at_end` is "queue empty". Deterministic, no stdin/stdout/
  file/harness coupling — a pure `.ns` test can write known bytes and read them back.
- **`serial_zephyr.c` / `serial_esp_uart.c` (Part 2)** implement the same four functions
  against a real UART; selected at firmware-build time. **Nothing above the seam changes.**

Host-mock limitation (accepted): the loopback cannot simulate bytes "arriving from
outside" the program; genuine external RX is a device-backend concern, out of Part 1 scope.

---

## 4. Newspeak protocol — `Serial` (the `.ns` wrapper)

Self-contained byte/line port; no integration with the in-memory `ReadStream`/`WriteStream`
(those stay String-source-only; generic streaming over a live source is deferred). All
String results are built via `platform string:`.

| Message | Answers | Semantics |
|---|---|---|
| `writeByte: aByte` | self | write one byte (SMI 0..255) via `basicWriteByte:` |
| `write: aString` | self | write each byte of `aString` in order |
| `readByte` | SMI 0..255, or **nil** | one byte; **nil when no data available** |
| `read: n` | a `String` | up to `n` bytes as a String; a **short String** if fewer are available; an empty `String` `''` if `n=0` |
| `readLine` | a `String`, or **nil** | bytes up to (and consuming) the next `\n`, with a trailing `\r\n`/`\n` **stripped**; **nil when no data at all** |
| `atEnd` | boolean | `basicAtEnd` — no byte currently available |

**EOF / empty semantics (ratified — a deliberate divergence from 3c-3a, dialect #13).**
A serial port is a **live port with no known length**, so "buffer empty" is *not* "stream
of fixed length came up short." Therefore **all serial reads return `nil` when no data is
available** (`readByte`→nil, `readLine`→nil) — there is **no "short read errors" rule**
(3c-3a's fixed-count `error:` applied to a bounded in-memory String, which a port is not).
`read: n` is the one nuance: it answers the **short String** of whatever is available (you
asked for *up to* `n` bytes — the classic serial read), and only an **empty `String`** when
nothing is available (not nil — `read:` is a "give me what's there" call). This matches
Toit's `io.Reader` returning null at end. Nil-check serial results with `== nil` (the
`String`-on-`isNil` VM gap, oevm#6).

Byte-granular and rune-unaware (dialect #10): `write:`/`read:`/`readLine` are byte
operations; `readLine`'s `\n`/`\r` test is ASCII-safe because those bytes never occur
inside a multi-byte UTF-8 sequence. `write: aString` works regardless of `aString`'s
encoding (it copies bytes).

---

## 5. Boot wiring (both image paths) & entry arity

Boot extends the existing two-stage sequence (3b: native C `platform` → construct `Kernel`
→ hand `Kernel` instance to the app as `platform`):

1. native C `platform` → construct `Kernel` (unchanged).
2. **`serial_open()`** (init the host backend); `&SerialObj` is ready.
3. **construct the `.ns Serial` wrapper** (a Kernel class — available once Kernel exists),
   storing `&SerialObj` in its slot.
4. hand **`(platform, serial)`** to the entry module per its declared signature.

**Entry arity (the one empirical-verify item).** Boot must pass capabilities matching each
entry's **declared** `usingPlatform:[serial:]` selector, in canonical order
`[platform, serial]`. Entries declaring only `usingPlatform: platform` receive **only**
`platform` → existing tests are **untouched** and this is **purely additive**.
- **Path A** (compiler-generated C entry call) already knows the entry's parsed signature
  and can emit the matching call — verify and wire.
- **Path B** (`.oimg` loader constructs the entry) needs the entry's ctor arity/signature
  available at load — verify whether the existing descriptor carries it or a small addition
  is needed. **Goal: no `.oimg` format-version bump** — the three native prims live in
  shared `serial.c` reached from `dispatch.c` (like 3c-3a's `platform string:` shared via
  `dispatch.c`, which needed no format change), and the wrapper is a normal Kernel object
  constructed by boot code, not serialized.

**Cost profile (small, like 3c-3a):** new C module + one (two, with platform.c) hook line;
new static singleton; boot init on both paths; **no expected `.oimg` format change**; **no
expected new GC root**. The arity threading is the item most likely to need a small
boot/loader change — to be nailed empirically during planning (the established
"controller verifies the under-specified machinery first" pattern).

---

## 6. Newspeak dialect — row #13

Add to `docs/NEWSPEAK_DIALECT.md`:

> **#13 — Native capabilities are confined, boot-granted objects over a link-time per-target
> C backend; `Serial` is byte/line-oriented with live-port (nil-at-empty) read semantics
> (std-lib step 3c-3b).**
> *Full Newspeak/Smalltalk:* hardware access via ambient globals or a rich device framework;
> streams generic over any source; one uniform read-EOF rule.
> *ns-oevm:* a native capability is a `.ns` wrapper over a static C singleton whose few
> receiver-gated prims call a **link-time-selected** backend (`serial_backend.h`; host
> loopback now, device later — purely additive, no change above the seam). The capability is
> **non-ambient**: handed to a module only via an explicit `serial:` header argument threaded
> from boot through parents (a module not handed it cannot name it). `Serial`'s protocol is
> byte/line (`writeByte:`/`write:`/`readByte`/`read:`/`readLine`/`atEnd`); because a port has
> no fixed length, **reads answer `nil` when empty** (no short-read `error:`; `read: n`
> answers a short — possibly empty — `String`), diverging from #12's fixed-count-errors rule.
> *Why:* one capability = one C module + one hook line + one backend seam keeps `dispatch.c`
> from re-monolithising as Gpio/Timer/Net follow (vs PrimordialSoup's flat primitive bank);
> confinement is master design §2.1; nil-at-empty matches Toit `io.Reader` for a live port.
> *Revisit when:* multiple ports / runtime backend selection (adopt Toit Resource-group +
> two-level numeric dispatch), or streaming a live source (generalize ReadStream over a
> source, reversing #12's deferral).

---

## 7. Testing

- **`38_serial.ns`** (pure `.ns`, both paths): write/`readLine` round-trip over the loopback
  (`serial write: 'hello'. serial writeByte: 10. (serial readLine = 'hello') ...`); `atEnd`
  true after drain; `writeByte:`/`readByte` byte round-trip; `read: n` short-String when fewer
  than `n` available and `''` when empty; `readByte`/`readLine` answer `nil` at empty;
  `readLine` strips trailing `\r\n` and `\n`.
- **Confinement evidence:** demonstrate that a class given only `usingPlatform: platform`
  cannot reach `serial` (compile-time unresolved / DNU evidence, in the spirit of the
  Option-C `collect:` DNU pins). Exact mechanism (compile error vs runtime DNU) is an
  empirical-verify item (§9); pin whatever the toolchain actually produces.
- **Regression net:** full suite stays green **both paths** — especially after the `platform.c`
  extraction (§3.3), whose entire correctness argument is "behaviour preserved, suite proves it."
- Regenerate the committed default `solidified_image.c` from the serial test (per the
  established per-step convention).
- **Bar:** green on `oevm_runner`, both paths — run from `ns-oevm` cwd
  (`python3 tools/run_tests_oevm.py`). Baseline 61/0/3 → 62/0/3 (or +2 if confinement is a
  separate file).

---

## 8. Definition of done & scope boundaries

**In scope (Part 1):** the `Serial` `.ns` wrapper + `&SerialObj` native singleton + the three
byte prims; `serial.{c,h}` + `serial_backend.h` + `serial_host.c` (loopback); the
receiver-identity dispatch-hook composition pattern; `platform.{c,h}` extraction behind the
same hook; boot wiring + entry-arity threading both paths; dialect #13; tests; regenerated
default image.

**Out of scope (Part 2 / deferred):** any real UART backend (`serial_zephyr.c`/ESP-IDF) and
the firmware/cross-compile path; multiple ports / runtime backend selection; baud/parity/
flow-control configuration; integrating a live serial source into `ReadStream`/`WriteStream`
(generic streaming, dialect #12 deferral); `Gpio`/`Timer`/`Thread`/`Net` (each reuses this
cycle's pattern in its own cycle).

---

## 9. Empirical-verify items for planning (nail before/within SDD, per the established pattern)

1. **Entry arity threading** (§5) — how boot passes 1 vs 2 capabilities by the entry's
   declared signature, Path A (compiler-emitted call) and Path B (`.oimg` loader). Confirm
   **no format-version bump** is required; if Path B needs the signature, find the smallest
   addition. *Highest-risk item.*
2. **GC** (§3.1) — confirm `&SerialObj` (static, no heap oops) needs **no** new GC root and
   that the wrapper is reachable via the entry for its lifetime.
3. **`platform.c` extraction** (§3.3) — confirm the move is behaviour-preserving on both
   paths, including the Path-A `solidify.py` linkage (platform prims are reached from
   `solidified_image.c` on Path A — disjoint binary from `kernel_boot.c`).
4. **Confinement evidence** (§7) — determine whether an undeclared `serial` reference is a
   compile-time error or a runtime DNU in this toolchain, and pin the actual behaviour.
5. **`serial_handle` selector matching** — `strcmp` over 3 names is fine; note whether a
   per-module pid cache is worth it (likely not at 3 selectors / non-hot I/O path).

---

## 10. References

- ns-oevm master design `2026-06-20-ns-oevm-stdlib-design.md` — §2.1 three tiers, §5 native
  scope, §7 host-mockable Serial.
- 3c-3a streams design `2026-06-22-ns-oevm-stdlib-step3c3-streams-design.md` — String
  substrate, `platform string:`, EOF semantics this diverges from for a live port.
- `docs/NEWSPEAK_DIALECT.md` — #3 (bare indexables), #10 (immutable UTF-8 String), #12
  (byte-granular String streams); this cycle adds #13.
- Prior art (source-verified §2): `toitlang/toit` (`src/primitive.h`, `src/resources/uart_posix.cc`),
  `rmacnak/primordialsoup` (`vm/primitives.cc`), `smarr/SOMns` (`src/som/vm/Primitives.java`,
  `core-lib/Platform.ns`).
