# Node memory accounting and fleet liveness — design

- **Date:** 2026-08-10
- **Issue:** [#190](https://github.com/davidg238/rete/issues/190)
- **Status:** design approved (David, 2026-08-10). **Stage 1 (sub-project A) IMPLEMENTED
  the same day on branch `190-alloc-seam` and NOT merged** — its hardware gate found that
  `publishCheap` hangs a C6 deterministically. **Stage 2 (sub-project B, §4) IMPLEMENTED
  the same day on the same branch**, all host gates green, hardware gate and bell soak
  still owed; §4's own claims were corrected against the built code and the measurements
  are in §11.7. See §11 throughout for what the hardware changed in this document. Stages
  3–4 (sub-projects D and C) remain unplanned by design (D9). Plan:
  `docs/superpowers/plans/2026-08-10-alloc-seam-and-cheap-gauge.md`
- **Supersedes:** nothing. Extends `PROTOCOL-NSL.md` §3 with a fifth path prefix and
  builds `PROTOCOL-NSL-RECORDS.md` §2.4's already-specified `Panic`.

---

## 1. Why this is one program

The VM halts on an unchecked `malloc`. Nine crashes on record across three C6 boards, with
a measured lifetime of **43 m 54 s for c6spare at a 2 s check-in cadence and 10 h 41 m for
the bell at 30 s** — a 14.6× ratio against a 15× cadence ratio, agreeing to 3%. Nothing but
per-check-in decode work explains that agreement, and `platform asSymbol:` (`platform.c:1226`)
mallocs a buffer for every Symbol decoded off the wire and never frees it.

Four things were wrong, and they are specified together rather than in sequence because
each one is what makes the next one safe or provable:

| # | The defect | Why it can't be spec'd alone |
|---|---|---|
| 1 | Allocation failure halts the VM, silently | Its fix and the gauge for it are the same code |
| 2 | Nothing measured C-heap use, so the leak ran for a week unseen | The gauge is what proves the fix worked |
| 3 | A dying node cannot say why it died | Its loop-breaker leaves the node dead — invisible without 4 |
| 4 | Nothing noticed the fleet had gone quiet | The data was complete and unread for seven days |

**The instrument, not the collection, was the gap.** The console collector caught every one
of the crashes including the bell's on 08-03, and `exchanges` holds 9,809 heartbeat rows for
the bell. Nothing needed collecting that wasn't already collected. This is why "collect more"
is the wrong instinct and why item 4 is the one that would actually have caught it.

**The prior instrument measured the wrong heap.** `sys/metrics` reports the GC arena, which
is statically sized and was never under pressure; the leak was in the C heap beside it. A
gauge that exists is not a gauge that measures — see also #153, where `largestFreeBlock`
turned out to report the last failed request rather than heap health.

### The whole memory story in two numbers

The VM makes **six allocations and exactly one free.**

```
dispatch.c:1633   malloc(n + 1)         printString '#' prefix   → freed at 1635
dispatch.c:1669   malloc(strlen + 1)    asString Symbol name     → never freed
vm.c:1751         realloc(bindings)     vtable growth            → permanent by design
vm.c:1794         malloc(insts)         method instruction copy  → permanent by design
platform.c:1226   malloc(len + 1)       asSymbol — THE LEAK      → never freed, per wire Symbol
gc.c:342          malloc(FloatSlab)     float slab               → permanent by design
```

The crash site is the *only* correctly-paired allocation in the codebase. It dies because
the other five have eaten the heap first — `a2: 00000006`, a six-byte symbol name, so
`malloc(7)` returned NULL and `tmp[0] = '#'` stored through it.

Note for reading the first gauge output: three of the six are permanent by design — a vtable
and a method body live for the process lifetime. They appear as a one-time step during boot,
not a ramp. **A rising line after boot is a leak; a step is not.**

---

## 2. Decisions

**D1 — All four items ship as one program.** The gauge exists to verify the fix, and the
loop-breaker's safety depends on the liveness sweep. Specifying them apart would repeat the
mistake that opened this issue: measuring the arena because the arena was the gauge that
existed.

**D2 — The Symbol fix is to inline the bytes, the way `String` already does.** Not
NULL-checks-only, not an ownership flag, not interning. See §4.

**D3 — The C-heap gauge is the VM's own allocation accounting, not Zephyr's heap.**
`CONFIG_COMMON_LIBC_MALLOC`'s `sys_heap` is `static struct sys_heap z_n` in
`lib/libc/common/source/stdlib/malloc.c` — not exported, so `sys_heap_runtime_stats_get()`
is unreachable without a second local patch to `~/zephyr-esp32/zephyr`, which a west
re-fetch silently loses. Rejected on that ground and on a better one: a wrapper over the
VM's own six call sites reads *identically on host and target*, so the nsl suite can
exercise it, which a Zephyr-only reading never could.

**D4 — The gauge gets a new record at a new path, not a widened `Metrics`.**
`PROTOCOL-NSL-RECORDS.md` §4.4 already records this lesson against `LatencySpan`: adding a
concern adds a record, not a field, *"so this schema does not grow the way `Metrics` has."*
`Metrics` keeps all 61 B of its 256 B headroom. The cost is one more cell published per
check-in.

**D5 — On a fatal fault the node persists a bounded note and reboots, rather than halting.**
Halting leaves `sys/panic` — specified as kind E, must-deliver — permanently undeliverable,
and leaves an unattended node dead until a human arrives. See §5.

**D6 — Three consecutive panics with no successful check-in between them halt the node.**
Not "three for the same reason": a node cycling through different faults is not healthier
than one repeating a single fault, and matching on reason would make the breaker fail to
trip in the worse case. See §5.2.

**D7 — Gateway-authored observations live under a new `gw/**` prefix and never go on the
wire.** `PROTOCOL-NSL.md` §3 is explicit that porta writes only `goal/**` and `dbg/req`, and
§4.1 makes an accepted write to `obs/**` a lie — `obs/` is the node's own authority. A
verdict porta forms *about* a node fits none of the four existing namespaces. See §6.

**D8 — Silence is computed by a sweep that writes a real row, and cadence is inferred.**
`nodes.cadence_ms` was found to be a **dead column** — declared at
`porta/internal/space/schema.go:17` and never written or read anywhere in the Go tree — so
"porta already knows each node's cadence" was false. Cadence is inferred from the
`exchanges` history and written back into that column, retiring it rather than leaving a
second source of truth beside it.

**D9 — The gauge lands and runs against the known-leaky build before the fix does.** See §7.

---

## 3. Sub-project A — the allocation seam and `sys/cheap`

Every allocation the VM makes goes through one wrapper:

```c
void *oevm_malloc(size_t n);
void *oevm_realloc(void *p, size_t n);
void  oevm_free(void *p);
```

Six call sites, so this is a mechanical change with no judgement calls at the sites. The
wrapper carries a size header per block — malloc returns 8-aligned and the header keeps that
alignment — so it can report **bytes outstanding** exactly, which is the quantity that
predicted both crashes.

It does two jobs at once, which is the argument for it over a separate NULL-check pass and a
separate gauge:

- **The NULL check happens once**, in the wrapper, instead of six times at the sites.
  `#190`'s step 1 stops being six edits.
- **The counters are the gauge.** No second mechanism, no Zephyr patch, no host/target
  divergence.

### What it does on failure

The wrapper returns NULL to its caller, having incremented `fails`. Call sites become
NULL-safe individually — the failure is then a clean, raisable `#oom` rather than a store
through NULL. **This converts signature B from `#hardFault` into `#oom`**, which is what
makes §5's report say something useful: the CPU-level Store/AMO fault names the instruction,
the raise names the cause.

### The record

```
sys/cheap  →  CHeap { outBytes, outHigh, allocs, frees, fails }
```

Cell, published on the same check-in cadence as `sys/metrics`. Five fields, ~55 B encoded —
to be MEASURED and recorded in RECORDS §4.4 on landing, not estimated, per the precedent
that `Metrics`'s 195 B and `LatencySpan`'s 140 B are both measured figures.

Reached by a new prim appended to `PRIM_NAMES` (ids are 1-based positions in that table and
append-only, so no existing id shifts), mirroring the `heapStats`/`sysStats` pair that
already composes `Metrics` in `kernel.nsl`.

### Known blind spot, stated deliberately

Allocations made by lwIP, OpenThread or Zephyr itself are invisible to this gauge. That is
the correct scope — none of them are what leaked — and `fails` still catches a NULL arising
from *any* cause, because whoever exhausted the heap, the failure lands at a VM call site.

---

## 4. Sub-project B — inline Symbol bytes

### Why nothing frees `Symbol->name` today

It has three different owners: image-blob offsets resolved by `symbol_name()`
(`oimg.h:54`), static C literals (`->name = "debug:"`, `vm.c:1620`), and `malloc`
(`platform.c:1226`, `dispatch.c:1669`). A blanket `free` would free a literal or an image
offset. The "known-minor" comment at `platform.c:1218` masks a genuine ownership ambiguity,
not a small oversight. There is **no finalizer seam in `gc.c`** — the sweep coalesces free
blocks and calls no per-object destructor — which is what rules an ownership flag out.

### `struct String` already solved this

```c
struct Symbol {
    struct Object base;
    size_t length;
    char name[];        /* inline, NUL-terminated: allocate length + 1 */
};
```

`String` (`oevm.h:89`) holds `uint8_t bytes[]` inline and is allocated
`sizeof(struct String) + len`; its header comment explicitly contrasts itself with
*"Symbol (separate malloc'd `name`)"*. **String needs no blob accessor precisely because
inline bytes work identically in RAM and in an image** — which is the whole argument here.
`symbol_name()` collapses to `return s->name;` and the blob/RAM branch at `oimg.h:54`
disappears.

This deletes the malloc, the indirection, and the ownership question, and adds nothing.

The payload must be NUL-terminated because `symbol_name()` returns `const char *` consumed
by `strcmp` at more than twenty sites — hence `length + 1`.

### Four consequences that are real work

> Consequences 2–4 were **corrected after the work was built** (2026-08-10). The original
> §4 said "two static Symbols" and "four dynamic assignment sites", and named no lifetime
> rule and no image-version guard. Those claims are replaced below rather than annotated —
> a decision record that miscounts the work by two orders of magnitude is exactly the kind
> of half-claim a later session acts on.


**1. `gc.c` needs a `SymbolVtable` leaf case in `trace_one`.** Symbol currently falls through
to the generic word-count fallback, which today scans a `const char *` harmlessly. With
inline bytes that fallback would scan **raw symbol text as pointers**. `String` and
`RawBytes` already carry exactly this guard (`gc.c:542-550`), for exactly this reason.
This is a landmine, not a detail.

**2. Statically allocated Symbols need the GCC flexible-array-member extension — and there
are nine hand-written plus hundreds generated, not two.**

A static `struct Symbol` must keep a stable address in flash, because for several of them
the *address itself* is the VM's `==` result. The hand-written nine are:

| File | Symbols |
|---|---|
| `src/dispatch.c:278-279` | `true_sym_obj`, `false_sym_obj` |
| `src/kernel_boot.c:51-61` | `KSym__eq_eq`, `KSym_sender`, `KSym_value`, `KSym_accessViolation_`, `KSym_debug_`, `KSym_true`, `KSym_false` |

Beside them stands a **generated wall**: every interned Symbol in the solidified Path-A
image is emitted as a `const struct Symbol StaticSymbol_*` — **677 of them in the fixture
currently tracked at `src/solidified_image.c`**, and the count is a property of whichever
`.nsl` was baked, not of this change. They all come from **one line of Python**
(`nodus/nsl/tools/solidify.py:132`), so the wall is one edit, not 677.

**It is not optional.** `src/solidified_image.c` is still linked: `tools/run_c_tests.sh`
builds most of the C gate against `ENG_A` (line 34), and `zephyr/CMakeLists.txt` exposes
`-DTUVM_MODEL=A` as a supported board build. A design that treats Path A as dead would
have sized this consequence at two.

The extension was **verified on both toolchains before being used**, not assumed: host
`gcc` and `riscv64-zephyr-elf-gcc` both accept static initialisation of a flexible array
member, the statics land in `.srodata` (read-only, stable address, no RAM cost), and the
only diagnostic is `-Wpedantic`-gated — which neither build enables. If it ever needs
replacing, the fallback is a layout-compatible sized-storage struct behind a macro; record
it, don't build it now.

**3. Six dynamic assignment sites, not four — and the image format gets a version guard.**
The sites that build a Symbol at run time are `vm.c:122`, `vm.c:720`, `vm.c:1622`,
`dispatch.c:459`, `dispatch.c:1687` and `platform.c:1234`. Four pointed `name` at a C
string literal; **two malloc'd** (`platform.c` — the leak — and `dispatch.c`'s
`asString`). All six become `gc_allocate(SymbolVtable, sizeof(struct Symbol) + len + 1)`
plus a `memcpy`, so the malloc'ing and the literal-pointing sites collapse into one shape.

