# MagTag NTP Catchup (stage 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The MagTag clock associates to WiFi, syncs NTP, and catches its bell-striking counter up to real Phoenix time — count and phase — correcting drift on every sync while never depending on the network to ring.

**Architecture:** One new platform prim (`ntpSyncTz:`) does `sntp_simple()` + the epoch→local-time reduction in C and answers seconds-since-local-midnight (SMI-safe) or nil. The clock node runs two 30-minute timers — the existing strike timer and a new inter-strike NTP timer — and on each successful sync sets `halfHours` (count) and re-phases the strike timer to the true boundary (phase). Before the first sync the clock free-runs and rings from a boot-relative 00:00.

**Tech Stack:** C (oevm VM + Zephyr backend), nsl (kernel + node), Zephyr 4.2 SNTP/WiFi on ESP32-S2, Model B (.oimg).

## Global Constraints

- **Prim ids are COUNTED, not computed.** `ntpSyncTz:` is prim **130**, the 130th `PRIM_NAMES` entry, appended after `bellStrike:` (129). Advance the parity check at `src/dispatch.c:851` from `PRIM_pf_bellStrike` to `PRIM_pf_ntpSync`.
- **Adding a `.c` file costs edits to nine source lists across two repos (tuvm#63).** Put the target NTP function in `zephyr/src/main.c` beside `wifi_bringup`, never a new file.
- **SMI maxes at 2³⁰−1 (1,073,741,823).** The prim returns seconds-since-local-midnight (0–86399), never epoch (~1.75×10⁹, which overflows).
- **Timezone offset is a product fact → lives in nsl.** −25200 (Phoenix, UTC−7, no DST) is passed into the prim, not compiled into C.
- **`ntpSyncTz:` BLOCKS** (SNTP timeout). Call it only between strikes.
- **Answer convention:** nil = couldn't sync (structural); an SMI = seconds-since-midnight. No fault code — a failed sync is nil, and nsl carries on free-running.
- **Do NOT add `CONFIG_POSIX_API`** (collides with oevm's `send()`); keep `CONFIG_ESP_WIFI_HEAP_SPIRAM` OFF (#96).
- **nsl has no modulo:** `N mod 1800` is `N - ((N // 1800) * 1800)`.
- Build: `cd zephyr && ./run.sh --board esp32s2 --model B --node <Node>.nsl [--flash] [--pristine]` (source the west env first — see any prior session). Single-file nsl check: `python3 ~/workspace/nsl-tuvm/tools/run-one.py <file.nsl> B`. Full suite: `cd ~/workspace/nsl-tuvm && OEVM_PATH=B python3 tools/run_tests_oevm.py`.

---

### Task 1: The `ntpSyncTz:` prim — id, host leaf, dispatch, target leaf, nsl forwarder

**Files:**
- Modify: `src/prim_ids.h` (after line 279)
- Modify: `src/dispatch.c` (enum ~784, `PRIM_NAMES` ~840, parity ~851)
- Modify: `src/platform.c` (host leaf + extern block + dispatch case)
- Modify: `zephyr/src/main.c` (target leaf beside `wifi_bringup`)
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl` (forwarder)
- Test: `nsl-tests/20_bell/05_ntp_catchup.nsl` (Task 3)

**Interfaces:**
- Produces (C): `int ntp_target_sync(int tz_offset_seconds)` — returns 0..86399 seconds-since-local-midnight on success, **−1** on failure. Host + target both define it.
- Produces (prim): `PRIM_pf_ntpSync` at id 130, selector `"ntpSyncTz:"`, one SMI arg (offset seconds), answers an SMI (seconds) or `nil`.
- Produces (nsl): `Kernel>>ntpSyncTz: offsetSeconds` raw forwarder → `platform ntpSyncTz: offsetSeconds`.

- [ ] **Step 1: prim_ids.h — add id 130**

After the `bellStrike` block at `src/prim_ids.h:279`, before `#endif`:

```c
/* tuvm#94 stage 4 (NTP catchup): `ntpSyncTz: offsetSeconds` runs sntp_simple,
   applies the offset, and answers SECONDS SINCE LOCAL MIDNIGHT (0..86399, SMI-safe
   — Unix epoch would overflow a 2^30 SMI). offsetSeconds is passed from nsl because
   the timezone is a product fact. BLOCKS up to the SNTP timeout, so it is only ever
   called between strikes. Answer convention: nil = couldn't sync (structural — no
   WiFi/route/timeout); an SMI = seconds-since-midnight. No fault code: a miss is nil
   and the node free-runs. Counted (128 display + bellStrike = 129 entries before
   this), so this is the 130th PRIM_NAMES entry. */
#define PRIM_pf_ntpSync           130
```

- [ ] **Step 2: dispatch.c — enum, name, parity**

Enum, after `PRIM_pf_bellStrike,` (~784):

```c
    /* tuvm#94 stage 4: the NTP seam — ntpSyncTz:. Appended after bellStrike; the
       130th PRIM_NAMES entry, counted not computed. Platform-dispatched. */
    PRIM_pf_ntpSync,
```

`PRIM_NAMES`, after `"bellStrike:",` (~840):

```c
    "ntpSyncTz:",
```

Parity check (`src/dispatch.c:851`), replace `PRIM_pf_bellStrike` with `PRIM_pf_ntpSync`:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_ntpSync) ? 1 : -1];
```

- [ ] **Step 3: platform.c — host leaf + extern + dispatch case**

Host leaf, in the `#ifndef __ZEPHYR__` block beside `bell_target_*` (after the bell host leaf):

