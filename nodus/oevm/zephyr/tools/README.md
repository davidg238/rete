# Soak dashboard — Thread fleet health for the oevm bench

`http://gw85224-01:8085` — per-node ping counts and packet loss for the `SoakNode`
fleet, read straight out of the `udp-echo6` journal on the border-router host.

The echo oracle (`udp-echo6.service`, the round-trip gate for `NetBridge`) is
**untouched**: this is a read-only sidecar that tails its journal. Breaking the
dashboard cannot break the test it is watching.

| file | what |
|---|---|
| `soak_web.py` | the dashboard; deployed to `gw85224-01:~/soak_web.py` |
| `soak-web.service` | systemd unit; installed at `/etc/systemd/system/`, runs as `david`, `Restart=always` |
| `soak_replay.py` | replays a real journal through the health metric — **use before changing it** |

## Deploy

```sh
scp soak_web.py david@gw85224-01:~/soak_web.py
ssh david@gw85224-01 'sudo systemctl restart soak-web'   # sudo prompts: no NOPASSWD
```

State is in-memory, so a restart re-backfills 12 h from the journal — restarting is
cheap and loses nothing.

## The health metric — do not retune by taste

A node is **THIN** above 2% packet loss, **DEAD** after 60 s of silence. Loss is
counted from *cadence gaps* (a gap that skipped whole 15 s slots) over the last
`WINDOW_S` seconds — **not** cumulatively over the node's lifetime.

That distinction is the whole point. The original metric was a ratio accumulated
since a node's first-ever packet, so it could never forgive: one 14-minute outage
during a deploy left a node branded THIN for the rest of the day, hours after it
had recovered and gone perfectly clean. Counting from gaps is also phase-agnostic
on purpose — a node that holds its cadence but loses packets in the mesh reads as
lossy, while a node that merely *started late* does not.

**`WINDOW_S = 3600` is measured, not guessed.** `soak_replay.py --sweep` against
real fleet history (2026-07-12) shows both failure modes flanking it:

```
 window  slots   2% bar   verdicts
   30m    120     2.4   5fd2= 0.0% OK   | e295=0.0% OK | db9b= 1.7% OK     <- hides a sick node
   60m    240     4.8   5fd2= 0.0% OK   | e295=0.4% OK | db9b= 4.6% THIN   <- ground truth
   90m    360     7.2   5fd2=16.3% THIN | e295=1.4% OK | db9b= 4.7% THIN   <- old outage creeps back
```

Real loss out here is **bursty**, so a short window lands *between* bursts and calls
a genuinely lossy node clean; a long one lets a dead-and-buried outage re-brand a
node that has since recovered. Only ~60 min classifies every node correctly. It also
gives the 2% bar usable resolution (4.8 of 240 slots; at 30 min the metric cannot
express anything between 0.8% and 1.7%).

So: change the metric, then **replay it** and diff the verdicts.

```sh
ssh david@gw85224-01 'journalctl -u udp-echo6 --since -12h --no-pager' > j.log
./soak_replay.py j.log            # verdict per node
./soak_replay.py j.log --sweep    # sensitivity to WINDOW_S
```

## Correlating a THIN node with the radio

The dashboard says *that* a node is losing packets. To find out *why*, ask
OpenThread. `ot-ctl` is **not on the gw host** — OTBR runs in a container named
`otbr`, and `david` is in the docker group, so **no sudo**:

```sh
docker exec otbr ot-ctl neighbor table   # RSSI + Link Quality In, per child
docker exec otbr ot-ctl childip          # RLOC16 -> ML-EID  <- the join key
```

`childip` is the only way to tie a radio row to a node we log: RLOC16 and extended
MAC are not stable identity (see `../docs/BOARD_PORTS.md`), and the journal knows nodes
only by ML-EID.

This paid off immediately: `…5430:db9b`, the one THIN node, turned out to be the only
child in the mesh with **LQ In = 2** (every other child: 3) and the worst RSSI by 4–5
dB. Its 15 s cadence never slipped through a single dropout — the firmware was fine
and the frames were dying in the air. **That is a wallwart-position bug, not a code
bug.** Two independent instruments, application-layer and radio-layer, agreeing is
what makes the conclusion trustworthy.

## Attachment is not liveness

A healthy `child table` row proves nothing about whether the VM is alive: OpenThread
attaches from its own thread while the VM is wedged or dead. Seen live on 2026-07-12
— `…f977:8626` held the *best* link in the fleet (−57 dBm, LQ 3), happily attached,
while producing **zero** echoes for 11 hours.

The dashboard is built to be immune to this: every number on it is evidence a node
*produced*. That is also why `SoakNode` pings on its own port (`:54361`) rather than
the gate's (`:54360`) — a soak that silently stopped must read as DEAD, not as absent.