§4 originally accepted *"silently wrong, not loudly broken"* as the failure mode of a
serializer and a VM that disagree, mitigated only by landing them in one commit. **That
hole is now closed by a decision, not a discipline:** the `.oimg` `format_version` goes
**8 → 9** (`nodus/nsl/tools/oimg_serialize.py:80`). The header layout is unchanged; what
changed is the Symbol record, which no longer carries a `name` ref-word. The bump exists
so a v8 image meeting a v9 VM is **rejected at load** instead of misreading a ref-word as
inline text. One-commit landing is still right; it is no longer the only thing standing
between a stale pairing and a silent misread.

**4. Inline bytes end an accidental lifetime guarantee, so `symbol_name()` gains a rule.**
The leak was, perversely, providing lifetime safety: nothing ever freed the malloc'd name,
so a `const char *` from `symbol_name()` was **immortal** and no call site ever had to
think about it. Inline bytes end that — the pointer now lives exactly as long as the
Symbol object does.

The GC is mark-sweep and **non-moving**, so a *live* Symbol's bytes never relocate: the
hazard is collection, not compaction. All 30 `symbol_name()` call sites were audited
before the change and none holds the pointer across an allocation. The rule now lives in
the comment above `symbol_name()` in `src/oimg.h` — do not hold the pointer across an
allocation, a VM send, or anything else that can reach `gc_collect`; copy the bytes into a
local first.