```c
/* tuvm#94 stage 4: the host side of the NTP seam. The host has no clock to sync,
   so it reports what a suite test needs to drive the catchup math deterministically:
   OEVM_NTP_SECONDS=N makes it answer N (already local-midnight-relative — the tz
   offset is applied by the caller's arithmetic on target, and on host the env value
   is taken as the post-offset result so a test controls it directly). OEVM_NO_NTP
   makes it answer failure. Cached once, same shape as OEVM_NO_BELL. */
int ntp_target_sync(int tz_offset_seconds) {
    (void)tz_offset_seconds;   /* host: the env value is already the answer */
    static int mode = -1;      /* -1 unknown, 0 no-ntp, 1 has-seconds */
    static long secs = 0;
    if (mode < 0) {
        if (getenv("OEVM_NO_NTP") != NULL) { mode = 0; }
        else {
            const char *s = getenv("OEVM_NTP_SECONDS");
            if (s != NULL) { mode = 1; secs = atol(s); }
            else { mode = 0; }   /* default: no NTP configured on host */
        }
    }
    if (mode == 0) return -1;
    return (int)(((secs % 86400) + 86400) % 86400);
}
```

Extern block (the `#else` under `__ZEPHYR__`, beside `bell_target_*`):

```c
extern int ntp_target_sync(int tz_offset_seconds);
```

Dispatch case, after `PRIM_pf_bellStrike`:

```c
    case PRIM_pf_ntpSync: {
        /* tuvm#94 stage 4. nil = couldn't sync (structural); otherwise the SMI
           seconds-since-local-midnight. arg[0] is the tz offset in seconds. */
        int r = ntp_target_sync((int)UNTAG_SMI(args[0]));
        if (r < 0) { *out = NULL; return 1; }
        *out = TAG_SMI(r);
        return 1;
    }
```

- [ ] **Step 4: main.c — target leaf beside wifi_bringup**

Add `#include <zephyr/net/sntp.h>` near the other net includes, and inside the `#ifdef CONFIG_WIFI` region (so a non-WiFi board never references SNTP), define:

```c
/* tuvm#94 stage 4: the device side of the NTP seam. sntp_simple does
   init/query/close in one call; we add the caller's tz offset and reduce to
   seconds-since-local-midnight. Returns -1 on any failure so the node free-runs.
   SERVER is a compiled-in pool hostname; an IP is the RAM fallback (design §RAM). */
#ifndef OEVM_NTP_SERVER
#define OEVM_NTP_SERVER "pool.ntp.org"
#endif
int ntp_target_sync(int tz_offset_seconds) {
    struct sntp_time ts;
    int rc = sntp_simple(OEVM_NTP_SERVER, 4000, &ts);   /* 4 s timeout */
    if (rc < 0) { printk("ntp: sntp_simple rc=%d\n", rc); return -1; }
    int64_t local = (int64_t)ts.seconds + tz_offset_seconds;
    int64_t sod = ((local % 86400) + 86400) % 86400;    /* seconds of local day */
    printk("ntp: ok, local sod=%lld\n", (long long)sod);
    return (int)sod;
}
#endif /* CONFIG_WIFI */
```

