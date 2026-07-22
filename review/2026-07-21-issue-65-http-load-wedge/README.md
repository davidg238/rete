# tuvm#65 — evidence folder (2026-07-21)

Raw artifacts behind the two findings on tuvm#65: the HTTP **accept ceiling** (fixed) and
the **fatal RX-slab starvation** (root-caused, not fixed). This folder is the investigation
record, not a document of record — per the repo's promotion rule the durable knowledge
lands in a mainline doc at close-out, and the likely home is
`~/workspace/tuvm/review/reliability-gaps.md`.

Hardware: ESP32-D0WDQ6 / WROOM-32, MAC `30:ae:a4:1e:7b:84`, console `/dev/ttyUSB1`,
DHCP `192.168.0.240`, serving `ThermalHttpNode.nsl` (`/thermal`, chunked SVG, 12 chunks).
Branch `fix/65-http-load-wedge` (`ad462bf`, `093df88`, `8622000`).

## Finding 1 — the accept ceiling was TIME_WAIT, and it is fixed

Every reply is HTTP/1.0 + `Connection: close`, so the **node** is the active closer and
parks each connection's `net_context` in TIME_WAIT. `CONFIG_NET_MAX_CONTEXTS=5` minus the
listener's permanent one leaves **4**, held `CONFIG_NET_TCP_TIME_WAIT_DELAY=1500` ms each:
a hard ceiling of 4/1.5 s = **2.67 req/s**. Everything above it is RST by the TCP stack
*below* `zsock_accept()`.

`tools/timewait_probe.py`, quiesced node, before the fix:

```
t=0,111,216,312 ms   OK       <- exactly 4 accepted
t=390 ms onward      REFUSED  <- RST, instantly
t=1624 ms            OK       <- resumes; 1624-111 = 1513 ms == TIME_WAIT
```

Load sweep before the fix — achieved throughput is **pinned** regardless of offered load,
which is a flat resource ceiling, not congestive collapse:

| offered | ok/failed | achieved |
|---|---|---|
| unpaced | 4/116 | 2.82/s |
| 20/s | 13/47 | 2.55/s |
| 10/s | 32/28 | 2.49/s |
| 4/s | 59/1 | 2.44/s |
| 2/s | 60/0 | 1.64/s (offered-bound, zero failures) |

**This re-reads #65's original rate table as ONE phenomenon, not two regimes.**
`latency_harness --delay` sleeps *between* requests, so nominal "4/s" is really
1/(0.25+0.11) = 2.78/s — right at the ceiling, hence 297/300 — and nominal "10/s" is
really 4.8/s, hence almost exactly half failing.

Fix: `CONFIG_NET_TCP_TIME_WAIT_DELAY=250` (`ad462bf`). After: 8/8 saturating requests
accepted, and **147 ok in 13 s = 11.3 req/s**, ~4.5x.

Corollary worth keeping: **the G2 503 shed cannot engage under connection-rate overload.**
The RST happens below `accept()`, so `http_accept_request`'s shed only ever sees a
connection already accepted. At this layer the graceful-degradation guarantee is a paper
one — which is why every observed failure was connect-level and never an `HTTPError`.

## Finding 2 — the fatal mode is net_pkt RX-slab starvation

`03-console-wedge-confirmed-with-census.log` — 409 heartbeat samples across a confirmed,
permanent wedge (declared only after 5 consecutive probes over 25 s failed **both** ICMP
and HTTP; see the false-positive note below):

```
  3 s   rx_free=7  ctx=1     idle
 23 s   rx_free=3  ctx=5     under load
 43 s   rx_free=7  ctx=1     recovered
  ...   clean 7<->3 cycling for 7 rounds / ~300 s ...
306 s   rx_free=0  ctx=5     <-- and never again
560 s   rx_free=0  ctx=5     frozen, 250+ s later
```

**Not a leak.** The pool cycles perfectly for seven rounds, then collapses to **0 free of
8** RX packets in a single burst and stays, with all 5 contexts pinned and `inflight=0`
(the VM's ring is empty — the application holds nothing).

Mechanism, and why it is permanent: with zero free `net_pkt`s the WiFi driver cannot
allocate a buffer to receive **any** frame, including the ARP request that would make the
node visible. Freeing them requires the stuck connections to complete or abort, which
requires *receiving* an ACK or FIN. The node is deaf and the cure requires hearing —
self-sustaining by construction.

Every symptom in the original report falls out of that:

| observed | explained by |
|---|---|
| no crash, no log | nothing faulted — the stack is starved, not broken |
| console silent | the heartbeat kept printing throughout: the kernel schedules fine, no deadlock |
| on the network but invisible | still associated (no `DISCONNECT_RESULT`, no address removal); the RX path is starved, not the association |
| only a hardware reset recovers | recovery needs RX buffers that only RX can free |

Implicates `CONFIG_NET_PKT_RX_COUNT=8`, halved from 16 in #46's RAM trim — whose own
comment says "trim or raise at bring-up if RAM is tight or connections stall".

**OPEN:** why the stuck connections never time out and release. TCP retransmit timers
should abort them and do not, across 4+ minutes. That answer decides whether the fix is
more buffers (raises the bar, keeps the trap) or making the trap arithmetically impossible
(cap concurrent connections below the pool size) — and it may land in Zephyr's TCP rather
than in tuvm at all.

## Files

| file | what it is |
|---|---|
| `01-console-prefix-baseline.log` | pre-fix build. Boot banner then **nothing** across ~800 served requests and ~1000 refusals — the silence that made #65 read as "no crash, no log". |
| `02-console-fatal-mode-no-heartbeat.log` | the net_mgmt build. The fatal mode reproduced here; note the **absence** of `DISCONNECT_RESULT` and of `IPv4 address REMOVED`. That is the evidence the node stays associated and addressed while unreachable — it falsified the WiFi-deauth hypothesis. No census in this build. |
| `03-console-wedge-confirmed-with-census.log` | the netdiag build. The primary artifact: 409 census samples spanning healthy cycling → collapse → frozen. |
| `tools/hunt.sh` | the reproducer. Bursts of 300 unpaced requests; wedged on round 8. |
| `tools/timewait_probe.py` | the time-resolved TIME_WAIT measurement (Finding 1). |
| `tools/reset_watch.py` | reset into run mode and capture — for catching a boot banner. |
| `tools/attach_watch.py` | attach **without** resetting, for reading a node mid-experiment. |

## Two methodology notes, both learned the expensive way

1. **A single probe cannot distinguish "busy" from "gone".** An early version of the hunt
   broke on one failed 6 s curl and reported a wedge; the node was serving 200s in 65 ms
   fifty seconds later. Everything inferred from that trace was wrong and was withdrawn.
   The detector now requires 5 consecutive failures of both ICMP and HTTP over 25 s. The
   same weakness applies to any health signal the node itself ever ships.
2. **`stty` on the console port reboots the board.** It asserts DTR/RTS, i.e. the auto-reset
   circuit, and can latch the target into download mode — where it is silent and off the
   network. This produced a "wedged node" that was really a board in the bootloader, on an
   issue whose entire subject is a node going unreachable. `cat` on the port also read zero
   bytes from a healthy node where pyserial read everything. Use the tools above.