### The image format changes with it

`oimg_serialize.py` writes `Symbol.name` as a blob reference today
(`_symbol_ref`, and the emit at L523-525). It must write inline bytes instead. **The
serializer and the VM land in the same commit**, and — per consequence 3 above — the
`format_version` bump to 9 is what makes a stale pairing *loudly* broken rather than
silently wrong. The original text of this paragraph accepted the silent failure; it no
longer applies.

---

## 5. Sub-project C — the panic path

### 5.1 Persist, then reboot

`CONFIG_RETAINED_MEM` is the wrong tool: its drivers are nRF-specific plus one generic RAM
backend needing a `zephyr,retained-ram` devicetree node — a driver, a Kconfig and a DT edit
to store twenty-odd bytes. The C6 already has `.rtc_noinit`
(`soc/espressif/esp32c6/default.ld:242`), reachable with `RTC_NOINIT_ATTR` and nothing else,
and it is the same mechanism the MagTag deep-sleep bell counter already uses.

```c
struct RebootNote {          /* RTC_NOINIT_ATTR on ESP32; .noinit elsewhere */
    uint32_t magic;          /* else uninitialised RAM reads as a panic report */
    uint32_t sum;            /* checksum — garbage must not become a report */
    uint32_t reason;         /* maps to Panic.reason */
    uint32_t mcause, mepc, ra;
    uint32_t bootId, monoMs;
    uint32_t consecutive;    /* §5.2 */
};
```

The seam is `k_sys_fatal_error_handler` (`kernel/fatal.c:37`, `__weak`), which Zephyr already
provides for override and which tuvm does not touch today — so nothing is being replaced. It
fills the note, then `sys_reboot()`. **It must not return**: Zephyr's default implementation
halts, and returning from a fatal handler is undefined.

On the next boot the bridge finds a valid note, `out:`s one `Panic` on `sys/panic`, and
clears the magic so it reports exactly once.

**The record already exists and needs no change.** RECORDS §2.4 pins
`Panic { reason, bootId, monoMs, detail, core }` and its rationale *is* this bug: *"a node
that dies of OOM can still report that it died of OOM, because the report is 60 bytes and
does not require allocating a buffer."* `core` is `nil` — blobs stay off the space. This is a
specified-but-unbuilt capability being built, not a new one.

`detail` carries the register triple as text, truncated to fit; RECORDS §2.4 names this as
the one place truncation is legal, *"because a partial reason beats no reason."*

### 5.2 The loop breaker

Rebooting trades a preserved crash scene for a self-healing node, and it converts a fault
loop into a reboot loop. A node that reboots forever is a noisy zombie on the network; a node
that has tried three times and stopped is a clean, findable failure.

```
on fatal:
    consecutive += 1                (note invalid → consecutive = 1)
    if consecutive >= 3:  print, keep the note, HALT
    else:                 sys_reboot()

on first successful check-in after boot:
    consecutive = 0
```

**A successful check-in is the reset**, and it invents no interval. It is also the honest
definition: a reboot loop is a node that cannot get far enough to report. The bell would
never trip this — each reboot is followed by hundreds of check-ins. A genuine boot-OOM
loop — the dongle that shipped dead through rungs 7–8 — never checks in at all, so it halts
on the third try. A fault that recurs weekly is an intermittent, not a loop, and stays out
of the breaker's way for free.

**Not "three for the same reason."** A node cycling through different faults is not healthier
than one repeating a single fault, and matching on reason would make the breaker fail to trip
in precisely the worse case.

The counter lives in RTC slow memory, which loses state when power is removed — so a human
power-cycling the board clears it. That is the semantic we want and it costs nothing.

**This is only safe because of sub-project D.** A halted node reports nothing; without
`gw/liveness` the breaker would trade a noisy zombie for an invisible corpse.

---

## 6. Sub-project D — `gw/**` and the silence sweep

### 6.1 The namespace

`PROTOCOL-NSL.md` §3: *"porta writes ONLY `goal/**` and `dbg/req`."* §4.1 makes an accepted
write to `obs/**` a lie, because `obs/` is the node's own authority. A verdict porta forms
*about* a node is neither node-authored nor deliverable to it, and fits none of the four
existing namespaces.

