# tuvm#65 — evidence folder (2026-07-21)

Raw artifacts behind the two findings on tuvm#65 — the HTTP **accept ceiling** and the
**fatal RX-slab starvation**. Both are now root-caused and fixed, and the fix is verified
by a 60-minute soak; #65 is not closed until someone accepts that evidence. This folder is
the investigation record, not a document of record — per the repo's promotion rule the
durable knowledge lands in a mainline doc at close-out, and the likely home is
`~/workspace/tuvm/review/reliability-gaps.md`.

Hardware: ESP32-D0WDQ6 / WROOM-32, MAC `30:ae:a4:1e:7b:84`, console `/dev/ttyUSB1`,
DHCP `192.168.0.240`, serving `ThermalHttpNode.nsl` (`/thermal`, chunked SVG, 12 chunks).
Branch `fix/65-http-load-wedge`.

**Read in this order if you only want the answer:** Finding 2 → "Why the stuck connections
never time out" → "The fix". Finding 1 is a separate, simpler defect that happens to live
in the same issue.

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

### Why the stuck connections never time out — ANSWERED

`net conn` on a wedged node, read over UART on a handle held open since before the load
(`session-G.log`):

```
TCP        Context   Src port Dst port   Send-Seq   Send-Ack  MSS    State
0x3ffdbb88 0x3ffcea3c   18080        0          0          0  1460   LISTEN
0x3ffdc4e8 0x3ffceae4   18080    38254 3930841427   74448089  1460   ESTABLISHED
0x3ffdc290 0x3ffceb8c   18080    38260 3145407352 2673545321  1460   ESTABLISHED
0x3ffdbde0 0x3ffcec34   18080    38266 1168820119 2625408154  1460   ESTABLISHED
0x3ffdc038 0x3ffcecdc   18080    38274  156486268 1438057269  1460   ESTABLISHED
```

Four connections, all **ESTABLISHED**, four distinct client ports, alongside the listener —
and `net mem` in the same breath shows `RX 0/8 avail`.

**`CONFIG_NET_TCP_KEEPALIVE is not set`, so an idle ESTABLISHED connection has no timer of
any kind.** This was never a slow timeout or a long retry budget: *no timer exists*. The
retransmit path (`NET_TCP_RETRY_COUNT=9` @ `RTO=200 ms`, ~204 s) never applies, because
nothing is outstanding to retransmit — these connections are simply idle.

They are **un-accepted backlog entries**, not connections the application ever saw:
`inflight=0`, and had the app closed them they would read FIN_WAIT/TIME_WAIT rather than
ESTABLISHED. Four stuck connections is exactly `zsock_listen(listen_fd, 4)` in
`src/backend/zephyr/http_zephyr.c`.

The complete chain:

1. Load fills the 4-deep listen backlog with fully ESTABLISHED connections.
2. Each pins RX `net_pkt`s holding its request data.
3. The RX slab (8) exhausts.
4. With zero RX packets `zsock_accept()` can no longer succeed — and `http_poll()` treats
   **every** accept failure identically to "nothing pending" (`if (fd < 0) { k_msleep(1);
   return; }`), so the node spins forever, accepting nothing and saying nothing.
5. The backlog connections are idle ESTABLISHED with no keepalive ⇒ immortal.
6. Freeing RX packets requires receiving; receiving requires RX packets. Closed loop.

Step 4 is tuvm's own code, and `prj_esp32.conf` **predicted it**: *"a context-exhausted
`zsock_accept()` in `http_poll()` is indistinguishable from 'nothing pending' (both take
the same early return) — so a client would be silently dropped instead of hitting the
ring's graceful 503 shed."* That comment was written about context exhaustion; it came true
via RX-packet exhaustion.

This makes the fix menu concrete, and the cheap ones are additive:

- **`http_poll()` must distinguish accept failure from idle** (check `errno`; count it;
  surface it). Turns a silent permanent wedge into a legible one. tuvm-side, cheap.
- **Shrink the listen backlog** so the backlog cannot pin the whole RX slab.
- **`CONFIG_NET_TCP_KEEPALIVE`** so idle ESTABLISHED connections are reaped even if the
  trap is entered — the only lever that breaks the deadlock rather than avoiding it.
- **Raise `NET_PKT_RX_COUNT`** so backlog plus working set fit — raises the bar, keeps the
  trap.

## The fix (`be2e961`, plus the RX widening)

Four changes, layered so that one *ends* the failure and the others make it rarer or
visible. Ordered by what they actually do:

1. **Keep-alive on the LISTENING socket** — the only change that breaks the deadlock once
   entered. Set on the listener because the stuck connections are never accepted: Zephyr
   copies keep-alive params from the listening conn when an incoming connection reaches
   ESTABLISHED, so the listener is the only reachable place. After `keep_cnt` unanswered
   probes `tcp_conn_close(-ETIMEDOUT)` frees the conn and its RX packets **without needing
   to receive anything**. Timings in code (10 s idle / 5 s / 3 ⇒ ~25 s to reclaim); the
   Kconfig default of 7200 s idle is useless here.
2. **`http_poll()` distinguishes accept failure from idle** — only `EAGAIN`/`EWOULDBLOCK`
   is benign; anything else is counted, announced (rate-limited) and exposed as
   `http_accept_failures()`. This is the direct answer to "no crash, no log".
