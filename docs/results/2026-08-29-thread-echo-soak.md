# A 48-day Thread echo soak nobody was reading — 720,878 round trips, and one unattributable day

**Date:** 2026-08-29 (extracted; the soak itself runs 2026-07-11 → present)
**Boards:** three nRF52840s on wallwarts, joined to `OpenThread-0437` (PAN `0x0437`,
channel 14). Briefly `c6bench` too, before it became a porta bridge.
**Rig:** each node UDP-echoes a small opaque payload to `udp_echo6.py` on
gw85224-01:7777 on a ~15 s cadence. The server logs one line per **successful round
trip**; that log is the entire instrument.
**Instrument:** `journalctl -u udp-echo6` on gw85224-01 — and **nothing else**. This
predates the space protocol, so none of it reaches porta or the tracker. It reaches
`docs/results/` and `review/tests/CLAIMS.md` only because it was extracted by hand on
2026-08-29; nothing keeps either current.
**Issues:** rete#281 (P1 — the porta blindness this file documents), found during rete#79.

---

## 1. The answer

Two nRF52840s ran a 15-second Thread round trip to the gateway for **37.3 and 34.4
consecutive days with no interruption longer than ten minutes**. A third has 48.6 days
of span across two clean stretches. That is the longest continuous mesh-reliability
evidence this program has, and until this file it existed only in a systemd journal.

| node (ML-EID suffix) | echoes | span | mean interval | longest clean stretch | ended |
|---|---:|---:|---:|---:|---|
| `1e62:99b1:7766:5fd2` | 292,283 | 48.61 d | 14.37 s | **37.28 d** | still running |
| `c145:d47b:5430:db9b` *(`4ec0`)* | 213,516 | 37.28 d | 15.09 s | **34.45 d** | stopped 2026-08-18 19:30:29, **running again since 08-29 15:04 (§8)** |
| `2867:4291:e295:1e9f` *(`92c2`)* | 210,055 | 37.30 d | 15.34 s | 27.33 d | stopped 2026-08-18 19:28:11, **running again since 08-29 15:05 (§8)** |
| `24c0:a5b7:5ba0:0bd1` *(c6bench)* | 2,849 | 1.81 d | 55.05 s | — | 2026-07-22 15:50 |
| `408c:16cb:f977:8626` | 2,175 | 0.51 d | 20.17 s | — | 2026-07-12 02:38 |

Counts as of 2026-08-29 15:00; the first row is live and still climbing.