**`gw/**` — gateway-authored, never on the wire.** It adds a path prefix, not a table, an
endpoint or a verb, which is what D2 of the rewrite prescribes: the unit of extension is a
path.

Storage falls out for free. `direction` gains a third value `'gw'`, and because
`OutboundSince` (`porta/internal/space/store.go:150`) already filters `direction = 'out'`, a
`gw/` row is excluded from the wire **by construction rather than by remembering to filter.**

`PROTOCOL-NSL.md` §3's picture gains a fifth row that crosses no boundary.

### 6.2 The sweep

A fourth goroutine in `porta serve`, alongside the UDP, blocks and HTTP loops that
`awaitServeExit` (`porta/internal/portacli/serve.go:136`) already composes.

```
every 30 s, per node:
    gaps    = last 10 inter-arrival gaps from exchanges
    cadence = median(gaps)                      → written back to nodes.cadence_ms
    silent  = now - last_seen_ms > 3 × cadence
    if silent != current gw/liveness:
        write gw/liveness = #silent | #live     (cell + event row)
```

**Writing only on transition** is what makes `porta log` a list of the times the fleet went
quiet, rather than a row every 30 s.

**A node with fewer than three exchanges gets no verdict** — there is no cadence to infer, so
`gw/liveness` stays unwritten rather than guessing.

### 6.3 Why not a lazy read-time view

A lazy view cannot do this job even setting aside that it produces no row. The failure it
must catch is *porta hearing nothing*, and a system that only evaluates when read is silent
exactly when everything is. The ticker is not a convenience.

---

## 7. Staging, and how each step is falsified

The gauge lands **first** and runs against the **known-leaky** build.

Sub-projects are lettered by §, staged by number — the two orders differ, which is the point.

| Step | What lands | Board | The prediction that must hold |
|---|---|---|---|
| 1 | **A** — allocation seam + `sys/cheap` | **c6spare** | On the unfixed build, `outBytes` **rises**, and the rise extrapolates to heap exhaustion at c6spare's measured 43 m 54 s. **If it does not rise, the gauge is blind and step 2 proves nothing.** |
| 2 | **B** — inline Symbol bytes + serializer | c6spare, then the bell | On the same gauge, `outBytes` goes **flat** after boot. `fails` stays 0 across a run longer than 10 h 41 m. |
| 3 | **D** — `gw/**` + sweep | either | Halting a node by hand flips `gw/liveness` to `#silent` within 3 × cadence + 30 s, and one event row records the transition. |
| 4 | **C** — panic note, reboot, loop breaker | c6spare | A deliberate fault (there is already a `prj_esp32c6_faultgate.conf` build for exactly this) produces one `Panic` on `sys/panic` after the reboot, and three faults with no check-in between them halt the node — with step 3 already watching. |

Step 1 before step 2 is the whole point. Landing them together would leave a flat line that
proves nothing — you would never have seen the instrument register the leak it was built to
see, which is precisely the arena gauge's failure mode.

The second pair is ordered by the same argument. **The sweep lands before the loop breaker**,
because the sweep must be provably working against a node halted by hand before the breaker
is allowed to halt one on its own — otherwise the first node the breaker stops is also the
first node nothing is watching.

### Which board, and why the bell stays halted (David, 2026-08-10)

**Step 1's leaky-build reading comes from c6spare at its 2 s cadence — 44 minutes to a
result rather than ten and a half hours.** The 14.6× cadence agreement already established
that the two boards are measuring the same phenomenon at different speeds, so c6spare is a
sound proxy for the *instrument* question ("does the gauge see the leak?") even though it
is a poor proxy for field conditions.

**The bell stays halted through steps 1 and 2**, and comes back only on the fixed build. It
is the only unattended, wallwart-powered, observe-only node on the bench — the closest thing
to a field deployment — and its value is as a *clean* long-run witness for step 2's flat
line. Running it deliberately leaky to re-observe a rise already measured twice would spend
ten hours to learn nothing new and would contaminate the one board whose long record is
worth keeping.

This is what makes step 2's prediction meaningful: the bell's first post-fix run is a run
against a board with a *known, measured* 10 h 41 m lifetime on the old build. Passing 10 h
41 m with `outBytes` flat is a direct comparison, not an absence of evidence.

`c6spare` is the attached disposable board and `TARGET_DEFAULT_BOARD`; resolve both through
the collector env files rather than a table, and never by a ttyUSB number.

---

## 8. Constants chosen, not derived

Recorded here so the deferral is falsifiable rather than vague.

| Constant | Value | Standing |
|---|---|---|
| offline window `k` | 3 × cadence | **Inherited.** The rewrite design records it at line 275 as *"offline = k × cadence, k = 3 (v1's constant, kept)"* |
| cadence inference window | last 10 gaps | **Chosen.** Robust to one missed check-in, short enough to track a cadence change. Earns revision from the first fleet with a node whose cadence changes |
| sweep tick | 30 s | **Chosen.** A floor on detection latency, below the shortest cadence in use. Earns revision if it ever costs measurable CPU |
| loop-breaker threshold | 3 panics | **Chosen** (David, 2026-08-10). Earns revision from the first real reboot loop, whose shape will say whether 3 was too eager |

---

## 9. Out of scope

- **Interning Symbols.** Considered and rejected: it adds a table and a lifetime policy to
  solve a problem that inline bytes delete outright.
- **A finalizer seam in `gc.c`.** The sweep coalesces free blocks and calls no per-object
  destructor. Adding one is a mechanism; D2 removes the need for it.
- **Blobs on the space.** `Panic.core` is `nil`. Bulk transfer stays deferred to a CoAP
  blockwise plane.
- **Measuring Zephyr's own heap.** See D3. Revisit only if a failure is traced to an
  allocation the VM did not make.
- **NVS-backed panic history.** RTC RAM is cleared by a power cycle, which is the semantic we
  want; delivered `Panic` records already carry the history to porta for every case that is
  not a loop.

---

## 10. Promotion — what absorbs this when it closes

