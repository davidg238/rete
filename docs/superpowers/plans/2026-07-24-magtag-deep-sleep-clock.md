# MagTag Deep-Sleep Ship's-Bell Clock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-architect the MagTag ship's-bell clock (tuvm#94 stage 5) from an always-on reactor into a mostly-asleep **run-once** appliance: each timer wake is a fresh VM that reads RTC-retained state, strikes, optionally syncs+paints, writes state, and deep-sleeps.

**Architecture:** State that must outlive the dead VM lives in a fixed 10-field SMI struct in ESP32-S2 `.rtc.data` (device) or a file (host, for re-exec testing), reached through four new platform prims. The node collapses to a straight-line script — no channels, no VM timers, no drain loop. The half-hour index is a **monotonic ratchet** (only ever increments) so a backward NTP correction can never double-strike.

**Tech Stack:** nsl (node + kernel forwarders), C (platform.c host leaves + dispatch, zephyr/src/main.c device leaves), ESP-IDF sleep HAL under Zephyr, Python (nsl-tests runner + a re-exec sequence harness).

## Global Constraints

- **Design of record:** `~/workspace/tuvm/docs/design/2026-07-24-magtag-deep-sleep-clock-design.md`. Every task implements a section of it; cite the section.
- **Branch:** `feat/magtag-esp32s2-port` (already checked out). Do not branch.
- **New node file:** `~/workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl`. **Leave `zephyr/MagTagClockNode.nsl` (the witnessed stages 1-4 reactor) untouched** — it stays as the always-on demo sample beside the deep-sleep sample (David's decision, 2026-07-24).
- **Prim recipe is FIVE sites, counted-not-computed** (see [[nsl-grammar-gotchas]], `tuvm-vm-source-lists`): `src/prim_ids.h`, `src/dispatch.c` (enum + `PRIM_NAMES` + parity typedef), `src/platform.c` (host leaf + dispatch case), `zephyr/src/main.c` (device leaf), `nsl-tuvm/lib/kernel.nsl` (forwarder). New prim ids are the **1-based position in `PRIM_NAMES`**, arrived at by counting, never `130+1` arithmetic.
- **No new `.c` files** — every leaf extends an existing translation unit, so the nine-source-list churn does NOT apply here.
- **nsl has no modulo** (tracked for a real fix in **tuvm#97**; this rung keeps the workaround). `x mod n` is spelled `x - ((x // n) * n)` — use the identical spelling the node and Task-3 units share (the `05_ntp_catchup.nsl` precedent).
- **Test the shipping node, not a copy of it.** The file-backed RTC seam lets the REAL `MagTagDeepSleepNode` be driven by a Python harness that pre-seeds `.rtc.data`-equivalent state and scripts NTP per wake. Only Task 3's tiny pure formulas are re-spelled as nsl units (minimal, precedented). No task copies the node body into a test file.
- **An app node's `platform` is the Kernel instance** ([[nsl-app-seam-vs-kernel]]): `Array`/`basicNew:indexable:` are unreachable; a new kernel class needs an explicit accessor. The RTC seam is therefore an indexed SMI get/put, not a struct-marshalling class.
- **Every RTC field is a 30-bit SMI** (≤86399 sec-of-day, ≤14400 commanded sec, index ≤47). Signed fields (`driftPpm`, `drift24Max/Min`) fit int32.
- **Test path is B (.oimg) only** ([[no-path-a-suite]]): run host tests with `OEVM_PATH=B` / `run-one.py <file> B`.
- **Single-test inner loop** ([[nsl-single-test-runner]]): `python3 ~/workspace/nsl-tuvm/tools/run-one.py <file.nsl> B`. A SKIP proves nothing (returns before compiling) — require PASS.
- **Host-leaf env-injection convention** (existing, mirror it): `OEVM_NO_BELL`/`OEVM_NO_NTP`/`OEVM_NTP_SECONDS`. New this rung: `OEVM_RTC_FILE` (persist RTC struct across process re-exec — a reusable test feature per David, not a one-off).

### RTC field table — the ONE source of truth (design §3)

Index order is identical in C (`platform.c` host array, `main.c` device array) and in the node's `rtc*` field-index constants.

| # | constant name (nsl + C comment) | field | notes |
|---|---|---|---|
| 0 | `rtcMagic` | `magic` | validity guard; `.rtc.data` undefined on cold power-on |
| 1 | `rtcLastHalf` | `lastStruckHalf` | **the ratchet** — only increments |
| 2 | `rtcBootTicks` | `bootTicks` | strikes since cold boot → `up` |
| 3 | `rtcAnchorSec` | `anchorLocalSec` | true sec-of-day at last good sync |
| 4 | `rtcElapsedSec` | `elapsedCommandedSec` | Σ commanded sleeps since anchor |
| 5 | `rtcDriftPpm` | `driftPpm` | signed, displayed |
| 6 | `rtcStaleSec` | `staleSec` | since last good sync → `sync Nd/NNh` |
| 7 | `rtcDrift24Max` | `drift24Max` | rolling max over 6 syncs |
| 8 | `rtcDrift24Min` | `drift24Min` | rolling min over 6 syncs |
| 9 | `rtcSyncsWin` | `syncsInWindow` | 0-5; resets max/min at 6 |

`RTC_FIELDS = 10`. `RTC_MAGIC = 0x94BE11C0u`.

---

### Task 1: RTC state seam — prims `rtcValid` / `rtcGet:` / `rtcPut:value:` (host, file-backed)

**Files:**
- Modify: `~/workspace/tuvm/src/prim_ids.h` (append 3 ids after 130)
- Modify: `~/workspace/tuvm/src/dispatch.c` (enum + `PRIM_NAMES` + parity typedef)
- Modify: `~/workspace/tuvm/src/platform.c` (host leaves + `#else` externs + 3 dispatch cases)
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (3 forwarders)
- Test: `~/workspace/nsl-tests/20_bell/07_rtc_seam.nsl`

**Interfaces:**
- Produces: prim selectors `rtcValid` (unary → SMI 0/1), `rtcGet:` (SMI index → SMI field), `rtcPut:value:` (index, value → SMI 0). Kernel forwarders: `rtcValid` (→ Boolean), `rtcGet: i` (→ SMI), `rtcPut: i value: v` (→ self).
- Host leaf funcs: `int rtc_target_valid(void)`, `int rtc_target_get(int i)`, `void rtc_target_put(int i, int v)`. File-backed via `OEVM_RTC_FILE`; in-memory (cold every process) when unset.

- [ ] **Step 1: Write the failing test** — `~/workspace/nsl-tests/20_bell/07_rtc_seam.nsl`

```
(*
expect:
cold valid false
put magic then valid true
half 5
bootticks 9
anchor 66600
finished
*)
(* tuvm#94 stage 5 — the RTC retention seam, host, in one process (no OEVM_RTC_FILE,
   so the struct is cold at start and lives only in memory). Cross-process persistence
   is exercised by the re-exec sequence harness (Task 7); here we pin the get/put
   round-trip and the magic-guarded cold path. *)
class RtcSeam packageUsing: manifest = () (
  public main: platform args: args = (
    (* Cold: no magic written yet -> invalid. *)
    ('cold valid ' , (platform rtcValid ifTrue: [ 'true' ] ifFalse: [ 'false' ])) out.
    (* Establish validity by writing the magic word, then read it back. *)
    platform rtcPut: 0 value: self magicWord.
    ('put magic then valid ' , (platform rtcValid ifTrue: [ 'true' ] ifFalse: [ 'false' ])) out.
    platform rtcPut: 1 value: 5.
    platform rtcPut: 2 value: 9.
    platform rtcPut: 3 value: 66600.
    ('half '      , (platform rtcGet: 1) printString) out.
    ('bootticks ' , (platform rtcGet: 2) printString) out.
    ('anchor '    , (platform rtcGet: 3) printString) out.
    'finished' out.
  )
  (* Must equal RTC_MAGIC 0x94BE11C0 = 2495717824 as an unsigned, but SMIs are
     signed 30-bit here; the seam compares the low 32 bits as uint32 in C, so the
     node writes the same bit pattern via its signed decimal. 0x94BE11C0 as int32 is
     -1799482944. nsl has no hex literal, so spell the decimal. *)
  magicWord = ( ^ -1799482944 )
) : (
)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/20_bell/07_rtc_seam.nsl B`
Expected: FAIL — `rtcValid`/`rtcGet:`/`rtcPut:value:` are unknown selectors (no prim).

- [ ] **Step 3: Append the three prim ids** in `~/workspace/tuvm/src/prim_ids.h` after `#define PRIM_pf_ntpSync 130` (before `#endif`):

```c
/* tuvm#94 stage 5 (deep-sleep clock): the RTC-retention seam. State that must
   outlive the dead VM across a deep sleep lives in a fixed 10-field SMI struct
   (.rtc.data on device, a file on host). rtcValid guards the magic word; rtcGet:/
   rtcPut:value: are indexed SMI accessors (design §3 table). Appended after
   ntpSync; no pre-existing id shifts. Counted (not computed): these are the
   131st/132nd/133rd PRIM_NAMES entries. */
#define PRIM_pf_rtcValid          131
#define PRIM_pf_rtcGet            132
#define PRIM_pf_rtcPut            133
```

- [ ] **Step 4: Extend the enum + names + parity guard** in `~/workspace/tuvm/src/dispatch.c`.

After `PRIM_pf_ntpSync,` in the enum (line ~788):

```c
    /* tuvm#94 stage 5: the RTC-retention seam — rtcValid / rtcGet: / rtcPut:value:.
       Appended after ntpSync; counted (not computed) to be the 131st/132nd/133rd
       PRIM_NAMES entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_rtcValid,
    PRIM_pf_rtcGet,
    PRIM_pf_rtcPut,
```

After `"ntpSyncTz:",` in `PRIM_NAMES` (line ~845):

```c
    "rtcValid", "rtcGet:", "rtcPut:value:",
```

Update the parity typedef (currently `== PRIM_pf_ntpSync`) to the new last id — this changes again in Task 2, so set it to the final id of THIS task:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_rtcPut) ? 1 : -1];
```

- [ ] **Step 5: Add the host leaves + externs + dispatch cases** in `~/workspace/tuvm/src/platform.c`.

In the host `#ifndef` block (beside `ntp_target_sync`, before line ~289 `#else`):