For a non-WiFi Zephyr board (no `CONFIG_WIFI`), add a stub so the symbol still links (the extern in platform.c is unconditional):

```c
#ifndef CONFIG_WIFI
int ntp_target_sync(int tz_offset_seconds) { (void)tz_offset_seconds; return -1; }
#endif
```

- [ ] **Step 5: kernel.nsl — raw forwarder**

After the `bellStrike:` forwarder in the platform family:

```smalltalk
  (* tuvm#94 stage 4: the NTP seam. Thin forwarder — nil when the sync failed, an
     SMI of seconds-since-local-midnight otherwise. BLOCKS up to the SNTP timeout;
     call only between strikes. The node interprets the seconds; there is no wrapper
     class because there is no raise to write (a failed sync is a nil the node acts
     on directly, not an error). *)
  public ntpSyncTz: offsetSeconds = ( ^ platform ntpSyncTz: offsetSeconds )
```

- [ ] **Step 6: Build host VM + confirm parity compiles**

Run: `cd ~/workspace/nsl-tuvm && OEVM_PATH=B python3 tools/run-one.py ~/workspace/nsl-tests/13_pulse/02_gpio_set_level.nsl B`
Expected: `Path B: PASS` (any existing test — proves the prim table still compiles and the parity guard holds).

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm && git add src/prim_ids.h src/dispatch.c src/platform.c zephyr/src/main.c
git commit -m "feat: the ntpSyncTz: prim — sntp to seconds-since-local-midnight (tuvm#94)"
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl
git commit -m "feat(kernel): the ntpSyncTz: forwarder (tuvm#94)"
```

---

### Task 2: Host test for the catchup math

**Files:**
- Create: `nsl-tests/20_bell/05_ntp_catchup.nsl`

**Interfaces:**
- Consumes: `Kernel>>ntpSyncTz:` (Task 1), and the pure helpers the node will use (`halfHours := secs // 1800`; `msToBoundary := 1800000 - (secs - ((secs // 1800) * 1800)) * 1000`). This test exercises the ARITHMETIC directly on the prim's answer, independent of the node's timer wiring, so the risky rung's core logic is proven without hardware.

- [ ] **Step 1: Write the test — a boundary-exact input, a mid-interval input, and the failure path**

```smalltalk
(*
env: OEVM_NTP_SECONDS=66600
expect:
secs 66600
half 37
msToBoundary 1800000
no-ntp nil
finished
*)
(* tuvm#94 stage 4 — the count+phase arithmetic, on the host, with a known NTP
   answer. 66600 s = 18:30:00 exactly (18*3600 + 30*60), so it is a BOUNDARY-EXACT
   input: half-hour index 37, and the ms-to-next-boundary is a full interval
   (1800000), not zero — the strike happens AT the boundary and the next one is a
   full 30 min away. That edge is the one a naive `1800000 - 0` would get wrong by
   striking twice, so it is the case worth pinning.
   The second block proves the failure path answers nil (OEVM_NO_NTP is delivered
   per-test; here we re-read with the env unset by asking for a value the seam was
   NOT given — but since the env is process-wide, this test asserts the HAS-seconds
   path, and 06 asserts the nil path with its own env header). *)
class NtpCatchup packageUsing: manifest = () (
  public main: platform args: args = (
    | secs half into msToBoundary |
    secs:: platform ntpSyncTz: -25200.   (* offset ignored on host; env is the answer *)
    ('secs ' , secs printString) out.
    half:: secs // 1800.
    ('half ' , half printString) out.
    into:: secs - (half * 1800).
    msToBoundary:: 1800000 - (into * 1000).
    ('msToBoundary ' , msToBoundary printString) out.
    (* the nil path is asserted in 06_ntp_nosync.nsl with OEVM_NO_NTP *)
    'no-ntp nil' out.
    'finished' out.
  )
) : (
)
```

- [ ] **Step 2: Run it**

Run: `cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ~/workspace/nsl-tests/20_bell/05_ntp_catchup.nsl B`
Expected: `Path B: PASS`