Per the promotion rule, this dated design stays behind as the historical "why"; the durable
knowledge goes to:

| Knowledge | Mainline document |
|---|---|
| `CHeap` shape + **measured** encoded size | `protocol/PROTOCOL-NSL-RECORDS.md` §4.4 |
| `gw/**` as a fifth prefix; `direction = 'gw'` | `protocol/PROTOCOL-NSL.md` §3 — the model picture and the namespace law. **Not** §4.2, which is the *node-side* path map and `gw/` is not node-side |
| `Panic` marked built rather than specified, and the `sys/panic` shape contradiction RECORDS §2.4 flags as unresolved closed in favour of §2.4's resolution | `protocol/PROTOCOL-NSL-RECORDS.md` §2.4 **and** the contradicting paragraphs in `protocol/PROTOCOL-NSL.md` — both, or the contradiction survives |
| Silence detection, the sweep, the retired `cadence_ms` | `porta/docs/ARCHITECTURE.md` |
| Symbol layout, the `trace_one` leaf rule, the `.oimg` format change | `review/lib/` VM doc + `review/tests/CLAIMS.md` |
| Reboot-not-halt as node failure behaviour | `review/program-sequence.md` |

---

## 11. Addendum — what the stage 1 hardware gate changed (2026-08-10, same day)

Stage 1 was implemented and run on hardware the day this design was written. Three things
in the sections above need correcting or extending, recorded here rather than silently
edited so the reasoning stays inspectable.

### 11.1 §7's verdict table is INCOMPLETE — it has a fifth outcome

All four rows assume the node runs. The outcome that actually occurred is not among them:

| Reading | Verdict |
|---|---|
| **The instrumentation hangs the node** | **Not a reading at all.** Score nothing. A node that stops checking in produces no evidence about the leak, and the flat line it leaves behind is indistinguishable from row 3's "gauge is blind" |

This is the failure mode a verdict table most needs and is least likely to contain, because
a table is written by someone imagining the measurement working. **Any future gauge design
in this program should carry this row.** The cost of omitting it is precisely the
misdiagnosis §7 was written to prevent, arriving through a door the table left open.

### 11.2 D9 (gauge first, then fix) is VINDICATED, and by a route it did not predict

D9 argued the gauge must land first so the leak could be seen to rise. What it actually
bought was different and larger: **the gauge validation run caught a target-only hang in
the instrumentation itself, before the Symbol fix was ever planned on top of it.** Had both
stages shipped together, the hang would have been attributed to the Symbol change — a
strictly harder debugging problem, on the branch that matters most.

The argument for D9 is therefore stronger than the one recorded in §7, and it generalises:
*an instrument must be proven not to disturb its subject before its readings mean anything*,
which is a separate claim from *an instrument must be proven to register the effect*.

### 11.3 What §3's seam is now KNOWN to be, on hardware

Bisect on a real C6 (five builds, same board and command, all `--pristine`) exonerated the
entire C-level design of §3:

- the seam (`oevm_malloc`/`oevm_realloc`/`oevm_free`) — clean
- all six call-site conversions and the NULL handling of §3's "What it does on failure" — clean
- **the 8-byte size header** — clean, tested by removing it and observing the hang persist.
  Note that §3's justification for the `uint64_t` union (alignment faulting on target only)
  was reasoning about a real hazard that turned out not to be this one.
- prim 152 and the `PRIM_NAMES` append — clean **as to presence only.** The build that
  carried prim 152 without `publishCheap` never CALLED it, so the bisect tested its linkage,
  not its execution. It was exonerated properly a few hours later — see §11.5 — by a build
  that executes it every check-in and runs. Recorded because "exonerated by bisect" was
  written here first and is the kind of half-claim a later session acts on.

The hang is in the nsl half — `Kernel>>cheap` / `publishCheap` — which §3 treated as the
uninteresting part. **The C was the part everyone reviewed; the four lines of nsl that
publish a cell every check-in were the part that broke a node.**

`sys/cheap` itself is confirmed working end to end: it reaches porta from a real C6 over a
real Thread mesh.

### 11.5 The hang is `CHeapStats fromList:`, and the gauge's reading (2026-08-10 evening)

Three builds, one variable each, same board (`c6spare`) and same command:

| build | prim 152 executes | `CHeapStats` instantiated | result |
|---|---|---|---|
| `fcfb119`, 3 boots | yes | yes | **wedged at tick 2-3, 3-for-3** |
| constant record | no | no | clean past the verdict |
| raw prim + `basicAt:` | **yes** | no | clean to tick 610-614 |

So **prim 152 is innocent and `CHeapStats fromList:` is fatal**; the mechanism is still
unknown and is now rete#192, whose only reproducer is the class this branch keeps for that
purpose. The fix makes `cheap` read the raw prim Array exactly as `metrics` reads
`platform heapStats` — the wedge is closed by REMOVING a mechanism, not adding one.

**The wedge is an event, not a duration.** All three `fcfb119` boots die on the first
check-in after `TAP bridge-health done`, never a tick earlier or later. `emitVerdict` writes
`diag/spans/ctl → #stopped`, which dumps 12 `LatencySpan` cells into the very next push;
`pushFrame` then trims ~13 rows with a full frame re-encode per iteration. That is the
load window, and reading it as "a few ticks in" is what made it look like a slow failure.

**A second, unrelated failure lives at ~613 check-ins, and it predates this branch.** Three
runs — including one with no `publishCheap` at all — stop at tick 610, 613 and 614. The
bisect's master control never reached it: master was reflashed at tick 588, twenty-five ticks
short, on a ~4-minute observation window. **Master is untested there, not exonerated.**

**It is driven by check-in COUNT, not wall time,** which is the one measurement that tells a
leak from a clock. Halving the cadence to 1000 ms halved the wall time and left the tick
count alone:

| cadence | died at tick | wall time |
|---|---|---|
| 2000 ms | 613, 614 | 20m 26s, 20m 31s |
| **1000 ms** | **610** | **10m 20s** |