```c
/* tuvm#94 stage 5: the host side of the RTC-retention seam. On device this struct
   lives in .rtc.data and survives deep sleep; on host it is a plain int32 array that
   optionally persists to OEVM_RTC_FILE so the node can be re-exec'd N times and the
   ratchet witnessed ACROSS simulated sleeps (Task 7's harness). With no file set the
   array is cold at process start (magic unwritten -> rtcValid false), which is
   exactly the cold-power-on path. A reusable test feature, not a one-off. */
#define OEVM_RTC_FIELDS 10
#define OEVM_RTC_MAGIC  0x94BE11C0u
static int32_t g_rtc[OEVM_RTC_FIELDS];
static int g_rtc_loaded = 0;
static void rtc_load_once(void) {
    if (g_rtc_loaded) return;
    g_rtc_loaded = 1;
    memset(g_rtc, 0, sizeof g_rtc);          /* cold default: magic mismatch -> invalid */
    const char *p = getenv("OEVM_RTC_FILE");
    if (p) {
        FILE *f = fopen(p, "rb");
        if (f) { (void)fread(g_rtc, sizeof(int32_t), OEVM_RTC_FIELDS, f); fclose(f); }
    }
}
static void rtc_store(void) {
    const char *p = getenv("OEVM_RTC_FILE");
    if (!p) return;                          /* in-memory only */
    FILE *f = fopen(p, "wb");
    if (f) { (void)fwrite(g_rtc, sizeof(int32_t), OEVM_RTC_FIELDS, f); fclose(f); }
}
int rtc_target_valid(void) { rtc_load_once(); return ((uint32_t)g_rtc[0] == OEVM_RTC_MAGIC) ? 1 : 0; }
int rtc_target_get(int i) { rtc_load_once(); if (i < 0 || i >= OEVM_RTC_FIELDS) return 0; return g_rtc[i]; }
void rtc_target_put(int i, int v) { rtc_load_once(); if (i < 0 || i >= OEVM_RTC_FIELDS) return; g_rtc[i] = v; rtc_store(); }
```

In the `#else` (device) block beside the other externs (after line ~300):

```c
/* tuvm#94 stage 5: RTC-retention leaves live in zephyr/src/main.c (the .rtc.data
   struct is beside the wifi/ntp leaves). Same rule as bell/ntp — the symbols link on
   every Zephyr board; main.c guards bodies, never definitions. */
extern int rtc_target_valid(void);
extern int rtc_target_get(int i);
extern void rtc_target_put(int i, int v);
```

In the platform dispatch switch (beside `PRIM_pf_ntpSync`, before `default:` at line ~1263):

```c
    case PRIM_pf_rtcValid: {
        /* SMI 0/1; the kernel forwarder coerces to a Boolean, so no true/false oop
           is needed in C. */
        *out = TAG_SMI(rtc_target_valid());
        return 1;
    }
    case PRIM_pf_rtcGet: {
        *out = TAG_SMI(rtc_target_get((int)UNTAG_SMI(args[0])));
        return 1;
    }
    case PRIM_pf_rtcPut: {
        rtc_target_put((int)UNTAG_SMI(args[0]), (int)UNTAG_SMI(args[1]));
        *out = TAG_SMI(0);
        return 1;
    }
```

- [ ] **Step 6: Add the kernel forwarders** in `~/workspace/nsl-tuvm/lib/kernel.nsl`, after `public ntpSyncTz: offsetSeconds` (line ~2434):

```
  (* tuvm#94 stage 5: the RTC-retention seam. Thin forwarders over the platform prims,
     the same shape as ntpSyncTz: above. rtcValid answers a Boolean here (the prim
     answers an SMI 0/1 — no true/false oop is needed on the C side). rtcGet:/
     rtcPut:value: are indexed SMI accessors; the field indices (design §3) are node
     constants, not baked here, so the seam stays a dumb array. *)
  public rtcValid = ( ^ (platform rtcValid) = 1 )
  public rtcGet: i = ( ^ platform rtcGet: i )
  public rtcPut: i value: v = ( platform rtcPut: i value: v. ^ self )
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/20_bell/07_rtc_seam.nsl B`
Expected: PASS (output matches the `expect:` block).

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/tuvm && git add src/prim_ids.h src/dispatch.c src/platform.c
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl
cd ~/workspace/nsl-tests && git add 20_bell/07_rtc_seam.nsl
cd ~/workspace/tuvm && git commit -m "feat(magtag): RTC-retention seam — rtcValid/rtcGet:/rtcPut:value: (tuvm#94 stage 5)"
```
(Commit the nsl-tuvm and nsl-tests repos too, with the same message.)

---

### Task 2: `deepSleep:` prim — host leaf logs then `exit(0)`

**Files:**
- Modify: `~/workspace/tuvm/src/prim_ids.h`, `~/workspace/tuvm/src/dispatch.c`, `~/workspace/tuvm/src/platform.c`
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl`
- Test: `~/workspace/nsl-tests/20_bell/08_deepsleep.nsl`

**Interfaces:**
- Produces: prim selector `deepSleep:` (SMI seconds → never returns). Kernel forwarder `deepSleep: seconds`. Host: prints `sleep: arming N s` then `exit(0)` — so anything after it in a node does NOT print (the witness). Device leaf deferred to Task 8.

- [ ] **Step 1: Write the failing test** — `~/workspace/nsl-tests/20_bell/08_deepsleep.nsl`

```
(*
expect:
before sleep
sleep: arming 5 s
*)
(* tuvm#94 stage 5 — deepSleep: on the host is log-then-exit(0), so a host run
   TERMINATES rather than hanging (design §6). The proof is that 'after sleep' below
   is NEVER printed: the prim does not return. *)
class DeepSleep packageUsing: manifest = () (
  public main: platform args: args = (
    'before sleep' out.
    platform deepSleep: 5.
    'after sleep' out.   (* unreachable — deepSleep: never returns *)
  )
) : (
)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/20_bell/08_deepsleep.nsl B`
Expected: FAIL — `deepSleep:` is an unknown selector.

- [ ] **Step 3: Append the prim id** in `src/prim_ids.h` after `PRIM_pf_rtcPut`:

```c
/* tuvm#94 stage 5: deepSleep: seconds. On device it arms an esp_sleep timer wakeup
   and starts deep sleep (never returns — the VM ceases to exist until the next boot).
   On host it logs and exit(0)s so a run terminates. Counted (not computed) to be the
   134th PRIM_NAMES entry. */
#define PRIM_pf_deepSleep         134
```

- [ ] **Step 4: Extend enum + names + parity** in `src/dispatch.c`.

Enum, after `PRIM_pf_rtcPut,`:

```c
    /* tuvm#94 stage 5: deepSleep: — arms a timer wake and sleeps (device) / exits
       (host). Counted (not computed) to be the 134th PRIM_NAMES entry. */
    PRIM_pf_deepSleep,
```

`PRIM_NAMES`, after `"rtcValid", "rtcGet:", "rtcPut:value:",`:

```c
    "deepSleep:",
```

