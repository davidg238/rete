# A field node profiles on request, and was silent before it was asked

**Date:** 2026-08-06 · **Issue:** #165 Task 6 · **Design:** D1/D2/D3/D6 of
`docs/design/2026-08-06-method-profile-over-porta-design.md`

The host suite is structurally blind to this: posix timers bypass the pulse mailbox, so a
green suite says the parse and the wiring are right and says nothing about a window closing
on a device. This is the gate that says it.

## What ran

| | |
|---|---|
| Board | **c6spare** (console `d2e21074…`, JTAG `40:4C:CA:44:6E:60`) |
| Node id | `c6-bench` (hard-coded at `PortaBridgeNodeC6.nsl:47`, independent of the devkit) |
| Build id | `porta-v1-final-862-g8410661-20260806T223944` — printed by the firmware, and the key the ELF is archived under |
| Build | `run.sh --pristine --node PortaBridgeNodeC6.nsl --board esp32c6 --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"` with `OEVM_METHOD_PROFILE=1` and the countdown macros unset |
| Mesh | attached `role=3` (router) at 22:40:53; porta `serve --db soak-2026-08-04.db` on gw85224-01 |
| Request | `porta put --db soak-2026-08-04.db c6-bench dbg/req 'profile 10000' --kind stream` → `wrote dbg/req seq=2` |

## The three assertions

**1. Zero `mprof:` lines before the request.** Scoped to this boot:

```
$ logread.py --board c6spare since 2026-08-06T22:40:52 | rg -c "mprof"
0
$ logread.py --board c6spare since 2026-08-06T22:40:52 | rg -c "tick "      # positive control
6
```

The control matters and so does its scope. **The count-based form of this check is a false
red**: `tail -n 2000 | rg -c "mprof"` answers `50` on this board, and all fifty are from an
unrelated `MethodProfileNodeC6` spike that ran 12:49–13:29 — nine hours and a different
image earlier. A window large enough to be safe is large enough to reach a previous
experiment. Scope the read to the boot under test, with `since`, or the control silently
reports the opposite of the truth.

**2. Exactly one window header.** Not several — a re-arming dump would mean the countdown
seam was still compiled in:

```
2026-08-06T22:41:51.432-07:00 mprof[window]: methods=177 activations=148442 overflow=0 cap=256
```

One header, 177 rows.

**3. The window is 10 s, and the 10 s is measured from delivery, not from the write.**

| event | time | |
|---|---|---|
| `porta put` returns | 22:41:35.0 | the write lands in porta's store |
| `tick 11 … cursor 1 → 2` | 22:41:41.413 | the node's next pull fetches the entry |
| `mprof[window]` | 22:41:51.432 | **10.019 s** after delivery |

The requested window was 10 000 ms and the node held it to 19 ms. The extra 6.4 s between
the operator's write and the start of counting is **inbound pull latency** — the node polls
on a 2 s cadence and this entry waited for the pull that fetched it (`cursor` advancing 1→2
is the fetch). That is the space's own delivery latency, not window error, and reporting the
window as "16 s" would have blamed the timer for the transport.

A dump arriving instantly would have meant the timer never armed and `profileStop` ran
inline. It did not.

## The resolved profile

177 of 177 rows resolved. **Zero `UNKNOWN`** — the sidecar and the image agree, which is the
thing that cannot be checked by eye: a map from a different build resolves nothing and looks
identical to a quiet program.

```
window=window methods=177 activations=148442 cap=256

       count       %  method
       32468   21.9%  Kernel_ByteArray>>u8At:put:
       30370   20.5%  Kernel_ByteArray>>u8At:
       14129    9.5%  Kernel_ByteArray>>byteSize
       13944    9.4%  Kernel_ByteBuilder>>add:
        8814    5.9%  Kernel_Socket>>receive
        8060    5.4%  Block_Kernel_Cbor_block40>>value:
        4384    3.0%  Kernel_Array>>at:
        3786    2.6%  Kernel_ByteBuilder>>at:
        3494    2.4%  Kernel_Array>>at:put:
        3288    2.2%  Kernel>>rankOf:
        2867    1.9%  Kernel_OrderedCollection>>at:
        1433    1.0%  Kernel_Array>>size
        1362    0.9%  Kernel_OrderedCollection>>at:put:
        1222    0.8%  Kernel_ByteBuilder>>size
        1156    0.8%  Kernel_Cbor>>emitHead:len:into:
         995    0.7%  Kernel_Cbor>>encode:into:depth:
         866    0.6%  Kernel_OrderedCollection>>add:
         866    0.6%  Kernel_OrderedCollection>>ensureCapacity
         853    0.6%  Kernel_ByteArray>>setRaw:
         853    0.6%  Kernel_ByteArray>>init_new

showing top 20 of 177 rows — percentages are of ALL activations, not of the shown rows.
```

**Four per-byte accessors are 61.3% of everything the node does**: `u8At:put:` 21.9 +
`u8At:` 20.5 + `byteSize` 9.5 + `ByteBuilder>>add:` 9.4. This is the standing
per-byte-accessor cost (#163) measured for the first time on a real bridging workload rather
than on a synthetic one, and it is what makes the codec the target rather than a suspect.

## What this says about D9, and what it does not

**`methods=177`, `overflow=0`, load factor `177/256 = 0.69`** — just under the ≤0.7 the
counter warns about at `nodus/tuvm/src/vm.c:388`, and no activation was dropped.

This does **not** replace Task 1's host number of `methods=224` (load factor 0.875). The two
measure different things and both are real:

- **224** is the host gate's whole run — every path the bridge takes, including start-up and
  test-only paths, accumulated over a long window.
- **177** is a 10 s window of a node in steady-state bridging on hardware.

D9's branch therefore depends on which case the table has to hold. 256 slots holds the
steady state with room to spare; it does **not** have margin for the union of everything the
image can do, and a window opened during start-up or during an unusual path would be nearer
224 than 177. The honest reading is that **the table is adequate for the case this rung
ships and marginal for the general one** — and since a saturated table now reports
`INCOMPLETE` with exact counts and lower-bound percentages rather than `INVALID`, saturation
degrades rather than lies. The choice between a 512-slot table, Space-Saving displacement
and scoping (D10) stays David's and stays out of this plan.

## What remains unmeasured

The counter's own cost at a realistic load. The 0.90% figure everyone cites was measured at
load factor 0.008 (Hanoi, 2 methods). At 177 live slots the probe chain is longer and the
cost is **unknown**, which is #166. This gate does not answer it, and D1's "acceptable in
the field" still rests on the 2-method number.