**Read §8 before quoting this table.** Its `ended` column was written at 15:00 and the two
stopped boards were re-powered four minutes later, so for the length of one extraction this
file said "dead" about two nodes that were about to come back. All three have been echoing
continuously since; re-confirmed 2026-08-30 12:53 (rete#281), when `4ec0` was unplugged for
82 s to re-verify §8's identity map and came straight back. That the summary and §8 had to
be reconciled by hand is the argument for §9's third addition.

"Clean stretch" means **no gap over 600 s**. Gaps under that are not counted because the
instrument cannot distinguish a lost datagram from a slow one (§4).

The payload grew over the soak — the survivor logs sizes 20/21/22/23 B for its first
~12 hours (5,553 of each, a stringified counter gaining digits) and 32/33/34/36 B
thereafter; the other two only ever show the 32–36 B family. So all three were reflashed
to a common build on 2026-07-12 around midday, and have not been touched since.

## 2. Every gap over ten minutes

The whole list, so that "clean" is falsifiable rather than asserted:

```
1e62 (survivor)
  1235s   2026-07-12 00:18:49 -> 00:39:24     setup day
  1284s   2026-07-12 00:39:24 -> 01:00:48     setup day
  4211s   2026-07-12 09:16:06 -> 10:26:17     setup day
  2090s   2026-07-12 12:12:39 -> 12:47:29     setup day
   821s   2026-07-12 12:51:14 -> 13:04:55     setup day
 96131s   2026-08-18 19:49:10 -> 2026-08-19 22:31:21    26.70 h — see §3

c145
  2175s   2026-07-13 03:30:04 -> 04:06:19
  2174s   2026-07-15 08:12:10 -> 08:48:24
  (then 34.45 days with nothing, until it stopped for good)

2867
   896s   2026-07-12 12:25:27 -> 12:40:23     setup day
  1140s   2026-08-08 20:31:06 -> 20:50:06  ┐
  4575s   2026-08-09 09:25:49 -> 10:42:04  │  a bad 27 hours — six gaps,
  1094s   2026-08-09 11:29:19 -> 11:47:33  │  the only cluster in the record
  5790s   2026-08-09 19:58:17 -> 21:34:47  │
  2160s   2026-08-09 22:01:02 -> 22:37:02  │
  1200s   2026-08-09 22:42:17 -> 23:02:17  ┘
 23819s   2026-08-18 10:09:12 -> 16:46:11     6.62 h, then dead 2h42m later
```

Everything after 2026-07-12 13:04 is real running. The five setup-day gaps are the
boards being flashed and are excluded from every "clean stretch" figure above.

## 3. The 2026-08-18 event

All three nodes went silent inside twenty-one minutes:

```
2026-08-18 19:28:11   2867  last echo — never returns
2026-08-18 19:30:29   c145  last echo — never returns
2026-08-18 19:49:10   1e62  last echo — returns 26.7 h later
2026-08-19 22:24:01   gateway shuts down
2026-08-19 22:28:43   gateway boots            (4 min 42 s outage)
2026-08-19 22:29:45   udp-echo6 starts
2026-08-19 22:31:21   1e62 resumes
```

**What this establishes.** `2867` and `c145` stopped while the gateway and the echo
server were both demonstrably up — the server went on logging `1e62` for another
nineteen minutes. Their stopping is real, it is theirs, and it is not a firmware fault:
the three ran the same build for 37 days, and a firmware defect does not take two boards
down two minutes apart on day 37 and leave the third running.

**What it does not establish, and this is the more useful half.** `1e62`'s 26.7 h gap
**cannot be attributed**, and the instrument is structurally incapable of attributing it:

- The gateway was up for all but 4 min 42 s of it (`journalctl --list-boots`: boot −1 ran
  2026-06-10 10:50 → 2026-08-19 22:24).
- The echo server shows `NRestarts=0` and had been running since that boot began — so a
  wedged socket in a python UDP server that never restarts would look **exactly** like a
  dead node, and would never self-clear.
- `1e62` resumed 96 seconds after a *fresh* server process started. That timing is what a
  server-side wedge would produce.
- But a server-side wedge cannot explain the other two, which never came back at all.

So the honest reading is: **two nodes died; the third's day of silence is unattributed.**
A log of successes cannot report its own deafness, which is the same shape as rete#269 —
an instrument reporting a failure that was not happening.

## 4. What this soak cannot tell you

Naming these is most of the value of the file, because the numbers above read as more
than they are.

- **There is no loss rate here.** The server logs only round trips that *completed*. A
  dropped request and a dropped reply are both invisible, and are indistinguishable from
  a node that simply did not send. The mean intervals (14.37 / 15.09 / 15.34 s) are the
  only handle on loss, and they are consistent with a low rate — but a mean interval is
  not a loss rate and must not be quoted as one.
- **The cadence is not pinned.** The survivor's 14.37 s mean against the others' ~15.1 s
  suggests the nodes do not share a period, or that its earlier payload used a different
  one. Nothing in the record says what the intended cadence is.
- **No node identity is recorded.** These are ML-EIDs, not names. The mapping to physical
  boards is by elimination against porta's `nodes.last_addr` (which identified `24c0` as
  `c6bench` and cleared the two C6s). §8 pins each nRF to a stable extended address, which
  is the best identifier available — but which *physical* board is which is still unknown,
  because nothing on a wallwart announces itself.
- **Nothing records the server's own liveness**, which is precisely why §3 ends in a
  question. A heartbeat line every minute, whether or not a node spoke, would have settled
  it — cf. the `@@rete-console@@ heartbeat port=up` line the console collector already
  emits for exactly this reason.

## 5. A discrepancy worth recording

`docs/next-steps-overview.md` describes "the 2026-08-19 power failure [that] took the
gateway down for 10 h 6 min". The machine's own boot record says the 2026-08-19 outage was
**4 minutes 42 seconds**. Both can be true and probably are — `porta` is hand-started with
no systemd unit, so a 4-minute reboot leaves the *gateway service* down until somebody
notices, and ten hours is what "until somebody notices" looks like overnight.

The distinction matters because the two readings have different fixes. "The machine was
down for 10 h" wants a UPS. "The machine rebooted in 5 minutes and porta stayed down for
10 h" wants a systemd unit, which is cheap and is not filed anywhere.

## 6. Re-extracting this

The journal is the only copy, and `journalctl --vacuum-*` or a full disk ends it. The
extraction is one pass:

```bash
ssh gw85224-01 'journalctl -u udp-echo6 --no-pager -o short-iso' \
  | grep -oE '^\S+ .*echo [0-9]+B <-> \[fdc5:[0-9a-f:]+'
```