Parity typedef → new last id:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_deepSleep) ? 1 : -1];
```

- [ ] **Step 5: Add the host leaf + extern + dispatch case** in `src/platform.c`.

Host `#ifndef` block:

```c
/* tuvm#94 stage 5: host deepSleep: — no real sleep exists, so log the commanded
   duration and exit(0). This makes a host node run TERMINATE (not hang) at its idle
   point, and — with OEVM_RTC_FILE set — leaves the retained state on disk for the
   next re-exec, which is how the ratchet is witnessed across simulated sleeps. */
void deep_sleep_target(int seconds) {
    char line[48];
    int len = snprintf(line, sizeof line, "sleep: arming %d s\n", seconds);
    if (len > 0) platform_console_write(line, (size_t)len);
    exit(0);
}
```

`#else` externs:

```c
extern void deep_sleep_target(int seconds);
```

Dispatch case (beside the rtc cases):

```c
    case PRIM_pf_deepSleep: {
        /* Never returns (device sleeps; host exits). *out is set for form only. */
        deep_sleep_target((int)UNTAG_SMI(args[0]));
        *out = TAG_SMI(0);
        return 1;
    }
```

Confirm `<stdlib.h>` is included in platform.c (for `exit`); it is (getenv/atol already used). No new include.

- [ ] **Step 6: Add the kernel forwarder** in `~/workspace/nsl-tuvm/lib/kernel.nsl`, after the rtc forwarders:

```
  (* tuvm#94 stage 5: deepSleep: seconds. NEVER RETURNS — the VM ceases to exist
     until the next timer wake reboots it. There is no wrapper and no answer to coerce:
     the caller's very next line is unreachable by construction. *)
  public deepSleep: seconds = ( ^ platform deepSleep: seconds )
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/20_bell/08_deepsleep.nsl B`
Expected: PASS — `after sleep` absent.

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/tuvm && git add src/prim_ids.h src/dispatch.c src/platform.c
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl
cd ~/workspace/nsl-tests && git add 20_bell/08_deepsleep.nsl
cd ~/workspace/tuvm && git commit -m "feat(magtag): deepSleep: prim — arm-and-sleep (device) / log-and-exit (host) (tuvm#94 stage 5)"
```

---

### Task 3: Pure arithmetic — ratchet, drift, boundary, staleness, wrap (nsl unit tests)

The design §9 pins the state machine's pure formulas as standalone host tests, re-spelling the exact expressions the node will use (the `05_ntp_catchup.nsl` precedent — "pins the exact expression, not an equivalent one"). Tasks 4-6 reuse these identical spellings by name, so divergence is visible.

**Files:**
- Test: `~/workspace/nsl-tests/20_bell/09_bell_math.nsl`

**Interfaces:**
- Produces: the reference spellings for `secondsToNextBoundary:`, the §2 forward-only ratchet decision (advance-if-behind / hold-if-ahead), `driftPpm:over:`, `staleField:`, and midnight wrap. The node (Tasks 4-6) uses character-identical bodies for the pure formulas; the ratchet decision lives in the node's `applySync:` and is re-verified through the harness.

- [ ] **Step 1: Write the failing test** — `~/workspace/nsl-tests/20_bell/09_bell_math.nsl`

Every printed line below equals exactly one `expect:` row — the values are floor-division
results (nsl `//` floors toward −∞), verified: `(10*1000000)//86400 = 115`, and the negative
`(-10*1000000)//86400 = -116` (asymmetric — worth pinning, since a symmetric-magnitude drift
does NOT give symmetric ppm under floor division).

