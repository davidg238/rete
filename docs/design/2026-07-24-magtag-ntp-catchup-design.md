# MagTag bell clock — stage 4: WiFi + NTP catchup

Design record for tuvm#94 stage 4. Extends
`docs/design/2026-07-23-magtag-bell-clock-spec.md`, whose stage 4 section named the
Kconfig and the arena target but left the *correction behaviour* unspecified. This
doc fills that in. Stages 1–3 are shipped and witnessed on the MagTag; this is the
last rung, and the only one that can fail on RAM.

## The one architectural commitment (unchanged from the parent spec)

**The local timer is authoritative for STRIKING; NTP only corrects BETWEEN
strikes.** A WiFi outage degrades the clock's *accuracy* and never its
*reliability* — the bell always rings. Everything below serves that ordering.

## Decisions taken (2026-07-24)

1. **Correction aligns count AND phase, and re-phases on every sync.** A sync fixes
   both *which* bell rings (the count) and *when* it strikes (aligned to the real
   :00/:30 boundary). The first sync is a large re-phase; every later one is a
   sub-second nudge. Handling is uniform — no first-vs-later special case.
2. **Timezone: Phoenix, Arizona — hardcoded UTC−7, no DST.** Arizona does not
   observe DST, so a fixed −7 h offset is *correct year-round* for Phoenix, not the
   twice-a-year-wrong compromise a fixed offset is in a DST zone. This is why the
   simplest option is also the right one here. The offset lives in nsl (a product
   fact), passed into the prim; the parent spec's DST rabbit hole stays closed.
3. **Free-run and ring before the first sync.** The strike timer starts at boot from
   a boot-relative 00:00 and rings on the local crystal regardless of WiFi. NTP
   corrects it when it arrives. A board with no WiFi is a drifting clock, never a
   silent one — the reliability commitment above. In the happy path NTP lands within
   seconds of boot, up to 30 min before the first strike, so no wrong bell is heard.

## Why the seam returns seconds-since-midnight, not epoch

SMI maxes at **1,073,741,823** (2³⁰−1 on this 32-bit target; `src/span.h`). Unix
epoch seconds are ~1.75×10⁹ and **do not fit in an SMI**. So the prim must not hand
raw epoch time to nsl. It does the epoch→local-time-of-day reduction in C (64-bit)
and returns **seconds since local midnight**, 0–86399, which fits trivially. This is
also the *only* value the node needs — it never wants the date.

## The seam — one new prim

`platform ntpSyncTz: offsetSeconds`

- Performs `sntp_simple(server, timeout, &ts)` (one call: init/query/close,
  `include/zephyr/net/sntp.h:161`) against a **compiled-in server** (an NTP pool
  hostname; an IP is the RAM fallback — see §RAM).
- Adds `offsetSeconds`, reduces modulo 86400, answers **seconds-since-local-midnight
  (SMI)** on success.
- Answers **nil** on failure (no WiFi, no route, timeout) — the structural
  "couldn't sync", the same nil-means-structural convention as the display and bell
  seams.
- **BLOCKS** up to the SNTP timeout. Only ever called between strikes, the same
  discipline that orders the repaint after the strike.

`offsetSeconds` is passed from nsl (−25200 for Phoenix) because *which timezone* is
a product fact, exactly as the bell's 1–8 convention lives in nsl and not in C.

**Host seams** (for a deterministic suite test, mirroring `OEVM_NO_DISPLAY` /
`OEVM_NO_BELL`):
- `OEVM_NTP_SECONDS=N` → the host prim returns N as seconds-since-local-midnight,
  so the count+phase math runs on the host with a known input.
- `OEVM_NO_NTP` → the host prim returns nil, exercising the free-run/retry path.

## The correction algorithm (in the node)

Two 30-minute timers:

- **strike timer** — fires at the half-hour, runs `advanceAndStrike` (unchanged from
  stage 3: strike, then repaint).
- **ntp timer** — fires at the *midpoint between strikes*, maximally far from any
  strike, and runs a sync.

Flow:

1. **Boot.** The strike timer starts immediately (free-run from 00:00 — the bell
   rings regardless of WiFi). A **boot-sync retry** attempts `ntpSyncTz:` every ~30 s
   until the *first* success.