per-node counts, spans, mean intervals and the gap list in §2 come from grouping those by
address and differencing consecutive timestamps. Nothing else is needed and nothing else
was used.

## 7. What is owed

The soak produces zero rows in porta, because there is no `PortaBridgeNodeDK.nsl` — the
nRF nodes predate the space protocol and speak only to `udp_echo6`. So the longest-running
evidence on this bench is invisible to every instrument the program actually reads: the
gateway, `gw/liveness` and the status deck. **The claims ledger is no longer one of them:**
`review/tests/CLAIMS.md` absorbed this file on 2026-08-29 (David's call), marked
`FIELD EVIDENCE — NOT A GATE` — because the evidence is real and falsifiable while the
instrument is a journal on one host that no suite reads. It becomes an ordinary row when
#281 lands. That is **rete#281** (P1),
which also carries the two smaller findings above: the echo server cannot record its own
deafness (§3), and `porta` has no systemd unit on gw (§5).

## 8. Resolved 2026-08-29: it was power

The two dead boards were re-powered by hand at ~15:04 on 2026-08-29. **Both rejoined the
mesh within seconds and resumed echoing, with their original mesh-local addresses
unchanged.**

| node | dead since | back at | time off |
|---|---|---|---|
| `c145:d47b:5430:db9b` | 2026-08-18 19:30:29 | 2026-08-29 15:04:43 | **10 d 19 h 34 m** |
| `2867:4291:e295:1e9f` | 2026-08-18 19:28:11 | 2026-08-29 15:05:55 | **10 d 19 h 38 m** |

An unchanged ML-EID after eleven days means the OpenThread settings survived in NVS: same
firmware, same credentials, same identity. Nothing was reflashed and nothing needed to be.
**Firmware is ruled out.** What replaced it was "it was power" — and that claim was wrong;
see §9.

`1e62` was not interrupted by the re-power (97 consecutive echoes across the window, no gap
over 60 s), so its run is unbroken and continues past 48.6 days.

### The identity map, which did not exist before

Re-attaching gave each board a fresh RLOC16 but kept its extended address, so the two can
finally be paired. This is the table to use, and the extended address is the stable half:

| extended address | ML-EID | RLOC16 (2026-08-29) | history |
|---|---|---|---|
| `7adfea8b3ee2b7b8` | `1e62:99b1:7766:5fd2` | `0x2011` | never stopped; one unattributed 26.7 h gap (§3) |
| `4ec0a41289155d57` | `c145:d47b:5430:db9b` | `0x2019` | cleanest of the three — two gaps in 37 days |
| `92c25962249ad23b` | `2867:4291:e295:1e9f` | `0x201a` | six gaps on 08-08/09, then 6.6 h on the day it died |

For contrast, the two C6s on the same mesh: `ea8479ad07358744` is the bell and
`2a6d68fecc1974ea` is `c6bench`. A fourth nRF52840, `f4ce36271f782f13`, is the border
router's own RCP on gw85224-01 — it is the mesh leader and is **not** a soak node; power-
cycling it drops every row in this file.

### The boards are now physically labelled

Done 2026-08-29 ~15:22, and it is the durable half of this whole exercise. Each board now
carries the first four hex of its Thread extended address in marker: **`7adf`**, **`4ec0`**,
**`92c2`**.

The method, because it is reusable and cost nothing: unplug one board, watch which address
leaves the child table and stops echoing, label it, plug it back in. `92c2` and `4ec0` were
each confirmed directly that way — a 98 s gap in `4ec0`'s stream at 15:20:27 is the label
being written. **`7adf` was then taken by elimination and never unplugged**, which is why
its 48.6-day run is still unbroken.

Use the extended address, **not the sticker MAC**. The sticker carries the factory 48-bit
address (`ec:34:e5:…` on one of them — a real OUI, U/L bit clear); OpenThread's 64-bit
extended address is generated randomly and has the U/L bit *set* on all three (`7a`, `4e`,
`92` — bit 1 high in each). They are unrelated and cannot be correlated without reading the
node over a wire. The generated address is also what every diagnostic in this file and in
`ot-ctl` actually prints.

One caveat on the labels: a full-erase reflash regenerates the extended address, so treat
the marker as *current identity*, not a permanent serial. The sticker MAC remains the
immutable backstop.

### Distance from the gateway is not the explanation

`92c2` is physically the furthest board from the gateway, which makes weak-link the obvious
hypothesis. The neighbour table refutes it (2026-08-29 15:25, all five nodes attached):

| RLOC16 | label | avg RSSI | LQ In |
|---|---|---:|---:|
| `0x200d` | `c6bench` | −51 dBm | 3 |
| `0x2013` | the bell | −58 dBm | 3 |
| `0x2019` | `4ec0` | −66 dBm | 3 |
| `0x201b` | `92c2` | −70 dBm | 3 |
| `0x2011` | `7adf` | **−72 dBm** | 3 |

**The weakest link belongs to `7adf` — the node with the 48.6-day unbroken run.** `4ec0`,
which has the cleanest record of the three, sits between the two. Link strength and
reliability are not ordered together here at all. And `LQ In` is 3 — the maximum — for every
node including the weakest: −72 dBm is roughly 28 dB above 802.15.4 receiver sensitivity, so
none of these links is marginal.

Whatever hit the two boards on 2026-08-18 was therefore indifferent to link quality *and*
to board health — `4ec0` had the best record of the three. That pointed at shared power,
which §9 then ruled out too.

### `92c2` — flagged, not convicted

It is the only node with a bad record: the sole gap cluster in the entire dataset (six gaps
in 27 h on 2026-08-08/09), a 6.62 h gap the morning of the day it died, and — on its first
re-attach today — a self-inflicted drop after 2 m 45 s of running.

That drop looked like a second, board-specific cause for the 2026-08-18 event, and this file
nearly said so on that one data point. It should not have: after a clean power cycle the
node was watched for **twelve unbroken minutes** (15:18:43 → 15:29:46, five echoes per 70 s,
in the child table at every check, `dropped=0`) — indistinguishable from the two healthy
boards, and more than four times the life of the run that failed. One drop immediately after
eleven days dark is re-attach settling, not a fault.

So §8's verdict stands as written — the 2026-08-18 event was power — and `92c2`'s history is
recorded here as **the thing to watch first** if a node misbehaves again, not as a diagnosis.

## 9. Retraction: it was not power, and the cause is not known

§8 said the 2026-08-18 event was power. **That is withdrawn.** It rested on a shared-outlet
assumption that the bench refutes: the three nRFs are in **three different places, 15–40 ft
from the gateway, on different circuits** (David, 2026-08-29). There is no strip to blame.

Once that goes, the evidence reads differently, and mostly the other way:

- **A 21-minute spread is not a mains event.** 19:28:11 → 19:30:29 → 19:49:10, in three rooms.
  A brownout does not arrive twenty minutes apart in three places.
- **`7adf` resumed 3 minutes after the gateway rebooted**, having been silent 26.7 h
  (08-19 22:28:43 boot, 22:29:45 echo server, 22:31:21 first echo). If that node had lost
  power, the coincidence with a reboot a day later is unexplained. If the **gateway side**
  was broken, the node was fine throughout, retrying, and reconnected the moment there was
  something to connect to. The second explanation costs nothing extra.
- **The recovery of the other two on power-cycle proves less than §8 claimed.** A node stuck
  detached — settings intact, unable to rejoin, cleared by a restart — behaves identically
  to one that had no power. Today's recovery cannot distinguish them, and §8 treated it as
  if it could.
- **Nothing gateway-side is recorded, and the log that would record it is gone.** gw's system
  journal for 2026-08-18 19:00–20:15 holds only cron and tailscale. The OTBR runs in docker
  and that container started `2026-08-20T05:29:48Z` with `restarts=0` — its logs begin
  *after* the event. The single place a border-router or RCP fault would have appeared no
  longer exists.

**Verdict: cause unknown, gateway side the leading candidate.** Firmware is ruled out (§8),
credentials are ruled out (the dataset never rotated), distance and link quality are ruled
out (RSSI, above), and shared power is ruled out (this section). What is left is unproven.

### What would settle it next time

The instrument has to be able to implicate itself. Three cheap additions, in order of value:

1. **A heartbeat line from the echo server** every minute whether or not a node speaks —
   exactly the `@@rete-console@@ heartbeat port=up` line the console collector already
   emits. It separates "no node spoke" from "the server stopped listening", which is the
   distinction this whole section founders on.
2. **Persist the OTBR's logs** outside the container, so a border-router fault survives the
   reboot that clears it. Today the reboot destroyed its own best evidence.
3. **Get the nodes into porta** (rete#281). A node in the space is watched by `gw/liveness`,
   which forms a verdict on a ticker rather than at read time — the one mechanism here that
   is designed to notice silence.

This section is the argument for #281 in a sentence: **a channel nothing reads cannot tell
you why it went quiet.**