```
(*
expect:
boundary 66600 1800
boundary 67500 900
boundary 68399 1
boundary 86399 1
ratchet true40 last37 new40 strike false
ratchet true35 last37 new37 strike false
ratchet true38 last37 new38 strike true
ppm 10/86400 115
ppm -10/86400 -116
ppm 10/0 0
stale 0 0d/00h
stale 50400 0d/14h
stale 216000 2d/12h
finished
*)
(* tuvm#94 stage 5 — the run-once state machine's PURE arithmetic, host, no seam and
   no wall clock. Each formula is spelled EXACTLY as the node (Tasks 4-6) spells it, so
   this test and the node cannot silently diverge. nsl has no modulo (tuvm#97): `x mod n`
   is `x - ((x // n) * n)` throughout. *)
class BellMath packageUsing: manifest = () (
  public main: platform args: args = (
    (* seconds to the NEXT half-hour boundary, in SECONDS (design §3). A boundary-exact
       input yields a FULL interval (1800), never 0 — the double-strike edge. *)
    ('boundary 66600 ' , (self secondsToNextBoundary: 66600) printString) out.  (* 18:30:00 exact *)
    ('boundary 67500 ' , (self secondsToNextBoundary: 67500) printString) out.  (* 18:45:00 *)
    ('boundary 68399 ' , (self secondsToNextBoundary: 68399) printString) out.  (* 18:59:59 *)
    ('boundary 86399 ' , (self secondsToNextBoundary: 86399) printString) out.  (* 23:59:59 *)

    (* The §2 ratchet decision: given a true half-hour index and the last struck index,
       answer the new lastStruckHalf and whether THIS wake strikes. Only the ordinary
       strike wake (true = last+1) strikes; a sync jump forward, or a hold when ahead,
       never strikes. *)
    (self showRatchetTrue: 40 last: 37).   (* clock ran slow: advance, silent *)
    (self showRatchetTrue: 35 last: 37).   (* clock ran fast: hold, no strike *)
    (self showRatchetTrue: 38 last: 37).   (* the ordinary strike wake: +1, strike *)

    (* drift in ppm = (drift seconds * 1e6) // elapsed seconds, signed, floored. The
       0-elapsed guard answers 0 (the first sync has no interval to measure over). *)
    ('ppm 10/86400 '  , (self driftPpm: 10 over: 86400) printString) out.
    ('ppm -10/86400 ' , (self driftPpm: -10 over: 86400) printString) out.
    ('ppm 10/0 '      , (self driftPpm: 10 over: 0) printString) out.

    ('stale 0 '      , (self staleField: 0)) out.
    ('stale 50400 '  , (self staleField: 50400)) out.   (* 14 h *)
    ('stale 216000 ' , (self staleField: 216000)) out.  (* 2 d 12 h *)
    'finished' out.
  )

  secondsToNextBoundary: sec = ( ^ 1800 - (sec - ((sec // 1800) * 1800)) )

  showRatchetTrue: trueHalf last: lastHalf = (
    | newHalf struck |
    (trueHalf > lastHalf)
       ifTrue:  [ newHalf:: trueHalf. struck:: (trueHalf = (lastHalf + 1)) ]
       ifFalse: [ newHalf:: lastHalf. struck:: false ].
    ('ratchet true' , trueHalf printString , ' last' , lastHalf printString ,
       ' new' , newHalf printString , ' strike ' , (struck ifTrue: [ 'true' ] ifFalse: [ 'false' ])) out.
    ^ self )

  driftPpm: driftSec over: elapsedSec = (
    elapsedSec = 0 ifTrue: [ ^ 0 ].
    ^ (driftSec * 1000000) // elapsedSec )

  staleField: sec = (
    | mins d r h |
    mins:: sec // 60.
    d:: mins // 1440.
    r:: mins - (d * 1440).
    h:: r // 60.
    ^ d printString , 'd/' , (self pad: h) , 'h' )

  pad: n = ( (n < 10) ifTrue: [ ^ '0' , n printString ]. ^ n printString )
) : (
)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/20_bell/09_bell_math.nsl B`
Expected: FAIL — methods not yet written / output does not match.

- [ ] **Step 3: Make it pass** — the code above IS the implementation; run it and confirm every printed line equals its `expect:` row. If a value differs, the arithmetic spelling is wrong — fix the method, not the expectation (the `expect:` values are verified floor-division results).

- [ ] **Step 4: Run to verify it passes**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/20_bell/09_bell_math.nsl B`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/nsl-tests && git add 20_bell/09_bell_math.nsl && git commit -m "test(magtag): pin the deep-sleep state-machine arithmetic (tuvm#94 stage 5)"
```

---

### Task 4: The node — run-once skeleton (cold-boot + strike wake) + host harness

**Files:**
- Create: `~/workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl`
- Create: `~/workspace/nsl-tuvm/tools/test_deepsleep_node.py` (the real-node harness; grown by Tasks 5-6-7)

**Interfaces:**
- Consumes: `rtcValid`, `rtcGet: i`, `rtcPut: i value: v`, `deepSleep: sec` (Tasks 1-2); the arithmetic spellings (Task 3).
- Produces: `MagTagDeepSleepNode` with `main:args:` = the §7 straight-line script, plus pure helpers `secondsToNextBoundary:`, `initColdState:`, `predictedNow:`, and the `rtc*` index constants. The node REUSES the kernel's `Bell>>bellsForHalfHour:` (call `(plat bell) bellsForHalfHour: half` — it already exists at kernel.nsl:2166, `i mod 8`, 0→8; the reactor calls it the same way at MagTagClockNode.nsl:250, so the node does NOT redefine it). The node prints, per wake, `wake half N bells B`, then `struck`, then `sleep: arming S s` — the harness's assertion surface.
- Produces: `test_deepsleep_node.py` with a `run_wake(rtc_file, ntp=None, no_ntp=False)` helper (sets `OEVM_RTC_FILE` + optional NTP env, runs the compiled node once via the image VM, returns stdout) and a `read_rtc(path)` helper (`struct.unpack('<10i', ...)` → the field tuple). **Do NOT set `OEVM_NO_BELL`** — the host bell defaults available, so `strike:` prints `bell: struck N` and returns OK; with `OEVM_NO_BELL=1` the bell answers nil and the node's unguarded `strike:` would RAISE `#bellUnavailable` and never reach `deepSleep:`. Later tasks add scenarios.

**Why a Python harness, not an nsl copy:** the node ends in `deepSleep:` (host `exit(0)`), so one process = one wake. The file-backed RTC (`OEVM_RTC_FILE`) makes state survive the exit, so the harness drives the REAL node across re-execs and pre-seeds state by writing the binary struct directly — the faithful realization of design §9's "simulated struct + scripted sync results" with zero body duplication.

- [ ] **Step 1: Write the failing test** — `~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`, first scenario only:

```python
#!/usr/bin/env python3
# tuvm#94 stage 5 — drive the REAL MagTagDeepSleepNode across simulated deep sleeps.
# Host deepSleep: exits(0); OEVM_RTC_FILE persists the .rtc.data-equivalent struct to
# disk, so re-exec'ing the node is a real wake/sleep cycle with no hardware. A reusable
# feature (David, 2026-07-24), grown by Tasks 5-7.
import os, struct, subprocess, sys, tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import run_tests_oevm as R   # reuse the Path-B compile + host-oevm invocation

NODE = Path.home() / "workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl"
RTC_MAGIC = -1799482944   # 0x94BE11C0 as int32

# --- Path-B plumbing (mirrors run_tests_oevm.run_test_file_oimg:496-546) -----------
# Compile the node to .oimg ONCE, then re-run the image VM per wake with a custom env.
_OIMG = None
def _compile_once():
    global _OIMG
    if _OIMG: return _OIMG
    ok, err = R._build_pathb_once()          # builds /tmp/oevm_img + /tmp/oevm_layout.json
    if not ok: raise RuntimeError(err)
    oimg = "/tmp/magtag_deepsleep.oimg"
    comp = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"),
         str(NODE), oimg, "--format=oimg", "--layout=/tmp/oevm_layout.json"],
        capture_output=True, text=True)
    if comp.returncode != 0:
        raise RuntimeError("compile failed:\n" + (comp.stderr or comp.stdout))
    _OIMG = oimg
    return oimg

def run_wake(rtc_file, ntp=None, no_ntp=False):
    oimg = _compile_once()
    env = dict(os.environ, OEVM_RTC_FILE=str(rtc_file))   # NOTE: no OEVM_NO_BELL (would raise)
    if no_ntp: env["OEVM_NO_NTP"] = "1"
    elif ntp is not None: env["OEVM_NTP_SECONDS"] = str(ntp)
    else: env.pop("OEVM_NTP_SECONDS", None)
    p = subprocess.run(["/tmp/oevm_img", oimg], capture_output=True, text=True, timeout=30, env=env)
    # deepSleep: exits 0; a nonzero rc means an unhandled raise — surface it.
    assert p.returncode == 0, f"node rc={p.returncode}\nSTDOUT:\n{p.stdout}\nSTDERR:\n{p.stderr}"
    return p.stdout

def read_rtc(path):
    with open(path, "rb") as f: return struct.unpack("<10i", f.read(40))

# --- scenarios --------------------------------------------------------------------
def test_cold_boot_strike():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"          # absent -> cold boot
        out = run_wake(rtc)
        assert "cold boot: free-run from 00:00" in out, out
        assert "wake half 1 bells 1" in out, out      # first ratchet strike
        assert "sleep: arming 1800 s" in out, out       # predictedNow 0 -> full interval
        magic, last, boot, *_ = read_rtc(rtc)
        assert magic == RTC_MAGIC and last == 1 and boot == 1, read_rtc(rtc)

if __name__ == "__main__":
    test_cold_boot_strike(); print("ok")
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: FAIL — `run_node` unimplemented / node does not exist.

- [ ] **Step 3: Write `MagTagDeepSleepNode.nsl`** — the §7 straight-line script (cold-boot + strike wake; the watch-end branch is a stub Task 5 fills). The harness's `_compile_once`/`run_wake` above already carry the exact Path-B invocation — no reverse-engineering needed.

```
(* tuvm#94 stage 5 — the ship's-bell clock as a RUN-ONCE deep-sleep appliance.

   THE REACTOR IS GONE. Stages 1-4 (MagTagClockNode.nsl, kept as the always-on demo
   sample) boot one VM and drain an event loop forever. Here, between wakes the CPU is
   off and THE VM DOES NOT EXIST: each timer wake is a fresh boot that lives a few
   seconds and dies. There is no drain loop, no channel, no VM timer — deepSleep: IS the
   idle point. This also DELETES the timer-instance bug class that cost the stage 2
   overnight run (a timer registered on one Timers, dispatched on another): there are no
   VM timers left to misregister.

   THE HALF-HOUR INDEX IS A MONOTONIC RATCHET (design §2). A strike is a pure increment,
   so a strike wake CANNOT double-bell. A sync may push the count FORWARD (skip missed
   strikes, silently) but NEVER backward; when NTP says we are behind where we already
   struck, we hold the count and lengthen the next sleep. Drift never touches the count —
   only the sleep duration (phase).

   STATE LIVES IN .rtc.data (design §3), reached through rtcGet:/rtcPut:value:, because
   RAM does not survive the sleep. Field indices are the constants below and are
   identical to the C struct order in zephyr/src/main.c. nsl has no modulo (tuvm#97), so
   `x mod n` is `x - ((x // n) * n)` throughout. *)
class MagTagDeepSleepNode packageUsing: manifest = () (

  public main: platform args: args = (
    | plat half bells isWatchEnd predictedNow sleepSec |
    plat:: platform.

    (* Cold power-on leaves .rtc.data UNDEFINED (not zeroed), so a magic mismatch means
       "no valid state" -> free-run from 00:00 and let the first watch-end sync correct
       everything (design §8). *)
    (plat rtcValid) ifFalse: [ self initColdState: plat ].

    (* The strike about to happen: a pure increment of the ratchet (design §2). *)
    half:: (plat rtcGet: self rtcLastHalf) + 1.
    half = 48 ifTrue: [ half:: 0 ].                       (* midnight wrap: 47 -> 0 *)
    bells:: (plat bell) bellsForHalfHour: half.           (* reuse the kernel's Bell method *)
    isWatchEnd:: bells = 8.                               (* 8 bells == a watch end *)

    (* Strike BLOCKS to completion before any sleep (design §8: strike never cut off). *)
    ('wake half ' , half printString , ' bells ' , bells printString) out.
    (plat bell) strike: bells.
    'struck' out.
    plat rtcPut: self rtcLastHalf value: half.
    plat rtcPut: self rtcBootTicks value: (plat rtcGet: self rtcBootTicks) + 1.

    (* Watch-end: sync + paint (Tasks 5-6). Stubbed here so the skeleton stands alone. *)
    isWatchEnd ifTrue: [ self onWatchEnd: plat ].

    (* Phase-align the next wake to the true boundary. predictedNow = (anchor + elapsed)
       mod 86400 (design §3). Cold/never-synced => anchor 0, elapsed accumulates. *)
    predictedNow:: self predictedNow: plat.
    sleepSec:: self secondsToNextBoundary: predictedNow.
    plat rtcPut: self rtcElapsedSec value: (plat rtcGet: self rtcElapsedSec) + sleepSec.
    plat deepSleep: sleepSec.                             (* NEVER RETURNS *)
  )

  (* Free-run from 00:00: predictedNow 0, ratchet 0 (so the first strike is half 1),
     staleSec large so the panel shows "never synced". Magic set LAST so a crash between
     writes cannot leave a half-initialised struct reading valid. *)
  initColdState: plat = (
    'cold boot: free-run from 00:00' out.
    plat rtcPut: self rtcLastHalf     value: 0.
    plat rtcPut: self rtcBootTicks    value: 0.
    plat rtcPut: self rtcAnchorSec    value: 0.
    plat rtcPut: self rtcElapsedSec   value: 0.
    plat rtcPut: self rtcDriftPpm     value: 0.
    plat rtcPut: self rtcStaleSec     value: 999999.
    plat rtcPut: self rtcDrift24Max   value: 0.
    plat rtcPut: self rtcDrift24Min   value: 0.
    plat rtcPut: self rtcSyncsWin     value: 0.
    plat rtcPut: self rtcMagic        value: self magicWord.
    ^ self )

  (* Stub — Task 5 replaces this with sync + ratchet + re-anchor. *)
  onWatchEnd: plat = ( ^ self )

  predictedNow: plat = (
    | now |
    now:: (plat rtcGet: self rtcAnchorSec) + (plat rtcGet: self rtcElapsedSec).
    ^ now - ((now // 86400) * 86400) )    (* mod 86400 — nsl has no modulo (tuvm#97) *)

  (* IDENTICAL spelling to 09_bell_math.nsl. *)
  secondsToNextBoundary: sec = ( ^ 1800 - (sec - ((sec // 1800) * 1800)) )

  (* NOTE: bellsForHalfHour: is NOT redefined here — the node calls the kernel's
     Bell>>bellsForHalfHour: via `(plat bell) bellsForHalfHour: half`, exactly as the
     reactor does (MagTagClockNode.nsl:250). *)

  (* --- RTC field indices (design §3 table; identical order to the C struct) ------ *)
  rtcMagic      = ( ^ 0 )
  rtcLastHalf   = ( ^ 1 )
  rtcBootTicks  = ( ^ 2 )
  rtcAnchorSec  = ( ^ 3 )
  rtcElapsedSec = ( ^ 4 )
  rtcDriftPpm   = ( ^ 5 )
  rtcStaleSec   = ( ^ 6 )
  rtcDrift24Max = ( ^ 7 )
  rtcDrift24Min = ( ^ 8 )
  rtcSyncsWin   = ( ^ 9 )
  magicWord     = ( ^ -1799482944 )   (* 0x94BE11C0 as int32 *)
) : (
)
```

- [ ] **Step 4: Run to verify it passes**

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: `ok` — cold boot strikes half 1, arms an 1800 s sleep, and the RTC file reads `magic, last=1, boot=1`.

- [ ] **Step 5: Confirm a clean standalone compile** (the shipping node carries no `expect:` header, so `run-one.py` would SKIP it — compile it directly instead, which is also what `run_wake` does):

Run: `python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py ~/workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl /tmp/mtds_check.oimg --format=oimg --layout=/tmp/oevm_layout.json`
Expected: exit 0, no compile errors. (Step 4's harness already compiled AND ran it; this isolates a syntax error from a runtime one per [[nsl-app-seam-vs-kernel]].)

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/MagTagDeepSleepNode.nsl
cd ~/workspace/nsl-tuvm && git add tools/test_deepsleep_node.py
cd ~/workspace/tuvm && git commit -m "feat(magtag): deep-sleep node — run-once strike wake + cold-boot free-run (tuvm#94 stage 5)"
```

---

### Task 5: Watch-end sync + the §2 ratchet (`onWatchEnd:` / `applySync:`)

**Files:**
- Modify: `~/workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl`
- Modify: `~/workspace/nsl-tuvm/tools/test_deepsleep_node.py` (add scenarios)

**Interfaces:**
- Consumes: `ntpSyncTz:` (prim 130), the rtc seam, `predictedNow:`.
- Produces: `onWatchEnd: plat`, `applySync: trueSec plat: plat` (the §2 forward-only ratchet + drift measure + re-anchor + zero stale + rolling 24 h window), `trySyncUpTo3: plat`, `markSyncFailed: plat`, `recordDrift:plat:`, `wrapDrift:`, `driftPpm:over:`. On decision the node prints `sync true T predicted P -> hold half H` or `-> advance half H`; on failure `ntp miss` (×3) then `sync failed, stale grows`.
- Produces (harness): a `seed_rtc(path, last, anchor, elapsed, **fields)` helper that writes the 10-int32 struct (magic + given fields) so a scenario starts mid-history without walking every prior wake — the direct realization of design §9's "simulated struct".

- [ ] **Step 1: Write the failing scenarios** in `test_deepsleep_node.py`:

```python
def seed_rtc(path, last=0, boot=0, anchor=0, elapsed=0, drift=0, stale=0,
             d24max=0, d24min=0, syncs=0):
    with open(path, "wb") as f:
        f.write(struct.pack("<10i", RTC_MAGIC, last, boot, anchor, elapsed,
                            drift, stale, d24max, d24min, syncs))

def test_hold_when_ahead():
    # Watch end at 04:00 (half 8). Seed lastStruckHalf=7, predictedNow=14400 (04:00).
    # NTP says 03:59:57 -> trueHalf 7 <= struck 8 after the +1 strike -> HOLD, no re-strike.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=14397)
        # count the node's OWN per-wake marker, not "struck" — the host bell leaf also
        # prints "bell: struck N", so out.count("struck") would be 2, not 1.
        assert out.count("wake half") == 1, out               # exactly one strike wake
        assert "-> hold half 8" in out, out                    # count not moved back
        _, last, *_ = read_rtc(rtc)
        assert last == 8, read_rtc(rtc)                        # ratchet held at 8

def test_advance_silent_when_behind():
    # Missed wakes: seed lastStruckHalf=7 but NTP says it is already 06:00 (half 12).
    # After the +1 strike (half 8) the sync finds trueHalf 12 > 8 -> advance to 12,
    # SILENTLY (only the one strike this wake, skipped bells not rung).
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=21600)                         # 06:00:00
        assert out.count("wake half") == 1, out               # one strike wake, skipped bells NOT rung
        assert "-> advance half 12" in out, out
        _, last, *_ = read_rtc(rtc)
        assert last == 12, read_rtc(rtc)

def test_sync_fail_keeps_striking():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400, stale=0)
        out = run_wake(rtc, no_ntp=True)
        assert out.count("ntp miss") == 3, out                 # 3 attempts, all miss
        assert "sync failed, stale grows" in out, out
        _, last, _, _, _, _, stale, *_ = read_rtc(rtc)
        assert last == 8 and stale == 14400, read_rtc(rtc)     # struck, count kept, stale grew
```

Extend `__main__` to run all four scenarios.

- [ ] **Step 2: Run to verify they fail**

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: FAIL — `onWatchEnd:` is still a stub (`-> hold`/`advance` never printed).

- [ ] **Step 3: Replace the `onWatchEnd:` stub** and add the sync helpers (follow §7/§2):

```
  onWatchEnd: plat = (
    | trueSec |
    trueSec:: self trySyncUpTo3: plat.
    trueSec == nil
       ifTrue:  [ self markSyncFailed: plat ]
       ifFalse: [ self applySync: trueSec plat: plat ].
    (* paintStats: is added in Task 6, AFTER the sync, so a FAIL shows on the glass. *)
    self paintStats: plat.
    ^ self )

  (* 3 attempts fit one ~15 s awake window (design §4); "quit until next watch-end" is
     just: return nil, schedule nothing — the next attempt is 4 h on. sntp_simple's own
     4 s timeout spaces the retries (design §4: "3 × (4 s timeout + backoff)"); no extra
     backoff is spelled here (tuvm#97 has no sleep-spin primitive worth adding for it). *)
  trySyncUpTo3: plat = (
    | secs |
    3 timesRepeat: [
      secs:: plat ntpSyncTz: self tzOffset.
      secs == nil ifFalse: [ ^ secs ].
      'ntp miss' out ].
    ^ nil )

  (* The §2 ratchet, both directions. NEVER moves the count backward; drift only ever
     adjusts the sleep (phase) via the re-anchored predictedNow next wake. *)
  applySync: trueSec plat: plat = (
    | trueHalf lastHalf predicted drift |
    trueHalf:: trueSec // 1800.
    lastHalf:: plat rtcGet: self rtcLastHalf.
    predicted:: self predictedNow: plat.

    (trueHalf > lastHalf)
       ifTrue:  [ (* Fell behind (missed wakes / dead battery, or the clock ran slow):
                     advance SILENTLY — do NOT ring the skipped bells. `trueSec - predicted`
                     here is a CATCH-UP GAP, not oscillator drift (the board was off, so
                     elapsedSec never accrued those sleeps), so it is deliberately NOT
                     recorded — recording it would spike the 24 h drift window with a value
                     that is hours, not the seconds real drift produces. This also skips the
                     first-sync-after-cold-boot reset, which is almost always an advance. *)
                  ('sync true ' , trueSec printString , ' predicted ' , predicted printString , ' -> advance half ' , trueHalf printString) out.
                  plat rtcPut: self rtcLastHalf value: trueHalf ]
       ifFalse: [ (* On time / slightly ahead: HOLD the count and record the REAL drift —
                     the small signed gap between where the crystal put us and true time,
                     over the interval elapsedSec. This is the number the panel reports. *)
                  drift:: self wrapDrift: (trueSec - predicted).
                  self recordDrift: (self driftPpm: drift over: (plat rtcGet: self rtcElapsedSec)) plat: plat.
                  ('sync true ' , trueSec printString , ' predicted ' , predicted printString , ' -> hold half ' , lastHalf printString) out ].

    (* Re-anchor the wall clock to true time; zero the accumulated sleep + stale. *)
    plat rtcPut: self rtcAnchorSec  value: trueSec.
    plat rtcPut: self rtcElapsedSec value: 0.
    plat rtcPut: self rtcStaleSec   value: 0.
    ^ self )

  markSyncFailed: plat = (
    plat rtcPut: self rtcStaleSec value: (plat rtcGet: self rtcStaleSec) + 14400.
    'sync failed, stale grows' out.
    ^ self )

  (* Rolling 6-sync (24 h) window of the drift ppm; reset the extremes at 6 (design §3). *)
  recordDrift: ppm plat: plat = (
    | n mx mn |
    plat rtcPut: self rtcDriftPpm value: ppm.
    n:: (plat rtcGet: self rtcSyncsWin) + 1.
    n >= 6 ifTrue: [ n:: 0 ].
    n = 0 ifTrue: [ plat rtcPut: self rtcDrift24Max value: ppm. plat rtcPut: self rtcDrift24Min value: ppm ]
          ifFalse: [
            mx:: plat rtcGet: self rtcDrift24Max. mn:: plat rtcGet: self rtcDrift24Min.
            ppm > mx ifTrue: [ plat rtcPut: self rtcDrift24Max value: ppm ].
            ppm < mn ifTrue: [ plat rtcPut: self rtcDrift24Min value: ppm ] ].
    plat rtcPut: self rtcSyncsWin value: n.
    ^ self )

  (* Drift wrapped across midnight: a true 0 against a predicted 86399 is +1 s, not
     -86399 (the same wrap the reactor's correctionTo: needed). *)
  wrapDrift: d = (
    d > 43200 ifTrue: [ ^ d - 86400 ].
    d < -43200 ifTrue: [ ^ d + 86400 ].
    ^ d )

  (* IDENTICAL spelling to 09_bell_math.nsl. *)
  driftPpm: driftSec over: elapsedSec = ( elapsedSec = 0 ifTrue: [ ^ 0 ]. ^ (driftSec * 1000000) // elapsedSec )

  tzOffset = ( ^ -25200 )   (* Phoenix, UTC-7, no DST — a product fact passed to the prim *)
  paintStats: plat = ( ^ self )   (* Task 6 fills this in *)
```

- [ ] **Step 4: Run to verify they pass**

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: `ok` — hold-when-ahead holds at 8 with one strike; advance-when-behind jumps to 12 silently; sync-fail keeps the count and grows stale.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/MagTagDeepSleepNode.nsl
cd ~/workspace/nsl-tuvm && git add tools/test_deepsleep_node.py
cd ~/workspace/tuvm && git commit -m "feat(magtag): watch-end sync + forward-only ratchet (tuvm#94 stage 5)"
```

---

### Task 6: The watch-end panel (`paintStats:`) — sleep-build layout

**Files:**
- Modify: `~/workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl`
- Modify: `~/workspace/nsl-tuvm/tools/test_deepsleep_node.py` (assert echoed rows)

**Interfaces:**
- Consumes: the rtc seam, `plat display`, `plat heapStats`, plus two SAME-WAKE instance vars set during `onWatchEnd:`/`applySync:` (below): `syncOk` (did THIS watch's sync succeed) and `lastDriftSec` (the measured drift in seconds, or nil if not measured). These need not survive sleep — `paintStats:` runs in the same wake as the sync — so they are plain instance vars, NOT RTC fields (the 10-field struct is full, and there is no free slot for "last drift seconds"; this is the clean way to show the `ntp` row's seconds without one).
- Produces: `paintStats: plat` painting the design §5 layout, **per-row best-effort** — each row's draw is wrapped in its own catch (`tryRow:s:on:`), so one over-wide value skips only its own row instead of blanking the whole panel. This matters concretely on the host: the image VM's arena is a fixed 16 MB (`heapField` → `.../16384K`, 14 cols > the 12-col grid), so the **heap row raises `#displayBounds` and is skipped on host** while the other nine render; on device (20 KB arena → `20/20K`) every row fits. Formatters: `heapField:`, `signedField:`, `upField:`, `ntpField`, `staleField:`, `line:val:`, `kRound:`, `spaces:`, `pad:` (lifted from the reactor, `MagTagClockNode.nsl:266-352`).

- [ ] **Step 1: Add the SAME-WAKE instance vars and wire them.** Three small edits to the node (Tasks 4/5 code):

  1. Class header — add the two vars:
```
class MagTagDeepSleepNode packageUsing: manifest = ( | lastDriftSec syncOk | ) (
```
  2. `onWatchEnd:` — record the outcome so the panel can show it:
```
  onWatchEnd: plat = (
    | trueSec |
    trueSec:: self trySyncUpTo3: plat.
    trueSec == nil
       ifTrue:  [ syncOk:: false. self markSyncFailed: plat ]
       ifFalse: [ syncOk:: true.  self applySync: trueSec plat: plat ].
    self paintStats: plat.                    (* AFTER the sync, so a FAIL shows *)
    ^ self )
```
  3. `applySync:` hold branch — capture the measured drift seconds for the `ntp` row (add `lastDriftSec:: drift.` right before the existing `recordDrift:` line):
```
       ifFalse: [ drift:: self wrapDrift: (trueSec - predicted).
                  lastDriftSec:: drift.
                  self recordDrift: (self driftPpm: drift over: (plat rtcGet: self rtcElapsedSec)) plat: plat.
                  ('sync true ' , trueSec printString , ' predicted ' , predicted printString , ' -> hold half ' , lastHalf printString) out ].
```

- [ ] **Step 2: Write the failing scenario** in `test_deepsleep_node.py`. On the host the node echoes each successfully-drawn row as `| <row>`; the heap row is skipped (16 MB arena overflows the 12-col grid), so the test asserts the nine NON-heap rows and that each rendered row is ≤ 12 columns:

```python
def test_watch_end_panel():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400, syncs=2, d24max=8, d24min=-3)
        out = run_wake(rtc, ntp=14400)                      # exact 04:00 -> on-time, drift 0
        assert "sync true 14400 predicted 14400 -> hold half 8" in out, out   # on-time -> drift measured
        # 10 rows draw; the heap row (16 MB host arena -> "/16384K") overflows 12 cols and is
        # skipped, so EXACTLY 9 `| ` lines echo — assert the count so a missing row is caught.
        painted = [l for l in out.splitlines() if l.startswith("| ")]
        assert len(painted) == 9, painted
        assert out.count("| ----------") == 2, out          # BOTH separator rows render
        for row in ("| tuvm", "| ppm", "| up", "| ntp", "| 24h", "| sync"):
            assert row in out, (row, out)
        # row 8 = the unlabeled 24h-min continuation row, distinct from the 24h(max) row
        assert any(l.strip("| ").strip() == "-3" for l in painted), painted
        assert "+0.0s" in out, out                          # drift 0 measured this watch
        for line in painted:
            assert len(line) - 2 <= 12, line                # 12-col hard limit (design §5)
```

- [ ] **Step 3: Run to verify it fails**

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: FAIL — `paintStats:` is a no-op stub (no `| ...` rows).

- [ ] **Step 4: Implement `paintStats:` + the formatters.** Replace the `paintStats:` stub and add the helpers (adapted from the reactor `MagTagClockNode.nsl:266-352`, dropping the exact-time/`bells` rows per §5, `gc`→`ppm`, adding the drift/stale rows):

```
  (* Painted ONLY at watch-ends (design §5), AFTER the sync, so a FAIL shows. Per-row
     best-effort: each row draws inside its own catch, so an over-wide value (e.g. the
     16 MB host arena's heap row) skips just that row instead of blanking the panel.
     A whole-panel guard around `display`/`clear` still carries on headless if the board
     has no panel at all. *)
  paintStats: plat = (
    | disp hs |
    (* Sentinel-and-return: a handler block is a real escaping closure, so `^` INSIDE it is
       a compile error under nsl's no-NLR model (review/lib/errors.md). Capture the on:do:
       result — the cleared display, or the nil sentinel from the handler — and `^` from the
       METHOD BODY instead. (An earlier draft put `^ self` inside the handler; it does not
       compile.) *)
    disp:: [ | d | d:: plat display. d clear. d ]
      on: [ :e | e tag = #displayUnavailable ]
      do: [ :e | nil ].
    disp == nil ifTrue: [ 'no display; skipping paint' out. ^ self ].
    hs:: plat heapStats.
    self tryRow: 0 s: (self line: 'tuvm' val: '')                                       on: disp.
    self tryRow: 1 s: '----------'                                                      on: disp.
    self tryRow: 2 s: (self line: 'heap' val: (self heapField: hs))                     on: disp.
    self tryRow: 3 s: (self line: 'ppm'  val: (self signedField: (plat rtcGet: self rtcDriftPpm))) on: disp.
    self tryRow: 4 s: (self line: 'up'   val: (self upField: plat))                     on: disp.
    self tryRow: 5 s: '----------'                                                      on: disp.
    self tryRow: 6 s: (self line: 'ntp'  val: self ntpField)                            on: disp.
    self tryRow: 7 s: (self line: '24h'  val: (self signedField: (plat rtcGet: self rtcDrift24Max))) on: disp.
    self tryRow: 8 s: (self line: ''     val: (self signedField: (plat rtcGet: self rtcDrift24Min))) on: disp.
    self tryRow: 9 s: (self line: 'sync' val: (self staleField: (plat rtcGet: self rtcStaleSec)))    on: disp.
    [ disp refresh ] on: [ :e | (e tag = #displayFault) or: [ e tag = #displayUnavailable ] ]
                     do: [ :e | ('refresh skipped: ' , e tag printString) out ].
    ^ self )

  (* Draw one row and echo it, catching a too-wide/faulted row so the rest still paint.
     The echo is INSIDE the try and AFTER the draw, so a skipped row emits no `| ` line —
     which is exactly what the host test keys on for the (skipped) heap row. *)
  tryRow: r s: s on: disp = (
    [ disp at: r col: 0 put: s. ('| ' , s) out ]
      on: [ :e | (e tag = #displayBounds) or: [ e tag = #displayFault ] ]
      do: [ :e | ('row ' , r printString , ' skipped: ' , e tag printString) out ].
    ^ self )

  (* live rounded to whole KiB / arena in KiB. On device (20 KB) this is `20/20K`; on the
     host image VM (16 MB) it is `.../16384K`, which overflows the 12-col grid and is
     skipped by tryRow: — the row is witnessed on device (Task 9), not on host. *)
  heapField: hs = ( ^ (self kRound: hs liveBytes) , '/' , (hs heapSize // 1024) printString , 'K' )

  (* The `ntp` row: this watch FAILED -> FAIL; synced and measured drift (hold branch) ->
     `+N.Ns`; synced but advanced (missed wakes, no drift measured) -> `sync`. There is no
     `--` never-synced case because paint happens ONLY at a watch-end, where a sync was
     just attempted. *)
  ntpField = (
    syncOk ifFalse: [ ^ 'FAIL' ].
    lastDriftSec == nil ifTrue: [ ^ 'sync' ].
    ^ (self signedField: lastDriftSec) , '.0s' )

  (* Uptime from bootTicks (30 min per tick), wrap-safe — the reactor's upField, with the
     cadence inlined as 30 since a sleep tick IS one half-hour. *)
  upField: plat = (
    | mins d r h m |
    mins:: (plat rtcGet: self rtcBootTicks) * 30.
    d:: mins // 1440.
    r:: mins - (d * 1440).
    h:: r // 60.
    m:: r - (h * 60).
    ^ d printString , 'd' , (self pad: h) , ':' , (self pad: m) )

  (* Age since last good sync, `Nd/NNh` — IDENTICAL spelling to 09_bell_math.nsl. *)
  staleField: sec = (
    | mins d r h |
    mins:: sec // 60.
    d:: mins // 1440.
    r:: mins - (d * 1440).
    h:: r // 60.
    ^ d printString , 'd/' , (self pad: h) , 'h' )

  (* Signed integer, `+N` / `-N`. nsl has no unary minus display, so build it. *)
  signedField: n = (
    n < 0 ifTrue: [ ^ '-' , (0 - n) printString ].
    ^ '+' , n printString )

  (* Right-justify `v` against `label` in exactly 12 columns; overshoot (pad < 0) is left
     un-truncated so the too-wide row raises #displayBounds and tryRow: skips it. *)
  line: label val: v = (
    | pad |
    pad:: 12 - label size - v size.
    pad < 0 ifTrue: [ pad:: 0 ].
    ^ label , (self spaces: pad) , v )

  spaces: n = ( | s | s:: ''. n timesRepeat: [ s:: s , ' ' ]. ^ s )
  kRound: bytes = ( ^ ((bytes + 512) // 1024) printString )
  pad: n = ( (n < 10) ifTrue: [ ^ '0' , n printString ]. ^ n printString )
```

- [ ] **Step 5: Run to verify it passes**

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: `ok` — the nine non-heap rows render (each ≤12 cols) and the `ntp` row shows `+0.0s`; the heap row is skipped on host (echoed as `row 2 skipped: #displayBounds`).

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/MagTagDeepSleepNode.nsl
cd ~/workspace/nsl-tuvm && git add tools/test_deepsleep_node.py
cd ~/workspace/tuvm && git commit -m "feat(magtag): watch-end panel — ppm/drift/stale sleep layout, per-row best-effort (tuvm#94 stage 5)"
```

---

### Task 7: Capstone witness — monotonic ratchet across N simulated sleeps

The single biggest payoff of the file-backed RTC: chain many real-node wakes through one shared `OEVM_RTC_FILE` and prove the ratchet is monotonic across simulated sleeps — the only host witness for the multi-wake sequence itself (device power draw is never host-testable).

**Files:**
- Modify: `~/workspace/nsl-tuvm/tools/test_deepsleep_node.py` (add the capstone + wire into the aux-gate runner if the suite auto-discovers `tools/test_*.py`)

- [ ] **Step 1: Write the failing capstone** in `test_deepsleep_node.py`:

```python
def _mono_mod48(seq):
    # Each step advances by >=1 half, wrapping 47->0 counts as +1 (never a decrease).
    for a, b in zip(seq, seq[1:]):
        step = (b - a) % 48
        if not (1 <= step <= 47):
            return False
    return True

def test_ratchet_monotonic_across_sleeps():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"          # absent -> cold boot on wake 1
        halves = []
        for _ in range(12):                # 12 strike wakes through midnight-ish, no sync
            out = run_wake(rtc)
            halves.append(int(out.split("wake half ")[1].split(" ")[0]))
        assert _mono_mod48(halves), halves
        assert 0 in halves, halves          # crossed the 47->0 midnight wrap
```

- [ ] **Step 2: Run to verify it fails, then passes** (it should pass once Tasks 4-6 are in; if it fails, the failure is a real ratchet/wrap bug — fix in the node, not the test).

Run: `python3 ~/workspace/nsl-tuvm/tools/test_deepsleep_node.py`
Expected: `ok` — 12 wakes strictly advance mod-48 and cross the midnight wrap.

- [ ] **Step 3: Register the harness as an aux gate.** Confirm whether `run_tests_oevm.py` auto-discovers `tools/test_*.py` (it runs the C unit tests + integration gates — [[tuvm-vm-source-lists]]). If it does not, add `test_deepsleep_node.py` to its gate list so the sequence witness runs with the suite. Record which mechanism was used.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/nsl-tuvm && git add tools/test_deepsleep_node.py && git commit -m "test(magtag): capstone — monotonic ratchet across 12 simulated sleeps (tuvm#94 stage 5)"
```


---

### Task 8: Device leaves — `.rtc.data` struct + `deepSleep:` HAL

No host test — the device path is witnessed on hardware (Task 9). This task makes the four leaves link and build for the S2.

**Files:**
- Modify: `~/workspace/tuvm/zephyr/src/main.c` (rtc struct in `.rtc.data`; `rtc_target_valid/get/put`; `deep_sleep_target`)
- Possibly Modify: `~/workspace/tuvm/zephyr/prj_esp32s2_wifi.conf` (only if a Kconfig is needed for retention/poweroff — verify at build)

**Interfaces:**
- Produces on target: `int rtc_target_valid(void)`, `int rtc_target_get(int)`, `void rtc_target_put(int,int)`, `void deep_sleep_target(int)` — the same signatures platform.c externs.

- [ ] **Step 1: Add the retained struct + rtc leaves** in `zephyr/src/main.c` (beside the wifi/ntp leaves; guard bodies, never definitions — the symbols must link on every board):

```c
/* tuvm#94 stage 5: the RTC-retention seam, device side. .rtc.data lands in the S2's
   8 KB rtc_slow_seg (0x50000000) and survives deep sleep; cold power-on leaves it
   UNDEFINED (not zeroed), which the magic word guards. The field order is IDENTICAL to
   the host array in platform.c and the node's rtc* index constants (design §3). */
#define RTC_DATA_ATTR __attribute__((section(".rtc.data")))
#define OEVM_RTC_FIELDS 10
#define OEVM_RTC_MAGIC  0x94BE11C0u
static RTC_DATA_ATTR int32_t g_rtc[OEVM_RTC_FIELDS];
int rtc_target_valid(void) { return ((uint32_t)g_rtc[0] == OEVM_RTC_MAGIC) ? 1 : 0; }
int rtc_target_get(int i) { if (i < 0 || i >= OEVM_RTC_FIELDS) return 0; return g_rtc[i]; }
void rtc_target_put(int i, int v) { if (i < 0 || i >= OEVM_RTC_FIELDS) return; g_rtc[i] = v; }
```

- [ ] **Step 2: Add the deep-sleep leaf** in `zephyr/src/main.c`. Per [[magtag-deep-sleep-timebase]] and design §6: Zephyr's `poweroff.c` arms NO wake source (#83), so call the ESP-IDF sleep HAL directly, then start deep sleep. Also drive GPIO21 (NeoPixel power gate) inactive first so the strip's quiescent draw does not survive the sleep (design §10):

```c
#include <esp_sleep.h>
/* tuvm#94 stage 5: arm a timer wake and enter deep sleep. Direct ESP-IDF HAL call
   because Zephyr's sys_poweroff() arms no wake source (#83); the HAL is already linked.
   Never returns — the SoC powers down until the timer fires and the VM cold-boots. */
void deep_sleep_target(int seconds) {
    /* NeoPixel power gate (GPIO21) inactive so the strip draws nothing while asleep. */
    /* (reuse the existing gpio_set path if one is wired; otherwise a direct HAL write) */
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();   /* does not return */
}
```

  Verify the `esp_sleep.h` include path resolves under the Zephyr/ESP-IDF build (the C6 build already links `esp_sleep_*` symbols — grep confirmed). If `esp_deep_sleep_start` is not exposed, fall back to `esp_deep_sleep((uint64_t)seconds*1000000ULL)` (arms timer + sleeps in one call). Pick whichever the HAL headers expose; document which.

- [ ] **Step 3: Build for the S2** (Model B / .oimg device default per [[tuvm-esp32-port]]). Use the established S2 build recipe (the wifi conf). Expected: clean link — all four symbols resolve; `.rtc.data` fits rtc_slow_seg (the ASSERT in the linker script guards it).

- [ ] **Step 4: Host regression** — re-run the full nsl suite Path B and the aux gates ([[tuvm-vm-source-lists]]: read the gate line, not just "0 failed") to confirm the new prims did not break parity or the `-m32`/asan gates:

Run: the controller-owned full suite (`run_tests_oevm.py`) — confirm `portability-32`, `boot-gc-asan`, and the C unit tests stay green.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/src/main.c && git commit -m "feat(magtag): device RTC-retention struct + deepSleep HAL (tuvm#94 stage 5)"
```

---

### Task 9: Device witness (compressed cadence) + promotion

**Files:**
- Modify: `~/workspace/review/program-sequence.md`, `~/workspace/review/tests/CLAIMS.md`, `~/workspace/tuvm/zephyr/docs/BOARD_PORTS.md`
- GitHub: tuvm#94 close-out comment

**No code — this is the witness + the mandatory promotion (CLAUDE.md close-out rule).**

- [ ] **Step 1: Flash + witness.** Flash the S2 (manual BOOT0+RESET; soldered UART header is the only console — hold ONE handle, per [[tuvm-esp32-port]]). Temporarily compress the "half-hour" and "4-hour" constants so several strike wakes, a watch-end sync, a paint, and a real `deepSleep:`→timer-wake→cold-boot cycle all show within minutes (the stage-3 6× trick, now compressing the sleep too). Capture the UART log with pyserial ([[esp32-flash-console-loop]]: any open reboots the board; esptool's post-flash reset eats the banner).

- [ ] **Step 2: Confirm the three witnesses** (design §9): (a) strikes advance **monotonically across real sleep/wake reboots** — proving `.rtc.data` survived; (b) a watch-end paints a **measured** drift (real ppm, not the reactor's eternal `+0.0`); (c) a forced sync failure paints `ntp FAIL` with a climbing `sync` row. Test the **unattended** path unattended ([[diagnosing-a-wedged-node]]: an interactive trigger does not witness the autonomous timer/ISR path).

- [ ] **Step 3: Restore the real cadence** (30 min / 4 h) before the final flash; note in the close-out that the compressed run was the witness.

- [ ] **Step 4: Promote** (CLAUDE.md — an issue is not closed until durable knowledge reaches a mainline doc; cite which doc absorbed what):
  - `~/workspace/review/program-sequence.md` — a rung line: the deep-sleep clock strikes across real sleeps, state survives `.rtc.data`, drift is measured every 4 h.
  - `~/workspace/review/tests/CLAIMS.md` — a row for the ratchet-across-sleeps witness + the re-exec harness; note the scope limit (power draw not host-testable).
  - `~/workspace/tuvm/zephyr/docs/BOARD_PORTS.md` — the `.rtc.data` retention fact, the RC-oscillator drift caveat (unmeasured until first sync), and the `deepSleep:` HAL recipe.
  - Update the memory [[magtag-deep-sleep-timebase]]: flip status DESIGNED → BUILT + witnessed, and record the measured first-sync drift number.

- [ ] **Step 5: Close-out comment on tuvm#94** naming which mainline doc absorbed which knowledge, and stating what stage 5 does NOT cover (button wake #83, drift compensation, power measurement — design §10).

- [ ] **Step 6: Commit the promotions** (review/ is David's tree — edit only the three named docs, never sweep):

```bash
cd ~/workspace/tuvm && git add zephyr/docs/BOARD_PORTS.md && git commit -m "docs(magtag): deep-sleep clock witnessed — .rtc.data survives, drift measured (tuvm#94 stage 5)"
# review/ docs committed in that repo separately.
```

---

## Self-Review

**Spec coverage (design §1-§10):**
- §1 run-once re-architecture → Task 4 (skeleton), throughout.
- §2 ratchet → Task 3 (pinned), Task 5 (`applySync:`), Task 7 (witnessed across sleeps).
- §3 RTC state → Task 1 (seam), field table in Global Constraints, Task 4/8 (index constants + struct).
- §4 NTP 3-attempts → Task 5 (`trySyncUpTo3:`).
- §5 panel → Task 6.
- §6 four new prims → Tasks 1 (3 prims) + 2 (deepSleep:) host; Task 8 device.
- §7 control flow → Task 4 `main:` + Task 5 `onWatchEnd:`.
- §8 edge cases → midnight wrap (Task 4 `half=48`), cold boot (Task 4 `initColdState:`), strike-never-cut-off (Task 4 ordering), display fault (Task 6 catch).
- §9 testing → Task 3 (pure-formula nsl units) + the `test_deepsleep_node.py` harness driving the REAL node (Task 4 cold-boot, Task 5 ratchet/sync scenarios, Task 6 panel rows, Task 7 monotonic sequence) + Task 9 device witness.
- §10 scope boundaries → Task 9 close-out states them.

**No node-body duplication:** an earlier draft tested Tasks 4-6 by copying the node body into re-spelled `10`-`13` nsl files — verbatim duplication of a logic block, which a quality review flags. Revised: the file-backed RTC lets the Python harness drive the shipping node directly (pre-seed the struct, script NTP per wake). Only Task 3's tiny pure formulas remain re-spelled (minimal, `05_ntp_catchup` precedent).

**Note on the stage-4 midpoint fix:** the stage-4 close-out queued a one-shot sync-offset fix for the reactor's periodic. Stage 5 DELETES the reactor's periodic entirely (run-once, no VM timers), so that fix is **moot here** — there is no periodic to re-phase. Recorded so it is not mistakenly re-applied.

**Placeholder scan:** the only deliberately-deferred detail is the exact `esp_deep_sleep_start` vs `esp_deep_sleep` HAL entry (Task 8 Step 2) and the GPIO21 write path (reuse existing gpio_set if wired) — both resolved at device build against the real headers, with the fallback named. Every host task carries complete code.

**Type/name consistency:** RTC field indices (`rtcMagic`..`rtcSyncsWin` = 0..9), `RTC_MAGIC 0x94BE11C0` / node `magicWord -1799482944`, and the formula spellings (`secondsToNextBoundary:`, `driftPpm:over:`, `staleField:`) are defined in Task 3's units and reused character-identically in the node (Tasks 4-6); the harness re-tests them through the real node, so any drift surfaces. Prim ids 131-134 counted, parity typedef updated in the task that adds the last id (Task 2 → `PRIM_pf_deepSleep`).