2. **On a successful sync** (`ntpSyncTz:` answers `N`):
   - **Count:** `halfHours := N // 1800` — the single assignment the counter model
     was built for.
   - **Phase:** cancel the strike timer; one-shot `after:` for
     `1800000 − (N mod 1800) × 1000` ms to the true next boundary; on that fire,
     resume `every: 1800000`. Uniform for first and later syncs — the first is a big
     re-phase, later ones sub-second.
   - On the *first* success only: arm the steady-state ntp timer at the inter-strike
     midpoint, and stop the boot retry.
3. **On failure (`nil`).** Leave the clock free-running, record the miss, retry next
   window. Reliability never depends on the network.

**Safety property that makes this correct:** syncs land between strikes, and
steady-state drift is sub-second per half-hour (10–50 ppm on the 40 MHz crystal), so
a correction **can never cross a strike boundary** — never skips a bell, never
double-rings. Only the first sync is a large jump, and it happens well before the
first strike.

nsl has no modulo, so `N mod 1800` is spelled `N − ((N // 1800) × 1800)`, as the
bell mapping and the uptime field already do.

Timer budget: `OEVM_MAX_TARGET_TIMERS = 8` — the strike timer, the ntp timer, and a
transient one-shot during a re-phase is at most 3. Ample.

## The panel's bottom block (the two rows stage 3 stubbed)

```
ntp   -0.3s
24h   +0.9
      -1.2
```

- **`ntp`** — at each sync, the correction magnitude = (what the clock *thought* the
  time was) − (what NTP said), in seconds. Steady-state sub-second (`-0.3s`); `--`
  before the first sync; the first sync itself is a huge jump, shown as `sync`
  rather than a misleading number.
- **`24h`** — the running max (`+0.9`) and min (`-1.2`) steady-state correction over
  a rolling 48-sync (24 h) window, reset daily, split across two rows with the label
  on the max line. The honest health readout: if it ever grows past a second or two,
  the crystal or the cadence is wrong.
- Everything above these rows is unchanged from stage 3. A sync does **not** repaint
  (it is between strikes); the panel is repainted only after a strike, so it is
  current at every boundary.

## RAM — the whole risk of this rung

The ladder isolated this stage on purpose. WiFi + SNTP + DNS at the **16 KB arena**:
`prj_esp32s2_wifi.conf` links at 16384 with ~11.8 KB spare, but **whether the node's
working set fits at runtime has never been tested** — 16 KB is below what #85 judged
adequate for this class of node.

Plan:
1. Link at 16 KB (known to link).
2. Witness the working set at runtime — associate, sync, strike, repaint — and watch
   for a heap exhaustion the 64 KB builds never hit.
3. Ordered fallbacks if it will not fit: **drop DNS and hardcode an NTP IP** (saves
   the resolver), then **drop SNTP and correct by button**, then **fix PSRAM
   (#96)**. Because this stage is last, a failure here costs the accuracy upgrade and
   nothing else — stages 1–3 stay shipped and working at 64 KB.

Two Kconfig constraints carried from the parent spec:
`CONFIG_ESP_WIFI_HEAP_SPIRAM` stays **off** until #96 is fixed (selecting a heap
whose backing region never came up is worse than the default); **do NOT add
`CONFIG_POSIX_API`** — it exports a bare `send()` that collides with oevm's own
`send()` dispatch function. And `run.sh:249` only auto-applies the git-ignored
credentials for `BOARD_ID=esp32` — extend it to `esp32s2`.

## Testing

- **Host suite test** (`nsl-tests/20_bell/05_ntp_catchup.nsl`): `OEVM_NTP_SECONDS=N`
  drives the count+phase math deterministically — assert `halfHours` and the
  computed ms-to-boundary for several N, including a boundary-exact N (mod 1800 = 0)
  and a near-midnight N (wrap). `OEVM_NO_NTP` asserts the free-run/retry path. This
  is the first stage whose core logic is host-testable without hardware — which
  matters most on the rung most likely to fail.
- **On-device witness:** console shows association + DHCP, the first-sync catchup
  (the jump from boot-relative to real Phoenix time), subsequent aligned strikes, and
  the ntp/24h rows filling. Plus the RAM witness in §RAM.

## Out of scope (carried from the parent spec)

DST; any timezone but Phoenix; correcting *across* a strike boundary (the schedule is
chosen precisely so that never happens); PSRAM (#96, an optimisation not a
prerequisite).