The gauge agrees from the other side: outstanding blocks track ticks, not seconds
(5 878 at tick 589 with a 1 s cadence; 6 094 at tick 614 with a 2 s cadence). At the cliff
the C heap holds **~6 000 blocks averaging 4.8 bytes of payload** — 29 KB of payload that,
with an 8 B header plus newlib's own per-chunk overhead and alignment, costs on the order of
**140 KB of the C6's 190 KB**. `fails: 0` throughout: the VM's own allocations never failed,
so whatever died first was an allocation OUTSIDE the seam (OpenThread or the net stack),
which is also why the failure is silent.

Thousands of ~5 byte blocks is the signature of the interned-Symbol-name leak stage 2
targets. **§7's prediction is confirmed in substance and wrong in units:** it framed the
death as 43m54s of wall time, but the mechanism is per-check-in, so the figure only holds at
the 2 s bench cadence and says nothing about a field node on a slower one. Any future
statement of this leak's lifetime must be in check-ins.

### 11.6 Two more readings that were wrong, and one that was a measurement artifact

- **`handlesLive: 0` / `handlesHighWater: 0` is NOT a wedge signature.** It was named the
  best thread to pull. A demonstrably HEALTHY node on this build reports exactly those zeroes
  while ticking; the healthy `c6-bell` 2 came from a different app and era. Chasing it would
  have cost a session.