3. **Listen backlog 4 → 2** — backlog depth is a direct claim on `NET_PKT_RX_COUNT`,
   because a backlogged connection is fully ESTABLISHED and pins the RX packets carrying
   its request. Serves no fewer connections; `http_poll` drains one per tick either way.
4. **`NET_PKT_RX_COUNT` 8 → 16** — restores what #46's RAM trim halved. Raises the bar;
   does not remove the trap. Taken *in addition to* 1, never instead of it.

**Paid for from the VM arena, not the packet pools.** Keep-alive's per-connection state
overflowed dram0 by 112 B (#53's ceiling was already reached), so `run.sh`'s esp32 default
went 53248 → 49152. The pools are what the wedge exhausts, so shrinking them to fund the
fix would have traded the cure for the disease. Justified by measurement: `/stats` reported
live bytes 21,860 against the 53,248 arena. dram0 ended at 98.19% (3568 B spare, from
224 B).

### Verification

Against the trigger that reliably wedged the node — continuous 8-worker concurrent load,
health judged only after quiescing:

| build | result |
|---|---|
| pre-fix | **TRUE WEDGE at cycle 16** (`session-G.log`) |
| keep-alive + accept legibility | 40 cycles / ~18 min / 1863 served, quiesced health TRUE at every cycle (`05-session-fix-verified-40-cycles.log`) |
| + RX slab 8 → 16 — **the soak** | **130 cycles / ~60 min / 17,374 served / 405,722 refused, quiesced health TRUE at every one of 130 cycles**, zero WiFi or address-loss events, one accept-failure announcement (`06-session-soak-130-cycles.log`) |

The RX widening also collapsed the **client-side timeout** count — 1716 → 183 over a
comparable run, and 0/260 failures on a paced follow-up. Most of the "slow" tail was
requests stalling on RX descriptors rather than on the VM, which means the #61 latency
figures were partly measuring buffer starvation and are worth re-taking on this build.

The absence of a wedge is the weaker half of the evidence. The stronger half is that the
node **entered the condition and announced it**, then carried on serving:

```
http: accept failed errno=113 (n=1) — listener cannot accept, likely RX packet exhaustion
```

`errno 113` is `ECONNABORTED` — a pending connection aborted before `accept()`, which is
the signature of `tcp_conn_close(-ETIMEDOUT)`, i.e. the reaper firing. Mechanism confirmed
in the field, not merely a quiet run.

### Reproduction is timing-sensitive

Not deterministic. The serial hunt wedged on round 8 (~860 served) on one build, then ran
25 rounds / ~4200 served clean on a build differing only by the shell and a smaller VM
heap. **"Did not reproduce" is therefore not evidence of a fix** — any fix needs either a
mechanism argument or a very long soak. The concurrent 8-worker hammer is the faster
trigger (wedge in ~8 minutes, and note `ok:0` — under it the node serves nothing at all).

## Files

| file | what it is |
|---|---|
| `01-console-prefix-baseline.log` | pre-fix build. Boot banner then **nothing** across ~800 served requests and ~1000 refusals — the silence that made #65 read as "no crash, no log". |
| `02-console-fatal-mode-no-heartbeat.log` | the net_mgmt build. The fatal mode reproduced here; note the **absence** of `DISCONNECT_RESULT` and of `IPv4 address REMOVED`. That is the evidence the node stays associated and addressed while unreachable — it falsified the WiFi-deauth hypothesis. No census in this build. |
| `03-console-wedge-confirmed-with-census.log` | the netdiag build. The primary artifact: 409 census samples spanning healthy cycling → collapse → frozen. |
| `tools/hunt.sh` | the reproducer. Bursts of 300 unpaced requests; wedged on round 8. |
| `tools/timewait_probe.py` | the time-resolved TIME_WAIT measurement (Finding 1). |
| `tools/reset_watch.py` | reset into run mode and capture — for catching a boot banner. |
| `tools/attach_watch.py` | **DOES NOT DO WHAT ITS NAME SAYS — kept only as the record of a failed approach.** Setting `dtr`/`rts` False before `open()` does *not* prevent the reset. |
| `tools/probe_session.py` | the correct design: ONE serial handle held open across load, detection and interrogation. Also the soak driver. |
| `05-session-fix-verified-40-cycles.log` | first post-fix run (keep-alive + accept legibility). |
| `06-session-soak-130-cycles.log` | the 60-minute soak on the final build. |

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

3. **OPENING the port at all reboots the board — so a tool that attaches at wedge time to
   ask what happened destroys the state it came to read.** Setting `dtr`/`rts` False before
   `open()` looks like it should avoid the auto-reset circuit. It does not. Verified
   directly: two successive attaches to a long-running node both reported uptime restarting
   at ~5 s. This was not caught before it did damage — an interrogation of a wedged node
   returned "all pools full, no TCP connections, fresh `CONNECT_RESULT`", which was read as
   evidence of a crash-and-reboot when it was simply *a fresh boot caused by the act of
   asking*. The fix is structural, not a flag: hold ONE serial handle open from before the
   load until after the interrogation (`tools/probe_session.py`), so the only reset is the
   deliberate one at t=0.

4. **A detector that probes while the load is still running cannot tell saturation from a
   wedge.** Under sustained concurrent load the node genuinely cannot answer, so "N failed
   probes" is guaranteed and means nothing — an 8-worker hammer produced a confident
   "WEDGE CONFIRMED" for a node that was serving normally seconds after the load stopped.
   Quiesce the load first, then judge; a node that recovers in the quiet window was
   saturated, one that does not is wedged.