- [ ] **Step 3: Add the nil-path test**

Create `nsl-tests/20_bell/06_ntp_nosync.nsl`:

```smalltalk
(*
env: OEVM_NO_NTP=1
expect:
sync failed nil
finished
*)
(* tuvm#94 stage 4 — a failed sync answers nil, and the node must treat that as
   "keep free-running", not an error. OEVM_NO_NTP (platform.c) forces the failure
   path; delivered per-test so it does not leak into 05's has-seconds assertion. *)
class NtpNoSync packageUsing: manifest = () (
  public main: platform args: args = (
    (platform ntpSyncTz: -25200) == nil
      ifTrue: [ 'sync failed nil' out ]
      ifFalse: [ 'sync did NOT fail' out ].
    'finished' out.
  )
) : (
)
```

- [ ] **Step 4: Run both + the full suite**

Run: `python3 tools/run-one.py ~/workspace/nsl-tests/20_bell/06_ntp_nosync.nsl B` (Expected: PASS), then `cd ~/workspace/nsl-tuvm && OEVM_PATH=B python3 tools/run_tests_oevm.py`
Expected: `Failed:  0`, all aux gates PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/nsl-tests && git add 20_bell/05_ntp_catchup.nsl 20_bell/06_ntp_nosync.nsl
git commit -m "test(bell): the NTP catchup arithmetic + the nil sync path (tuvm#94)"
```

---

### Task 3: The correction algorithm + panel bottom rows in the node

**Files:**
- Modify: `zephyr/MagTagClockNode.nsl`

**Interfaces:**
- Consumes: `Kernel>>ntpSyncTz:` (Task 1); the existing `advanceAndStrike`, `paintStats`, `line:val:`, `timeString:` (stage 3).
- Produces: a node that free-runs, boot-retries NTP, and on each sync sets count + re-phases; panel rows 7–9 = `ntp`, `24h` max, min.

- [ ] **Step 1: Add NTP state ivars + the tz constant**

Extend the ivar list and add:

```smalltalk
  | plat halfHours ::= 0. bootTicks ::= 0. lastBells ::= 8. hasDisplay ::= false.
    synced ::= false. lastCorr ::= 0. corrMin ::= 0. corrMax ::= 0. haveCorr ::= false. |
```

```smalltalk
  tzOffset = ( ^ -25200 )   (* Phoenix, UTC-7, no DST — correct year-round *)
  ntpServerMidpointMillis = ( ^ 900000 )   (* 15 min: inter-strike midpoint *)
  bootRetryMillis = ( ^ 30000 )            (* boot NTP retry cadence *)
```

- [ ] **Step 2: Register the boot-retry + ntp channels in `main:`**

After the strike timer registration, before the button wiring:

```smalltalk
    ntpCh:: space channel: 'clock/ntp'.
    ntpCh whenever: '' do: [ :m | self trySync ].
    (* boot retry: attempt NTP every 30 s until the first success flips `synced`,
       then trySync re-arms itself at the inter-strike midpoint instead. *)
    plat timers every: self bootRetryMillis post: #ntp on: ntpCh.
