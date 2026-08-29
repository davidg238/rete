# A 48-day Thread echo soak nobody was reading — 720,878 round trips, and one unattributable day

**Date:** 2026-08-29 (extracted; the soak itself runs 2026-07-11 → present)
**Boards:** three nRF52840s on wallwarts, joined to `OpenThread-0437` (PAN `0x0437`,
channel 14). Briefly `c6bench` too, before it became a porta bridge.
**Rig:** each node UDP-echoes a small opaque payload to `udp_echo6.py` on
gw85224-01:7777 on a ~15 s cadence. The server logs one line per **successful round
trip**; that log is the entire instrument.
**Instrument:** `journalctl -u udp-echo6` on gw85224-01 — and **nothing else**. This
predates the space protocol, so none of it reaches porta, `docs/results/`, the tracker
or `review/tests/CLAIMS.md`.
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
| `c145:d47b:5430:db9b` | 213,516 | 37.28 d | 15.09 s | **34.45 d** | 2026-08-18 19:30:29 |
| `2867:4291:e295:1e9f` | 210,055 | 37.30 d | 15.34 s | 27.33 d | 2026-08-18 19:28:11 |
| `24c0:a5b7:5ba0:0bd1` *(c6bench)* | 2,849 | 1.81 d | 55.05 s | — | 2026-07-22 15:50 |
| `408c:16cb:f977:8626` | 2,175 | 0.51 d | 20.17 s | — | 2026-07-12 02:38 |

Counts as of 2026-08-29 15:00; the first row is live and still climbing.

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
gateway, `gw/liveness`, the status deck and the claims ledger. That is **rete#281** (P1),
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

An unchanged ML-EID after eleven days without power means the OpenThread settings survived
in NVS: same firmware, same credentials, same identity. Nothing was reflashed and nothing
needed to be. **§3's reading is confirmed — the 2026-08-18 event was power, not firmware.**

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

### What is still not known

Which physical board on which wallwart corresponds to which extended address. Nothing in
the record answers it, and the only way to find out is to power one and watch which address
disappears. Worth doing once, and worth writing on the boards.