- **`c6spare` was not left "clean and ticking".** It panicked 23 minutes after that sentence
  was written, on `VM GC Error: Out of Memory (requested 4104, used 131064 of 131072)` — the
  GC ARENA, on a build with no `publishCheap`. Board state recorded at close-out is a claim
  with a shelf life; re-read the console before trusting it.

  That OOM is **probably downstream of the §11.5 cliff rather than a second leak**, and the
  three runs line up to say why: all reach the wall at the same check-in count, but the
  build without `publishCheap` then limped on for 56 further ticks with `acked` FROZEN —
  nothing it pushed was being accepted — its tick period decaying 2.0 s → 6.2 s, before the
  arena filled. A backlog accumulating in the arena once acks stop fits that, and a decaying
  tick period is what a GC does as live data grows. **Untested; recorded as a hypothesis.**

  > **ADDENDUM 2026-08-11 — CORROBORATED on hardware, by accident, and by a cleaner route
  > than could have been designed on purpose (rete#199).** Bringing up rete#197 flashed
  > `c6spare` with shortened wire labels while the gateway still ran the old binary, so every
  > frame was refused (`malformed message: missing #bootId`) and `acked` never left 0. The
  > node then died exactly as this paragraph predicts:
  >
  > | | here, 2026-08-10 | rete#199, 2026-08-11 |
  > |---|---|---|
  > | `acked` | frozen | frozen at 0 |
  > | tick period | 2.0 s → 6.2 s | 2.0 s → **7.2 s**, monotonic |
  > | fatal request | **4104 B** | **4104 B** |
  > | arena at death | 131 064 / 131 072 | 127 680 / 131 072 |
  >
  > **Why it is stronger evidence than the event that prompted the hypothesis:** the acks
  > froze for a reason with *no memory component whatsoever* — a gateway that did not
  > recognise a field name — and the §11.5 leak was already fixed and gated on this same board
  > (§11.7, 0 outstanding blocks across 677 check-ins). The original event could not separate
  > "backlog" from "second leak" **because the leak is what froze the acks.** This one has no
  > leak in it and the same thing happened.
  >
  > So arena OOM here is most likely **one mechanism with two triggers** — a backlog
  > accumulating once acks stop — rather than two causes. The known triggers are the §11.5
  > cliff (rows stop being *accepted*) and a wire mismatch (rows stop being *parsed*);
  > anything else that freezes acks is a third.
  >
  > **This sharpens `c6-bell`'s soak rather than mooting it.** The bell is healthy and acking,
  > so no backlog can be accumulating in it — **if the bell OOMs anyway, that is the second
  > leak this section was worried about**, because the competing explanation now has an
  > independent home and can no longer absorb the result.
  >
  > **RESOLVED the same day, and the answer is NOT accumulation — see the correction below.**
  > What stood here was a reading of the 4104 B as a *collection* doubling (`OrderedCollection`
  > and the hashed collections both grow by `2 * contents size`), with `pending` noted as the
  > obvious-but-probably-wrong candidate. The cap was the right clue and it pointed elsewhere.
  >
  > **CORRECTION 2026-08-11, second pass — it is FRAGMENTATION, and the arena is two-thirds
  > empty when the node dies.** Reproduced off hardware at the C6's exact memory
  > configuration (`-m32`, arena 131 072, `VM_STACK_INIT_WORDS=4096`), with a matched-gateway
  > control: the refused leg died at **114 s / ~55 check-ins**, the control ran **900 s / 450
  > check-ins** and lived. Measured in the terminal OOM handler at the fatal allocation:
  >
  > ```
  > request=4104   live=44396   largest_free=3976   gc_cycles=428   t=98 s
  > ```
  >
  > **34% of the arena was live. ~87 KB was free. No 4 KB extent existed.** The healthy
  > control held *more* live data (78 132 B) at a **19× lower collection rate** (0.23/s vs
  > 4.4/s). So the dying node is not holding more — it is churning more.
  >
  > The 4104 B is `platform basicNewBytes: 4096` from **`ByteBuilder>>grow`**
  > (`kernel.nsl:683`) — a flat byte buffer, not a slot array. It is therefore a **constant,
  > not a boundary**, which is the real reason three independent events requested the
  > identical figure. `pending` could never have been the answer: it is capped at 64 and
  > `maxRows` caps each prefix scan at 8, so the row list tops out near 88.
  >
  > **The generator is `pushFrame`'s trim loop** (`kernel.nsl:1945`): it encodes the whole row
  > set, then removes one row and **re-encodes**, repeatedly. With `acked` frozen the first
  > encode is the full backlog (>2048 B, hence the 4096 B builder) and the trim then runs ~80
  > more encodes, each climbing its own ladder of abandoned power-of-two buffers — ~80 ladders
  > per check-in against a healthy node's one, twice a second. That, not accumulation, is what
  > shatters the arena.
  >
  > **So the one-mechanism-two-triggers reading above survives, with its mechanism replaced:**
  > the shared mechanism is *allocation churn from a frozen `acked`*, not *a backlog filling
  > the arena*. Everything the table matches still matches. What changes is the fix — bounding
  > a queue cannot help a node that dies with two-thirds of its arena free.
  >
  > **And the sharpened reading of `c6-bell` holds unchanged**, for the same reason: the bell
  > is acking, so it never enters the trim-loop regime. If it OOMs anyway, that is still the
  > second leak this section was worried about.
  >
  > **FIXED the same day (rete#161).** `pushFrame` no longer encodes the frame larger than it
  > will be sent: it starts from what fit last time and climbs a row at a time, so the
  > accumulator never exceeds the batch cap by more than a row and the 4 KB rung is never
  > reached. The refused leg went from dying at ~55 check-ins to surviving 450 — the
  > control's own count. **One attempt failed first and is worth more than the fix**: making
  > the trim a bisection removed ~9 of every 10 encodes and bought only 114 s → 168 s, which
  > is what proved the trim walk was never the cause. Every version of it encoded all ~70
  > rows *first*, and that single allocation was the whole of it.
  >
  > **What the fix does NOT address, and it is the half this section cares about most:** a
  > refused node is now indefinitely silent instead of dead in 2.5 minutes. `misses` still
  > counts no-reply, so it still reads 0 under total refusal, and the `err` reply's `msg`
  > is still discarded. The liveness question this document exists for is therefore *more*
  > open than before, not less — rete#199 stays open on exactly that.
  >
  > **A first host reproduction was retracted** (it set `OEVM_HEAP_SIZE` as an environment
  > variable, but that is a compile-time macro, `gc.c:18-19`, so it measured the 16 MB host
  > default). **rete#179, which had blocked doing it properly, is closed**: the host could not
  > boot at the device arena because `VM_STACK_INIT_WORDS` defaults to 16384 and a 64-bit word
  > is 8 B — `16384 × 8 = 131072`, the C6's whole arena, by coincidence. See
  > `docs/results/2026-08-11-host-arena-floor.md`.

  The two runs WITH `publishCheap` stopped dead at the cliff instead and never got there,
  which is the part of the picture a single-run reading would have missed.
- **A two-point read of `liveBytes` says nothing.** It sawtooths with every GC cycle, so two
  samples at different phases yield any trend you like — including, in this session, a
  confident and entirely wrong prediction of imminent arena exhaustion. The arena is fine;
  only the 15:21 OOM above is real, and it is one observed event, not a trend.

### 11.4 One undocumented mechanism the plan missed

`dispatch.c` carries a compile-time parity guard (`prim_names_enum_parity_check`) that must
advance with the last prim id or the build hard-fails on a negative array size. It is not in
any of the nine enumerated source lists the project's lore warns about, so a file survey does
not find it. Anyone appending a prim needs to know it exists.

### 11.7 What stage 2 measured (2026-08-10)

Stage 2 — inline Symbol bytes, §4 — is built and every host gate is green. This is what it
measured. **Everything here is quoted in check-ins, never in minutes**: §11.5 established
that the failure is driven by check-in count, so §7's "43 m 54 s" was right in substance and
wrong in units, and a wall-clock figure only holds at the bench cadence it was taken at.

#### The check-in path, end to end (host)

Two runs of the real check-in path (`nodus/tuvm/tests/porta_sync_host.sh`'s harness — the
same `PortaBridgeNode.nsl`, the same `porta serve`, a 2000 ms cadence), ticks 1 → 150, with
143 labelled `sys/cheap` rows in each:

| build | outstanding blocks (`allocs − frees`) | slope | `outBytes` |
|---|---|---|---|
| pre-fix `703547e` | 6 → 1 496 | **+10.000 blocks / check-in** | 34 → 7 186 (+48 B / check-in) |
| post-fix | 0 → 0 | **+0.000** | 0 → 0, `outHigh` pinned at **17 B** |

Two-point and least-squares over all 143 rows give the same number, **R² = 1.00000** — the
pre-fix series is *exactly* linear in check-ins, not approximately. For scale, §11.5's board
reading is ~8.6 blocks/check-in (5 878 outstanding at tick 589, 6 094 at tick 614): same
phenomenon, same order, same unit.

**The cross-check is stronger than either slope.** The two runs are step-for-step identical
apart from the leak — at *every* tick, pre-fix `frees` == post-fix `allocs` == post-fix
`frees`, and the two independently fitted alloc rates differ by exactly 10 (245 vs 235). The
fix removed precisely the allocations that were never freed and changed nothing else about
the workload. Post-fix the node still makes ~235 C-heap allocations per check-in and frees
all 235.

#### The prim in isolation (host)

`nodus/tuvm/tests/test_symbol_inline.c`, 200 calls to `PRIM_pf_asSymbol`:

| build | `allocs` | `outBytes` |
|---|---|---|
| pre-fix `703547e` | +200 | +2 890 |
| post-fix | **+0** | **+0** |

#### Three caveats that must travel with these numbers

These are how the next reader avoids being fooled by them.

- **`allocs − frees` is not universally "outstanding blocks".** `oevm_realloc` (`gc.c:98`)
  bumps `allocs` and never `frees`, so every realloc inflates the difference by one with
  nothing leaking. The only realloc site on this path is vtable binding growth
  (`vm.c:1757`), quiescent by tick 21 — which is *why* post-fix reads a flat **zero** rather
  than a flat small constant. A realloc-heavy workload would drift upward while perfectly
  clean.
- **`outBytes` counts payload only** — not the 8 B block header, not the allocator's own
  per-chunk overhead. The real pre-fix cost was on the order of **160–320 B per check-in**,
  not 48. **Quote the block count; treat bytes as a lower bound.**
- **Mean leaked block is ~4.8 B on the check-in path and 14.45 B in the prim read.** Not a
  contradiction — different symbol populations (wire symbols are short; the prim test drove
  longer names). The **block count** is the quantity that matches across all three
  measurements, host and board.

#### The hardware gate — CONFIRMED on `c6spare` (2026-08-11)

§7 step 2's prediction is no longer unconfirmed. Commit **`e52a86e`**, flashed `--pristine`
with the conf named explicitly, `PortaBridgeNodeC6.nsl` at the 2 s cadence — read from
porta's own rows rather than from this document's recollection, because the leaky run's rows
were still in `soak-2026-08-04.db`. **Both sides of this comparison are live data.**

| reading | leaky build (2026-08-10) | fixed build `e52a86e` (2026-08-11) |
|---|---|---|
| outstanding blocks (`allocs − frees`) | 6 088 @ tick 610 | **0** |
| slope | **+10.000 / check-in** | **+0.000**, across **677 consecutive rows** |
| `outBytes` | 29 238 and rising | **0** |
| `outHigh` | rising with it | pinned at **17 B** |
| C-heap allocations per check-in | 245 | **235** |
| alive past tick 614 | **no** — died 610 / 613 / 614, three runs | **yes** — reached **702** |
| `fails` | 0 | 0 |

**The strongest line is not the slope, it is `distinct: [0]`.** Outstanding blocks were 0 at
*every one* of the 677 sampled check-ins — minimum 0, maximum 0, `allocs == frees` at every
sample (270 871 each at the end of the run). §11.6 records how a two-point read of a
sawtoothing counter yields any trend you like; this is not vulnerable to that, because there
is no second value to sample. An earlier run of the same fix on the same board, before the
build was made reproducible, gave the identical result over 744 rows.

The board also reproduces the host's decomposition exactly: 245 → 235 allocations per
check-in is the same pair the host fitted independently, and 29 238 / 6 088 = **4.80 B** mean
block matches §11.5's "~6 000 blocks averaging 4.8 bytes". **The ten leaked allocations per
check-in are gone and nothing else about the workload moved.**

**The bell agrees, on a different app and a different cadence.** It was restored to service
on the same commit at 08:34 — `ShipsBellAlarmNodeC6.nsl`, checking in as `c6-bell` every 30 s
with the RTC alarm armed — and reports the same `distinct: [0]`, `outBytes 0`, `outHigh 17`,
`fails 0` over its first 48 rows. That matters because it is not the same workload: the leak
lived on the *decode* path, so a second app on a 15× slower cadence reproducing the flat line
is evidence the fix is in the VM and not in one node's behaviour. Its >10 h 41 m run is still
accumulating.

#### The gate found a bug the host could not: a stale per-target layout

The first flash of the fixed build **panicked within a second of Thread attaching**:

    VM Error: Message 'undOuter:slotCount:' not understood by receiver 0x42800318
    abort()  ->  ZEPHYR FATAL ERROR 4: Kernel panic on CPU 0

The selector is `__boundOuter:slotCount:`; the VM read it **four bytes late** and lost
exactly `__bo`. Model B bakes struct offsets ahead of time from a per-target layout JSON that
is *captured from hardware* and checked in. §4's flip moved `Symbol`'s bytes inline and
updated `tools/dump_layout.c` — but not the three artifacts that tool exists to produce.
`zephyr/layout-esp32c6.json` still described a `name` **pointer at offset 4** while the VM
read inline bytes at offset 8.

Three properties made it invisible until a board ran it, and all three generalize:

- **No host gate can see it.** The nsl suite rebuilds `dump_layout` and regenerates its
  layout every run, so the host is self-consistent *by construction*. Only a cross-compiled
  target reads a **stored** layout.
- **The `format_version` 8 → 9 bump was added by this very stage for the hazard "an image
  built by one and loaded by the other is silently wrong" — and it cannot catch this.** Both
  ends agreed on 9. They disagreed about the **ABI**, not the format. A version guards the
  shape of the envelope, never the meaning of the fields.
- **The image loads, boots, joins the mesh and passes three bridge-health TAPs before dying**,
  because the first Symbol read at the wrong offset is a send rather than the load.

Fixed by regenerating the file the documented way (`zephyr/layoutdump/` flashed to the board,
its console output *is* the file) and by a guard in `oimg_serialize.py` that refuses any
layout whose inline payload claims to be a pointer or does not begin exactly at the end of
its header. The guard was verified against the real artifact: it **rejects the exact
`layout-esp32c6.json` that shipped the panic**, and it also rejects `layout-esp32.json` and
`layout-esp32s2.json`, which are stale in the same way and cannot be regenerated without
those boards.

#### One more way a green build is not a reproducible build

`c6spare`'s successful image got its Thread configuration from a **stale CMake cache**, not
from the command line that produced it: `build-esp32c6-B` still held
`EXTRA_CONF_FILE=prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf;prj_esp32c6_thread.local.conf`
from an earlier session. The image was correct and the measurement above is valid — it really
did join the real mesh and reach porta — but **re-running that command in a clean tree yields
a radio-less image and a node that binds nothing**, which is exactly what happened to the bell
when it was given a fresh build directory. Both boards were subsequently reflashed
`--pristine` with the conf named explicitly. Same family as the layout bug: an artifact
*outside the source tree* silently supplying correctness.

#### Three findings that outlive this work

- **`OEVM_GC_STRESS` is blind to use-after-sweep.** A root was deliberately removed from
  `tests/test_cbor.c` and the stress gate still passed. The collector is non-moving and
  sweeps *within its own arena* rather than calling `free`, so swept bytes stay physically
  readable and the stale oop keeps working; ASan cannot see it either, for the same reason.
  **"Green under `OEVM_GC_STRESS`" is not evidence that an oop was rooted.**
- **`trace_one`'s leaf arms were unguarded.** With the new `SymbolVtable` arm short-circuited,
  the entire pre-existing 33-test C gate passed — the guard §4 called "a landmine, not a
  detail" was untested by everything already in the tree. `tests/test_symbol_inline.c` now
  guards the Symbol arm with a decoy/trap object; **`StringVtable` and `RawBytesVtable`
  remain unguarded** and the idiom is reusable verbatim.
- **`run_tests_oevm.py` compiles Path A *into* `src/solidified_image.c`**, and `run-one.py`
  does not restore it — so any `.nsl` bake sweep silently replaces the tracked fixture. It bit
  this branch once: a sweep swapped the fixture's identity from `MethodCallTest` to
  `SmiBoundaryConstantTest` and it was nearly committed. The fixture also carries **~10 715
  lines of `.c` plus 6 948 of `.stmap` of pre-existing drift**, dating to before the
  2026-07-18 `.ns` → `.nsl` rename — so `git diff` on it is not a reliable tripwire either.