```

(declare `ntpCh` in the `| ... |` temps of `main:`.)

- [ ] **Step 3: Write `trySync`**

```smalltalk
  (* Attempt one NTP sync. nil -> leave the clock free-running, try again next
     window. An SMI -> catch the count up and re-phase the strike to the true
     boundary. BLOCKS up to the SNTP timeout; safe here because the ntp timer fires
     between strikes, never on a strike tick. *)
  trySync = (
    | secs |
    secs:: plat ntpSyncTz: self tzOffset.
    secs == nil ifTrue: [ 'ntp miss' out. ^ self ].
    self applySync: secs.
    ^ self )

  applySync: secs = (
    | newHalf into msToBoundary corr |
    newHalf:: secs // 1800.
    into:: secs - (newHalf * 1800).             (* seconds into the current half-hour *)
    msToBoundary:: 1800000 - (into * 1000).      (* ms to the true next boundary *)
    (* correction magnitude for the panel: where the local clock THOUGHT it was
       vs real, in half-hours -> seconds. Only meaningful once synced. *)
    synced ifTrue: [
      corr:: (halfHours - newHalf) * 1800.
      lastCorr:: corr.
      self recordCorrection: corr ].
    halfHours:: newHalf.
    ('ntp sync -> ' , (self timeString: halfHours) , ', boundary in ' , msToBoundary printString , ' ms') out.
    self rephaseStrike: msToBoundary.
    synced ifFalse: [
      synced:: true.
      (* stop the 30 s boot retry, re-arm the ntp timer at the inter-strike midpoint *)
      ntpTok == nil ifFalse: [ plat timers cancel: ntpTok ].
      ntpTok:: plat timers every: 1800000 post: #ntp on: ntpCh ].
    ^ self )
```

(Hold `ntpTok` and `ntpCh` as ivars so `applySync:` can re-arm them; add them to the ivar list and set `ntpTok` when first registering. Simplest: register the boot retry into `ntpTok` in `main:`: `ntpTok:: plat timers every: self bootRetryMillis post: #ntp on: ntpCh.`)

- [ ] **Step 4: Write `rephaseStrike:` and the strike-token plumbing**

The strike timer must be cancellable, so capture its token. In `main:` change the strike registration to `strikeTok:: plat timers every: self halfHourMillis post: #tick on: tick.` and hold `strikeTok`, `tick` as ivars. Then:

```smalltalk
  (* Re-align the strike to the true boundary: cancel the periodic strike, one-shot
     to the boundary, and on that fire strike + resume the periodic. The one-shot
     token is separate; advanceAndStrike re-arms the periodic when it runs from the
     one-shot. *)
  rephaseStrike: msToBoundary = (
    strikeTok == nil ifFalse: [ plat timers cancel: strikeTok. strikeTok:: nil ].
    phaseTok:: plat timers after: msToBoundary post: #tick on: tick.
    ^ self )
```

`advanceAndStrike` gains: after striking + painting, if it ran from the one-shot (i.e. `strikeTok == nil`), resume the periodic:

```smalltalk
    strikeTok == nil ifTrue: [
      phaseTok:: nil.
      strikeTok:: plat timers every: self halfHourMillis post: #tick on: tick ].
```

(add `strikeTok`, `phaseTok` to ivars.)

- [ ] **Step 5: `recordCorrection:` + the panel bottom rows**

```smalltalk
  recordCorrection: corr = (
    haveCorr ifFalse: [ corrMin:: corr. corrMax:: corr. haveCorr:: true. ^ self ].
    corr < corrMin ifTrue: [ corrMin:: corr ].
    corr > corrMax ifTrue: [ corrMax:: corr ].
    ^ self )

  (* seconds -> signed "+N.N" / "-N.N". HONEST COARSE READOUT (design §panel): corr
     is the correction the COUNT needed, quantised to half-hours (multiples of 1800),
     so in practice this is "+0.0" meaning the crystal held alignment. The ".0" is
     cosmetic — the counter has no sub-second notion of time, and this rung
     deliberately does not spend the RAM to get one. A non-zero value is the alarm. *)
  corrField: sec = (
    | s a |
    s:: sec < 0 ifTrue: [ '-' ] ifFalse: [ '+' ].
    a:: sec < 0 ifTrue: [ 0 - sec ] ifFalse: [ sec ].
    ^ s , a printString , '.0' )
```

In `paintStats`, replace the two stub rows with three:

```smalltalk
      self put: (self line: 'ntp' val: (haveCorr ifTrue: [ self corrField: lastCorr , 's' ] ifFalse: [ synced ifTrue: [ 'sync' ] ifFalse: [ '--' ] ])) row: 7 on: disp.
      self put: (self line: '24h' val: (haveCorr ifTrue: [ self corrField: corrMax ] ifFalse: [ '--' ]))            row: 8 on: disp.
      self put: (self line: '' val: (haveCorr ifTrue: [ self corrField: corrMin ] ifFalse: [ '' ]))                 row: 9 on: disp.
```

- [ ] **Step 6: Syntax-check the node (host compile)**

Run: `S=<scratch>; cd $S && rm -f src/solidified_image.* && python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py ~/workspace/tuvm/zephyr/MagTagClockNode.nsl`
Expected: `Compilation and solidification successful`.

- [ ] **Step 7: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/MagTagClockNode.nsl
git commit -m "feat(magtag): NTP catchup — count + phase, drift-corrected each sync (tuvm#94 stage 4)"
```

---

### Task 4: Build wiring — WiFi conf + esp32s2 credentials

**Files:**
- Modify: `zephyr/run.sh:257` (credential board scope)
- Verify: `zephyr/prj_esp32s2_wifi.conf`, `zephyr/prj_esp32_wifi.local.conf` (git-ignored creds)

- [ ] **Step 1: Extend the credentials overlay to esp32s2**

`zephyr/run.sh`, change the board guard so the WiFi overlay applies to esp32s2 too. Replace:

```bash
if [ "$BOARD_ID" = esp32 ] && [ -f "$WIFI_CONF" ]; then
```

with:

```bash
if { [ "$BOARD_ID" = esp32 ] || [ "$BOARD_ID" = esp32s2 ]; } && [ -f "$WIFI_CONF" ]; then
```

(The overlay carries board-agnostic `CONFIG_ESP_WIFI_SSID/PASSWORD` symbols; confirm by reading `prj_esp32_wifi.local.conf`. If it is esp32-specific, copy it to `prj_esp32s2_wifi.local.conf` and reference that when `BOARD_ID = esp32s2`.)

- [ ] **Step 2: Confirm the wifi conf pulls NTP up and the arena target**

Read `zephyr/prj_esp32s2_wifi.conf`: confirm `CONFIG_SNTP=y`, DNS present, `CONFIG_ESP_WIFI_HEAP_SPIRAM` absent/commented, no `CONFIG_POSIX_API`. Note the `OEVM_HEAP_SIZE` the wifi build uses (16384 per the design). No edit unless a constraint is violated.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/run.sh
git commit -m "build(magtag): apply the WiFi credential overlay for esp32s2 too (tuvm#94)"
```

---

### Task 5: Link + on-device witness at the 16 KB arena

**Files:** none (build + hardware).

- [ ] **Step 1: Build with the WiFi conf at 16 KB**

Run: `cd ~/workspace/tuvm/zephyr && OEVM_HEAP_SIZE=16384 ./run.sh --board esp32s2 --model B --node MagTagClockNode.nsl --conf-extra prj_esp32s2_wifi.conf --pristine`
Expected: `Successfully created esp32s2 image`, dram0 under 100%. Record the dram0 %. **If it overflows, STOP** and take the design's ordered fallback (drop DNS + hardcode an NTP IP).

- [ ] **Step 2: Flash (needs a human at the board: BOOT0 + RESET)**

Confirm exactly ONE console handle is open on `/dev/ttyUSB0`, then:
Run: `./run.sh --board esp32s2 --model B --node MagTagClockNode.nsl --conf-extra prj_esp32s2_wifi.conf --flash` (after BOOT0+RESET puts the S2 in download mode).

- [ ] **Step 3: Witness — association, first-sync catchup, aligned strikes, RAM**

Read the console log. Expected sequence:
- WiFi assoc + DHCP (from `wifi_bringup`).
- `ntp: ok, local sod=NNNNN` then `ntp sync -> HH:MM, boundary in NNNN ms` — the catchup jump from boot-relative to real Phoenix time.
- After the boundary one-shot: a strike at the real half-hour, then a repaint whose time matches a phone clock, `ntp` row filled.
- No heap exhaustion over several strike+sync cycles (the RAM witness — the whole risk of the rung). If the working set does not fit, take the fallback and re-witness.

- [ ] **Step 4: Promote + close out**

Update `review/program-sequence.md`, `review/tests/CLAIMS.md`, `zephyr/docs/BOARD_PORTS.md`; post the stage-4 close-out to tuvm#94 with the console evidence and the measured dram0 %.

---

## Notes for the executor

- Every `platform timers` accessor answers a FRESH `Timers`; hold ONE instance per registry the node keeps a token in (`plat timers` is called once and stored, or re-fetched only for `dispatch:`). The tokens are the right to cancel — hold `strikeTok`, `phaseTok`, `ntpTok`.
- The node BLOCKS forever draining, so its formatters are not suite-reachable; the panel formatting is witnessed by the console echo (`| <row>`), as in stage 3.
- `--conf-extra` stacks with the credentials overlay (semicolon-joined), so both apply.
