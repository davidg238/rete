# rete: a lifecycle for small radio nodes on Zephyr — what is built, what it costs, and what is not

_This copy shows the appendix listings without their comments. The paper of record, with the comments lifted into
numbered references after each listing, is `nsl-embedded-paper.md` beside it; the prose is identical._

_DRAFT 4, 2026-10-10 (§9 built on both bells; §3 the AOT encoder; Appendices A–C in the lifted-references form, decided 2026-10-09; a bare-listings copy and a paginated PDF beside it), for iteration with David. The companion paper, on the language, is [nsl-design-paper.md]; each topic has one owner and the other paper cites it._

Sources are bracketed keys, e.g. [VM], resolved in the References; in this Markdown they are links, and only there.
Anything that is designed but not built says so in the sentence that describes it.

## Abstract

This is a report on a system called rete: what was built, why, what it costs, what it does not do, and how it tries to
improve the develop, deploy and manage lifecycle of a small radio node. Zephyr reaches more than 900 boards, with
connectivity, security and drivers built in; its community names the learning curve as its most persistent cost
[ZT10]. rete puts a small object language, nsl, on top of Zephyr, and builds the lifecycle tools around it.

The costs come first. The interpreter is about a hundred times slower than C on the one method measured both ways, the
VM takes 112 KB of a 256 KB part, and no model was trained on nsl. In return, a program can be changed without
reflashing the firmware; debugging, patching and profiling are driven from a host over the one USB cable already on the
bench; a gateway holds what a sleeping node is to be told until it next wakes; and the loop is short enough to check, by
running it, the code an agent wrote.

*Status: a lifecycle system whose desk half (host VM, USB load, debug, patch, profile) is built and measured, and whose
field half is built for debug and for program over the air on both demo nodes; version 0.1 ships when every verb is
shown there (decided 2026-10-09), so nothing in the release is labelled "designed".* Remote debug through a gateway
was run on two boards on 2026-10-09 and 2026-10-10, a deep-sleep node over WiFi and an always-on node over Thread, and
a program was pushed through it to each on 2026-10-12 (§9).

## 1. The problem

Shipping a node is an obstacle course of five things: the chip (choose, source, bring up and live with it for a decade);
the drivers (every sensor and bus, written and then maintained); the software (application logic safe enough to run
unattended); deployment (the first version out, and the fiftieth); and maintenance (diagnosing and fixing a fault in a
device you cannot reach). Only one of them is a language problem, and it is not the one benchmarks measure [Perf].
Zephyr removes the first two. This paper is about the last three, and how rete tries to improve them [Script].

Three problems sit under that frame. None is new or claimed solved.

**The learning curve.** Zephyr's ten-year survey reports it as the most persistent challenge for contributors
[ZT10]. A newcomer who wants a sensor reading on a radio node meets devicetree, Kconfig, west and the
network stack's configuration before any application logic runs. The layers are not the subject here; the application
on top of them is.

**The field node you cannot reach with a probe.** On the bench, a node has a debug probe or a cable. In a wall, on a
roof or in a box, it has neither. It reports through a weak, slow radio and often sleeps for minutes or hours between
exchanges. The usual tools are the log it chose to send and a site visit. A fault that appears only in the field,
after a week, on the node that is hard to reach, is the one you would most like to stop at a line.

**Code an agent wrote, and no way to trust it.** Zephyr's contributors already use AI to write code and do not trust it. Of 125 contributors surveyed, 54 % say it helps them write code faster, and 54 % say it gives unexpected or
incorrect output they must fix frequently; 33 % spend more time validating its output than they would writing from
scratch; 18 % say it helps them write code with fewer errors; and 78 % do not trust its suggestions without heavy
review ([ZT10], Figure 35). This is a verification problem: generated C must be read closely, line by line.

## 2. Lineage: the 1996 answer, with the liveness moved off the node

The idea of a high-level object language on an embedded target is old. Kim Clohessy of Object Technology International
(IBM) described it in *Using object-oriented programming tools to build real-time embedded systems* [Clo96]. His
account, in the facts this paper uses:

- In most real-time embedded systems only about 10 % of the code is time-critical; the rest runs at human speeds. A
  high-level language is used for that rest, and C or assembly for interrupt service routines and hot mathematics.
- The VM and the executable part of the image are burned into ROM, with object state in RAM.
- Development happens on a host, and a packaged image is downloaded to the target.
- The target hardware and its environment are modelled in software, so the application runs before the hardware exists.
- He closes by claiming that the three historical barriers, no cross-development tools, images too large and
  inadequate performance, "have been overcome", and remarks that profiling tools "will probably never be considered
  good enough".
- His Smalltalk kept on the target a live image, an in-image debugger, a process model, an easy call to C and "fixed"
  (non-moving) objects.

rete keeps the first four. A 256 KB radio part cannot carry what his Smalltalk kept on the target: a live image and an
in-image debugger cost more RAM and flash than the part has to give. So the liveness is moved off the node. The node holds a frozen, read-only
image and a confined mirror that a debugger can use; the gateway (porta) and the desk peer (custos) hold the rest of
what a live image gave, and reach the node through the wire. The same split decides where the debugger, the patcher
and the profiler live (§8, §9).

The earlier record is Barry's OOPSLA '89 paper [Bar89], Clohessy's own reference: a radar signal processor
prototyped in Smalltalk on 68k VME boards, with the live image on the target. Three of its practices are rete's,
unchanged after thirty-five years. The system was begun as a simulation and grown by stages, developed on PCs and
moved to the target unmodified, which is Clohessy's fourth point and rete's host VM first (§5, §7). Optimisation was
deliberately postponed until the architecture was stable and then measured, "no guesswork", which is §3. And the
first of its three optimisation options was to re-code a frequently invoked method as a primitive in C or assembler,
which is the mechanism below. The paper also records a cost this one concedes in §11: its experienced C programmers
took two to four months to become effective in Smalltalk, and "most found the long training period discouraging". Its
closing open question, whether there is room for code at an intermediate level between Smalltalk and assembler and
where the boundary falls, is the question rete #383 and the companion paper's three kinds of C are still answering.

One mechanism carries over from this lineage and is owned by the companion paper [Lang]: every operation is a send,
and a send finds its method in a table, so whether a method is interpreted or C is one field of its binding. A hot
method can move to C without its callers, the interpreter or the image format changing. That is native substitution.
Its per-activation cost, and the one direct comparison with C, are in the costs table (§11).

## 3. Where the time goes: two measurements

The VM is not the bottleneck on one workload, and is on another. Both are real.

**(a) A sensor and a web page.** Rung #61 (2026-07-21) measured `ThermalHttpNode` on a classic ESP32 (WROOM-32): a
Grid-EYE thermal sensor read over I2C, with 310 thermal jobs and 296 paced HTTP requests, timed on the board with
seven spans [Spans]. Four of them:

| Span | Mean | Max / min |
|---|---:|---:|
| `acquire` (the I2C read) | 12,798 µs | 1.24× |
| `vmCompute` | 4,101 µs | 1.21× |
| `publish` | 8,225 µs | 1.15× |
| `egress` (the network, C-owned; provisional, see below) | 1,319 µs | 727× |

Everything the VM does for a job (`vmCompute` plus `publish`) is 12.3 ms, at parity with the sensor read. Two thirds of
the VM's share was copying the same 64 numbers twice, an OrderedCollection build and then a `List fromCollection:`.
The network half is provisional: it comes from a run paced below the node's connection ceiling, on a build whose
receive-packet slab was itself the bottleneck (#65, fixed 2026-07-22, not re-taken). On that run the client's median
round trip was 159 ms and `egress` spread 727× from its minimum; the roughly 119 ms of network (75 %) is a subtraction,
not a measurement. The VM's work is predictable and at parity with the sensor; on that provisional run the network is
the wild part; and garbage collection on a saturated heap sat inside the mean, not in the tail: the heap was pegged at
53,228 of 53,248 B with 1,523 collections during the run, and `vmCompute` still showed a max/min of 1.21×.

**(b) The CBOR encoder, the one place the cost was felt.** Every node encodes what it sends to the gateway as CBOR.
The encoder is nsl, with C only at the leaves: integers, floats and the decode heads have been primitives since the
codec's first version (2026-07-09). On an ESP32-C6, encoding the gateway's real 425-byte push frame took 124 ms; a fix
in nsl alone (less copying, a cheaper byte buffer) halved it, to 53 ms, about a quarter of a check-in [Checkin].
Measuring where the rest went showed that the cost is the walk itself: about nine interpreted activations per byte, of
which the CBOR logic is half a percent [CborShape]. At a check-in every two seconds that is tolerable. At a 50 Hz
control loop it is the whole tick.

A spike tried four ways of encoding the same frame, all producing identical bytes, on the host (2026-10-04) [CborArms]:

| Arm | ns per encode, host | Against today |
|---|---:|---:|
| Today: the walk in nsl, C leaves | 259,950 | 1× |
| More of the leaves in C, the walk still in nsl | 86,595 | 3× |
| The whole walk in C, which knows the container classes | 1,904 | 137× |
| The walk in C, finding each container's encoding through its class's method table | 2,045 | 127× |

More leaves in C buys only 3×; the walk has to move. The chosen design is the last row: each container class declares
in one line of nsl that its encoding is native, and the C walk asks each class's method table for that binding, so C
does not name the class hierarchy, and the nsl encoder stays as the authority for anything C refuses [CborNative].
**This is designed and planned (rete #297), not built.**

A second spike (2026-10-09/10) asked whether the walk has to be written in C at all, or whether the nsl encoder could
be compiled ahead of time, with the sends it makes to nsl-defined objects kept as calls back into the VM. Three
encoders of the same 425-byte frame, byte-identical, in one binary on an ESP32-C6 [CborAot]:

| Encoder | per encode, ESP32-C6 | Against today |
|---|---:|---:|
| Today: the walk in nsl, C leaves | 43.6 ms | 1× |
| The nsl encoder compiled, every send to an nsl object kept, each held object rooted | 1.45 ms | 30× |
| The whole walk in C, which knows the container classes | 0.15 ms | 280× |

A kept send costs what an interpreted activation costs, about 23 µs on this part, and that is the whole 9.5×
between the second row and the third; rooting a fresh object across a send, which a C local does not do here, costs
about 3 µs more. The second row is enough: it takes the encoder from a quarter of a check-in to noise, from one
source of truth, and the price of that path is a compiler that knows each send site's receiver class, which no
instrument collects today. Both compiled arms were hand-written C standing in for a compiler's output; the ratios are
the device's, the compiler is not built.

Decisions are cheap, per-byte walks are not, and the fix for a walk is native substitution [Lang].

## 4. The system at a glance

![The system](img/nsl-system.svg)

_Figure 1. Field nodes (A′, B′) run commissioned images and reach the gateway over Thread or WiFi; porta mirrors
each node's state and keeps the images it commissioned. Each developer works on a twin on the desk (A, B) over one
USB cable, and on test nodes on the desktop's VM, through custos, with the same HTTP calls whether a person or an
agent makes them. A and A′ drift apart while a bug is chased on both, which is why porta, not the developer's
machine, is to be the custodian of what runs in the field (decided; the commissioning step is not yet built).
Reaching a field node's debugger through porta is built and was run on both bells, 2026-10-09 and 2026-10-10, with
no cable on the board (§9) [Field]._

porta (Go, always on), custos (Go, on the desk) and the node meet only in the wire format `protocol/` [Porta], [Peer].

**porta** is one always-on Go binary on the gateway host, with a SQLite store. It mirrors each node's `obs/` and `tel/`
paths. It holds intent written under `goal/` until a node that is asleep, and so has no address to be called on, next
wakes and asks. It refuses any write the manifest does not declare. And it judges each node's liveness from outside,
against the cadence the node declared [Porta]. Retaining each commissioned image and its symbol map at porta, as "the
custodian of what runs in the field", is decided and not built.

**custos** runs on the desk, holds the board's cable and serves the HTTP verbs; a person and an agent make the same
calls [Peer]. **The node** runs the VM as one Zephyr thread over a read-only program image, with Zephyr in C beneath it.

![The node's stack](img/e-node-stack.svg)

_Figure 2. One node. Zephyr, OpenThread and mbedTLS are C and unchanged. The VM is one Zephyr thread running a
read-only program image in place from its own flash partition; the liveness observer is a second C thread watching
its progress. The console cable carries the log, the loader and the debugger's lines._

RAM is the tight resource. On the nRF52840, with the Thread build of 2026-10-03, Zephyr, OpenThread and
mbedTLS take about 140 KB of 256 KB, the VM's arena gets 112 KB, and 4 KB are spare at link time [Lang], [Boards]. The VM's code is about 42 KB of a 1 MB flash, so flash is not the
axis that binds.

## 5. What you write

An nsl program is a class whose header names what it was given. It keeps state under path names in a **space** and
reacts to writes at those paths. Appendices A and B show one whole program in its two power modes: the ship's-bell
clock as the MagTag runs it, asleep between half-hours, and as the wall-powered C6 runs it, always on — both
written over one library of classes with one responsibility each (Appendix C) and a `main:` that reads as the
order of one wake, or of one boot. Two parts follow.

The first is that order. The node is asleep between wakes and the VM does not exist while it sleeps, so there is no
loop here: a wake strikes, syncs if it is a watch end, joins the gateway, reports, and sleeps.

```
record valid
  ifTrue:  [ self strikeWake ]
  ifFalse: [ self coldBoot ].

self joinPorta.            (* the first check-in replays the breakpoint set *)
self report.
space drain.               (* the row above is captured by a reaction; only a drain runs it *)
bridge checkIn.
^ self sleep               (* NEVER RETURNS *)
```

The second is the method that reports, once per wake:

```
report = (
  space out: 'tel/log' value:
    (platform Record tag: #Log with: #lvl -> #info
       with: #txt -> (summary , ' up ' , record bootTicks printString
                      , ' held ' , bridge heldExchanges printString)).
  ^ self )
```

It appends one row to a log stream; the bridge pushes it at the next exchange. The node in Appendix A uses these
paths across the wire:

| Path | Kind | Who writes it | Who reads it |
|---|---|---|---|
| `goal/debug/main` | cell | the gateway, as intent: the breakpoint set | the node, at its first check-in of every wake |
| `obs/build`, `obs/liveness` | cells | the node, as what it is and how often to expect it | the gateway |
| `tel/log` | stream | the node, a row per wake (the wall node: a row per strike) | the gateway, every row |
| `sys/metrics` | cell | the kernel, the arena's numbers | the gateway |

A *cell* holds the latest value; a *stream* is append-only. Every path that crosses the wire has its kind and record
shape declared once, in the protocol's manifest, and the gateway refuses a write that disagrees [Manifest]. Paths
under `sys/` stay on the node.

**Your Zephyr driver, a 13-line kernel class, and four primitives.** There is no FFI. A device is reached through a
*primitive*: a selector on a closed list, 160 of them today, each backed by one C function that calls the Zephyr
driver API. To bind a new driver, a firmware author appends one name to the list, writes the C function, and adds
the kernel method that hands the device out through `platform`. An application cannot add a primitive of its own
[Serial]. The driver in Figure 3 is Zephyr's; nsl adds glue.

![One call, five layers](img/nsl-display-binding.svg)

_Figure 3. `display at: 4 col: 0 put: …` from the app down to the panel. The driver is Zephyr's (its character
framebuffer and the uc81xx e-paper driver); nsl adds a 13-line kernel class, four primitives, and the C that hands
their arguments to Zephyr. The host has its own board layer, a real character grid, so the same program can be
checked on the desktop first [Display]._

<p align="center"><img src="img/magtag-clock.jpg" width="240" alt="A MagTag showing an nsl clock node's status screen"></p>

_Figure 4. The same primitives in service: an nsl clock node on a MagTag, 19 days up, showing its heap, clock drift
and last NTP check. Photographed under the previous project name, `tuvm` [Clock]._

The cost: someone must write the C. The language moves driver work to the firmware's author and keeps it out of the
application; it does not remove it.

## 6. The reactor in practice

Zephyr is multithreaded and nsl is not. The VM runs on one Zephyr thread, with OpenThread, the network stack and the
liveness observer in threads around it. Inside nsl, one reaction runs at a time, to completion.

![The reactor's event flow](img/e-reactor-flow.svg)

_Figure 5. From interrupt to reaction. A timer interrupt posts to a mailbox and returns; the reactor, blocked on the
mailbox when idle, wakes, queues the reactions that care about the written path, and runs them one by one. The
supervisor catches a failed reaction; the liveness observer judges progress from outside._

**The cost is the one every event loop has.** A long computation starves the timers, because nothing can interrupt a
reaction; the answer is to split it across reactions. The guard is the liveness observer, a C thread that judges
progress against the shortest timer period the program has declared. A wedged node is dumped, rebooted, and leaves a
note saying why [Liveness].

What the cost buys:

- **No locks in nsl code.** A reaction cannot be interrupted by another, so shared state needs no protection. Races
  are impossible in the nsl layer and remain possible in the C beneath it.
- **One stack**, where threads cost one each; the July 2026 rewrite that removed fibers removed theirs [Pivot].
- **A failed reaction is caught, not fatal.** The supervisor keeps the heap and reactor running and restarts or
  quarantines the job [MD].
- **A wedge leaves evidence:** the observer's dump and note.

**Always-on and deep sleep.** A node's power mode is a value at a path, written by the gateway and acknowledged by
the node, not a command [Porta]. An always-on node (the mains profile) runs a loop that never returns. A deep-sleep
node wakes, reads, exchanges with the gateway and sleeps; `platform deepSleep:` never returns and the next wake is a
full reboot, so the program is declared run-once. Deep sleep runs on the MagTag today.

Every exchange is started by the node. A sleeping node has no address to be called on, so the gateway holds what it
wants, a new setting or a new program, until the node next wakes and asks. §9 depends on this.

## 7. Develop without reflashing

The firmware is flashed once. The program is a read-only image in its own flash partition, run in place, and the
firmware reads the partition at boot [Image]. The first two steps need no board.

**Build and run on the host.** The compiler turns a `.nsl` file into an image, and custos runs it once on the desktop VM. The body is the source; the answer is the output and exit status, or the
compiler's own `file:line:col` message if it does not compile.

```
curl -s --data-binary @average.nsl \
     'localhost:6971/run?for=dev&name=average.nsl' | jq
```

The answer, captured on the host VM on 2026-10-08 (the hash is shortened here):

```
{
  "Board":    "dev",
  "Image":    "average.oimg",
  "SHA256":   "d4200f2c…71892",
  "Output":   ["oevm: build-id host",
               "oevm: program sha256 d4200f2c…71892",
               "mean 4"],
  "Exit":     0,
  "TimedOut": false
}
```

**Build, then load onto the device.** `POST /build` takes the same body and answers the image's name, hash and size; the agent fetches the image and posts
it to `POST /load`. Build and load stay two verbs [Peer].

```
curl -s --data-binary @average.nsl \
     'localhost:6971/build?for=c6spare&name=average.nsl' | jq
curl -s 'localhost:6971/build/last.oimg' -o average.oimg
curl -s --data-binary @average.oimg \
     'localhost:6971/board/c6spare/load?name=average.oimg' | jq
```

The load answer, captured against the host VM (on a board, `BuildID` is the firmware's and `Offset` the
partition's):

```
{
  "LoadedMs": 1791474679885,
  "Result": { "Image": "average.oimg", "Size": 229160, "SHA256": "d4200f2c…71892",
              "BuildID": "host", "Offset": 0 },
  "Debug": ""
}
```

A load succeeds only when the node then boots the same firmware and reports the same program hash on its console.
That proves the program loaded, not that it runs correctly. On a board, the load closes the console cable, writes the partition with the board's flasher, resets, reads the two boot lines, and
reopens the cable. Only the ESP32-C6 has a loader today.

**What is not measured.** The loop has not been timed from an edit to a program running on a board, and this paper
claims no number. It is shorter than a firmware reflash because no firmware is rebuilt or written.

## 8. Debug, patch and profile over USB

A small device usually means `printf` and a reflash. These tools reach into the running VM over the board's one USB
cable, through custos, which holds it. Each is an HTTP call that answers JSON, with a web page and a
terminal front over the same calls [Peer]; HTTP is the only surface the tutorials advertise.

| Tool | What it does today | Record |
|---|---|---|
| **load** | replaces the program without reflashing; the node reports the new hash (ESP32-C6) | [Image] |
| **debug** | breakpoints by source line; `step`, `next`, `finish`, `pause`; the stack and named local variables at a stop | [Debugger] |
| **patch** | in a debug build, replaces one method of a program stopped at a breakpoint, restarts the frame, and continues into the new code; shown on an ESP32-C6 on 2026-10-06 | [Patch] |
| **profile** | per-method call counts from the standard image on the ESP32-C6, so a field node is profiled without a reflash; it costs 1.3 % on the worst-case benchmark | [Profile] |
| **log** | the node's log and console, collected by custos | [Console] |

Every one of these names source, not addresses: the image map beside each image gives every method's file and line
[Image].

**The costs first.** The debugger needs a separate image: the checks that make breakpoints possible cost 11.6 % per
bytecode and about 500 B of flash, so the standard image leaves them out [Debugger]. On a board, every bytecode step
is a round trip over the cable, about 0.2 s on the C6, and a line step is many of them: the first `next` in the
tutorial's transcript took 3.6 s. A long loop is better crossed with a breakpoint after it. A
breakpoint does not fire during `next` or `finish`, because the Supervisor decides each stop by frame depth alone.
Hot patching is limited to methods whose new version uses only constants the image already holds [Patch].

**A stop, as data.** A stop is waited for by long-polling `GET /debug/state?after=N`, never by sending a command, because any command that reaches
a running program stops it [Debugger]. The executed example, on the tutorial's `Average` program on the host VM:

```
curl -s -d 'break 16' "$base/debug/do"
curl -s -d continue   "$base/debug/do"
curl -s "$base/debug/state?after=$seq"      # long-poll, 25 s at most
curl -s -d step       "$base/debug/do"
curl -s -d bt         "$base/debug/do"
curl -s -d locals     "$base/debug/do"
```

The `State` has the fields `Seq`, `Image`, `Run`, `Stop`, `Frames`, `Locals`, `Breaks` and `Transcript`. After
`step` into `sum:`, captured on the host VM on 2026-10-08 and cut to the stop, stack and locals:

```
"Run":  "stopped",
"Stop": { "Reason": "step", "File": "average.nsl", "Line": 8,
          "Method": "Average>>sum:", "Off": 7968, "PC": 1 },
"Frames": [
  { "Where": "at average.nsl:8  Average>>sum:", … },
  { "Where": "at average.nsl:16  Average>>main:args:", … },
  { "Where": "at supervisor.nsl:41  Supervisor>>run:platform:args:debug:", … } ],
"Locals": { "Where": "at average.nsl:8  Average>>sum:",
            "Vars": [ { "Name": "self",  "Value": "an Object" },
                      { "Name": "xs",    "Value": "an Object" },
                      { "Name": "total", "Value": "nil" } ] },
"Breaks": [ "average.nsl:16" ]
```

The same calls drive a board; only the base URL changes.

**A patch.** The body is the edited `.nsl`, with the method named in the query [Peer]:

```
curl -s --data-binary @average-edited.nsl \
     'localhost:6971/board/c6spare/debug/patch?method=Average>>sum:' | jq
```

**A profile.** The request names a window in milliseconds; the answer ranks methods.

```
curl -s -X POST 'localhost:6971/profile/capture?ms=200' | jq '.Report.Rows[:5]'
```

Each row has `Name`, `Count`, `Pct` and `Unknown`. The program must dispatch `sys/timer` and outlast the window; a program that ends before its
window closes answers 502 at once [Debugger].

The patch row is one board, one method and one session: a demonstration, not a survey of patches.

## 9. Remote: through the gateway, to a node that is asleep

**The debugger through the gateway is built and has been run on two boards** with no cable on either: the MagTag,
which deep-sleeps, on 2026-10-09, and the wall-powered ShipsBell C6, which runs continuously, on 2026-10-10
(#375–#377, #379, #380) [Field]. It was designed on a host spike that runs a real porta and a host VM on the desktop
and prints `SLEEPY DEBUG OK` [Spike]. Program over-the-air load (#233) was proven on a bench C6 over Thread on 2026-10-10
and on both demo nodes on 2026-10-12 (below) [Field].

![Remote debug sequence](img/e-remote-debug-seq.svg)

_Figure 6. A breakpoint set at the gateway is held by porta as intent and delivered when the sleeping node next
asks; the node reports the stop and stays awake while the developer steps._

**The mechanism (peer design D9, D19).** A breakpoint is intent, so it is written under the node's `goal/` paths, and
porta holds it until the node next wakes and asks. On a hit, the node reports the stop and stays awake, one exchange per command. The
liveness observer already knows not to reset a node held at a breakpoint over USB [Debugger], and the VM holds
the same state whichever link serves the command loop (#377). The
`Debug` record in the manifest carries the breakpoints and the program's hash, because a breakpoint is an offset
in one image, and an `obs/debug` cell reports a held node (#375). The whole set is one cell: the empty set is
the detach.

**Two kinds of sleep** need different machinery; the spike exercised both on the host.

| | VM stays alive (long check-in cadence; Thread sleepy end device) | VM dies (deep sleep: every wake is a cold boot) |
|---|---|---|
| Where the breakpoint lives | the VM's breakpoint table, set by lines on `dbg/req` | the `goal/debug/main` cell: porta replays cells to a fresh boot, and replays no `dbg/req` row |
| When it is applied | at the next check-in | inside the first check-in of every wake, before the program's work |
| Proven by | gate leg A | gate leg B |

**The spike's measurements** (host, loopback, 2026-10-07) [Spike]:

| Measure | Value |
|---|---|
| Breakpoint written to stop reported in porta's store | 9.9 s, one 10 s cadence; the breakpoint waits at porta for the next check-in |
| One debug command while held | 3–4 ms |
| Held exchanges | about 1,600 per second (63,666 in a 39 s hold, manual run) |
| Node CPU while held | 99.9 %: the debugger polls and does not sleep |
| porta's store during a 39 s hold | its write-ahead log grew about 4 MB, since each pull is a write |
| After `dbg:continue` from that hold | three ticks fired back to back, the cadence timer catching up |

The spike also falsifies its own gate: breaking the `dbg/req` intercept turns leg A red, breaking the
`goal/debug/main` intercept turns leg B red, and a control leg sending the same rows to a standard image gets no
answer.

**On the boards** (the two bells of Appendices A and B, each on its shipped debug image, the breakpoint written at
the gateway as the `goal/debug/main` cell) [Field]:

| Measure | MagTag, WiFi, deep sleep (2026-10-09) | wall C6, Thread, always on (2026-10-10) |
|---|---|---|
| Cell written to stop reported | the first check-in of the next wake: `work:` stopped 1 s after boot | 56 s: the cell rode one 30 s check-in, the break hit at the next |
| `backtrace` / `inspect` / `step`, gateway to node and back | 755 / 636 / 754 ms | 239 / 233 / 230 ms |
| Held | 26 s; 186 collections, live 23 KB of 28 | 1.2 s |
| Release | `continue`; the next boot pulled the empty set and ran free | the empty set: cleared in 479 ms, the next check-in missed nothing |

A command's round trip is the node's held-exchange cadence, not the radio: about a quarter of a second on the C6
and three quarters on the MagTag, against 3–4 ms on the loopback. A breakpoint is placed by what runs: the wall
bell's `onTick` runs only at the alarm, so a break there would have held the bell at the alarm and delayed the
ring, and the break went on `checkIn`, which runs every 30 s.

**The costs.** Holding a node costs a busy CPU, which a battery node cannot afford; the MagTag's arena ran 186
collections in its 26 s hold. The
spike's own proposal, a long-poll at porta, was not accepted, and what an abandoned session should do is open (#378).
The store growth of about 4 MB in 39 s on the host is a cost on the gateway, at a rate the boards' held cadence
divides by several hundred. The `obs/debug` cell reports the hold, so a monitor with no debugger attached sees it,
and the debug image no longer waits at boot for a cable (#376).

**Program over the air (#233).** The firmware holds two program slots and runs one in place; a push is a signed
record in the goal tree naming an image porta serves in 256-byte blocks; the node fetches it into the other slot on
its own thread while it keeps checking in, reboots once into it, and the firmware confirms the new image at its first
exchange or resets after 150 s and reverts to the previous slot. On a bench C6 over Thread on 2026-10-10 [Field]: a 167 KB
image fetched at 3.3 KB/s, 64 s from push to confirmed; an image that never checked in was reverted after exactly 150 s;
a flipped signature byte was refused before the image ran; a reset half-way through the fetch cost one full refetch
(the fetch cursor is not yet kept across a reboot) and still converged. A rejected or reverted image is refetched at the
next check-in, about a minute of radio per retry, until the goal changes (#175, #426).

**On the demo nodes** (2026-10-12; both reflashed by cable from one source first, then pushed through porta with no
cable) [Field]:

| Measure | MagTag, WiFi, deep sleep | wall C6, Thread, always on |
|---|---|---|
| Image | 177 KB | 131 KB |
| Stage | 9 s, inside the awake window, in the check-in itself | 30 s (about 4.4 KB/s) on its fetch thread, checking in every 30 s throughout |
| Goal written to confirmed | not timed: the goal waits for the next wake, whose check-ins stage, reboot and confirm | 3 min |
| Next bell | 18:30, on time | 18:30, on time |

The MagTag stages in the check-in rather than on a thread of its own: its RAM cannot spare the thread's stack, and a
node awake only to check in has nothing else to do. Its trial boot is not a timer wake, so it does not strike, and
it sets its clock at once without scoring the gap as drift: before the catch-up was added, a stale goal made it
stage and fail two trials after a cable flash, the firmware held each for 150 s, and it rang 5 min 46 s late, which
an ordinary sync would have read as about 87,000 ppm of drift. After a push the breakpoint cell still names the old
image, and the new one refuses it until it is rewritten, since a breakpoint is an offset in one image. On the bench
the push was driven from porta's command line. An agent drives it through custos, the same `POST /load` it uses on a
cable (Appendix D), proven on host VMs against a real gateway (#398) and not yet on either demo node.

## 10. Trusting code an agent wrote

The problem is verification, and an agent verifies by running things and reading what comes back. Two questions
follow: what bounds what an agent's code can do, and what lets it check its own work? Whether the language suits an
agent is the companion paper's argument [Lang].

**What bounds the code.**

- **Authority is visible in the header.** A module that can use the network says so in its signature, and a channel
  capability can only be narrowed.
- **The primitive set is closed.** There are 160 primitives, and an application cannot add one.
- **There is no FFI.** An agent cannot wander into C to get around the first two.

The companion paper owns these [Lang]; they bound the effects of an error, not its likelihood.

**Not yet true.** `platform` hands out every device today (ledger row 32 [Ledger]), so a program that receives it can
reach all of them and the header's visibility is weaker than the design.

**What lets the agent check its work.** Every custos verb has a machine-readable HTTP path before any human front
(D11, D12), gated against a real VM the way an agent drives them [Peer]. Every crash and profile sample names a
source line.

![The agent loop](img/e-agent-loop.svg)

_Figure 7. The loop an agent runs: write a program, run it on the host VM, load it onto a board, probe it with a
breakpoint or a profile, and read the answer as data. Each step answers JSON and names a line. How many errors the host run
catches before a board is touched is unmeasured._

An error from the compiler is a `file:line:col` message. A message not understood names the
receiver's class and the nearest selector it does understand [Lang].

**What this does not show.** Nobody has measured anything about agents on this system, including whether an agent
writes more correct nsl than C. The expectation is that a short, repeatable loop makes each check cheap; the 33 % in §1 spend their time on
checks that are not cheap. The test that would settle it gives the same small board tasks to an agent in nsl,
with the language in its context, and in Zephyr C, scored on first-run correctness. It has not been run (rete issue
#373); the result will be published either way.

One more caution. Verification by running catches only what running reveals. A reaction that is wrong only under a
timing or radio condition the host VM does not reproduce passes on the host and fails on the board; the host suite is
blind to the interrupt seam, and nothing in this loop closes that gap.


## 11. Costs compared with Zephyr C

| Cost | Compared with Zephyr C | Would lift it |
|---|---|---|
| Speed per byte | about 12 µs per method activation: 11.83 µs on the ESP32-C6 and 11.13 µs on the RP2350 [RP2350]; about a hundred times slower than C on the one method measured both ways, and 116–119× on the Towers of Hanoi spike, a best case with all the work in one method [Hanoi]; no JIT, because a 256 KB part has no room for a code cache | native substitution of a hot method (§2, §3); or leave it in C; or compile the nsl method ahead of time with its sends kept, which on the encoder returned 30× of the C encoder's 280×, each kept send costing an activation and each rooted object 3 µs (§3) [CborAot] |
| RAM given to the arena | 112 KB of 256 KB on the nRF52840, with 4 KB spare at link [Boards] | a larger part; the C6 has 128 KB of arena by choice, and WiFi sets the classic ESP32 to 48 KB |
| No compile-time types | a misspelt selector is found when it runs, not compiled | the error line names the nearest selector, which softens the cost [Lang] |
| A language in no model's training data | an agent writes nsl from context, not habit | the test in §10 (#373); not yet run |
| A dialect to learn | a C programmer learns a Smalltalk dialect; the 1989 record put it at two to four months for experienced C programmers, with the training period itself the discouraging part [Bar89] | unmeasured for nsl; the surface is one kernel and 160 primitives rather than a Smalltalk image [Lang] |
| One kernel | no virtual classes, no per-target library, `platform` hands out everything [Ledger] | narrowed platforms (row 32) |
| Tooling at version 0.1 | the compiler is a Python tool, and the loader exists for one board, the ESP32-C6 | more loaders; each board needs its own region and flasher |
| Debug images are separate | 11.6 % per bytecode and about 500 B of flash [Debugger] | not planned; the standard image stays lean |
| Remote debug is slow per command | 230–750 ms per command through the gateway, the node's held cadence, against 3–4 ms on the host [Field] | a shorter held cadence, paid in the held node's radio and CPU |
| A single maintainer | one maintainer for the VM, kernel and tools | not planned |

### Against MicroPython

MicroPython has a decade of use, a large library of drivers written in Python, a REPL over the serial cable, deep
sleep on the ESP32 family, and program load by copying files; and it is in every model's training data. Per call it is
likely faster than nsl, and that is unmeasured.

What rete has that MicroPython's core does not: a gateway that holds intent for a sleeping node behind a
manifest-checked wire; a source-line debugger and a per-method profiler over the cable; a liveness observer that dumps a
wedged node and says why; and a closed primitive set with no ambient device module.

The July review decks already placed nsl beside Toit, MicroPython on Zephyr and AtomVM, and bet that confinement,
supervision, remote serviceability and Zephyr's driver catalogue decide whether a scripting layer survives production,
not types or benchmarks [Script]. The comparison that answers the question is rete #383: the same C6 node written in
MicroPython, scored on measured rows; several rows MicroPython wins, which is the point of running it [MP].

### Where this stands

Today rete is a lifecycle system whose desk half (host VM, USB load, debug, patch, profile) is built and measured, and
whose field half is built for debug and for program over the air (the two bells, through the gateway, over WiFi and
over Thread, §9). Version 0.1 ships when the
field half is built (decided 2026-10-09), so nothing in the release is labelled "designed". Not yet shown: the profile
through the gateway, an agent's push to a demo node (built and gated on host VMs, #398), or an agent writing C against
an agent writing nsl.

Version 0.1 is defined as one node, every verb, through the gateway, over the radio, while the node sleeps. By
lifecycle stage, against the C toolchain the reader already has:

| Stage | The C toolchain | rete today | Version 0.1 |
|---|---|---|---|
| write | edit, compile, link | the same program runs first on the host VM; authority in the header; 160 closed primitives | the same |
| deploy | flash the firmware | load an image beside the firmware over the cable (ESP32-C6 only); a manifest-checked wire | over the cable, or through the gateway: built on both bells (#233); the agent's push gated on host VMs (#398) |
| debug | `printf`; a probe and gdb | source-line breakpoints, the stack and named locals, patch and restart, over one USB cable; and through the gateway, on a node that sleeps (§9) | the same |
| observe | nothing | a per-method profile on the shipped image (1.3 %); heap statistics every check-in; a liveness observer that dumps and leaves a note | the profile through the gateway as well |
| manage | a site visit | the gateway holds intent for a node that is asleep and judges its liveness from outside | and keeps each commissioned image with its symbol map |

The cable is the desk. The last column is what 0.1 is.

<div style="page-break-before: always"></div>

## Appendix A. The ship's-bell clock as a deep-sleep node

One program, two power modes. This is the deep-sleep mode: the MagTag (ESP32-S2, WiFi) sleeps between half-hours,
and every wake is a cold boot that strikes, syncs at a watch end, exchanges once with the gateway, and sleeps again.
The always-on mode is Appendix B; the classes both name are Appendix C. The program was built on 2026-10-09 as
the image a debugger and a profiler can reach (the debugger's hooks and the method profiler compiled in; 187,768
bytes of image), checked in with the gateway, and rang six bells at 19:00 [Example].

The node's header names the platform and the network, and `main:` is the map of one wake: it names the
collaborators and the order, and nothing else. The clock's rules are in the library it names with `uses:`; the
counts came from the kernel's `Bell`, not from here. The listings in this copy carry no comments; the
source files keep them in place, and the full copy of this paper lifts them into numbered references after each file.

**`nodus/oevm/zephyr/MagTagDeepSleepNode.nsl`**

```
class MagTagDeepSleepNode usingPlatform: platform net: net = (
  | dial record drift clock space bridge summary |
) (
  gwAddr    = ( ^ 'fd3f:2e87:5f7:5554:6662:66ff:fe2f:710' )
  gwPort    = ( ^ 6969 )
  localPort = ( ^ 16976 )
  chip      = ( ^ #esp32s2 )

  public main: platform2 args: args = (
    dial::   Dial new.
    record:: RtcRecord on: platform.
    drift::  Drift on: record.
    clock::  BellClock on: record drift: drift ntp: platform dial: dial.
    space::  platform space.

    record valid
      ifTrue:  [ self strikeWake ]
      ifFalse: [ self coldBoot ].

    self joinPorta.
    self report.
    space drain.
    bridge checkIn.
    ^ self sleep
  )

  strikeWake = (
    | half bells |
    half::  clock nextHalf.
    bells:: (platform bell) bellsForHalfHour: half.
    ('wake half ' , half printString , ' bells ' , bells printString) out.
    (platform bell) strike: bells.
    'struck' out.
    record lastHalf: half.
    record bootTicks: record bootTicks + 1.
    summary:: 'half ' , half printString , ' bells ' , bells printString.
    bells = 8 ifTrue: [ clock trySync. self paint ].
    ^ self )

  coldBoot = (
    'cold boot: free-run from 00:00' out.
    record coldInitCal: drift seedPpm stale: clock neverSyncedSec.
    clock trySync.
    self paint.
    summary:: 'cold boot'.
    ^ self )

  paint = (
    | disp |
    disp:: [ | d | d:: platform display. d clear. d ]
      on: [ :e | e tag = #displayUnavailable ]
      do: [ :e | nil ].
    disp == nil ifTrue: [ 'no display; skipping paint' out. ^ self ].
    (StatsPage on: disp record: record clock: clock heap: platform heapStats) paint.
    ^ self )

  joinPorta = (
    | sock |
    sock:: net udpBindPort: self localPort.
    sock == nil ifTrue: [ 'no net: running unbridged' out ].
    bridge:: platform PortaBridge
      socket: sock space: space id: platform nodeId peer: self gwAddr port: self gwPort
      spans: platform Spans new.
    bridge subscribeStream: 'tel/log'.
    bridge liveness: self cadenceMillis misses: 4.
    bridge publishBuild: self chip.
    bridge checkIn.
    ^ self )

  report = (
    space out: 'tel/log' value:
      (platform Record tag: #Log with: #lvl -> #info
         with: #txt -> (summary , ' up ' , record bootTicks printString
                        , ' held ' , bridge heldExchanges printString)).
    ^ self )

  sleep = (
    | target |
    target:: clock sleepTarget.
    platform deepSleep: (self witnessSleepFor: (drift disciplined: target)).
    ^ self )

  witnessSleepFor: realSec = ( ^ realSec )

  cadenceMillis = ( ^ (dial slotSec + 60) * 1000 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

<div style="page-break-before: always"></div>

## Appendix B. The ship's bell as an always-on node

The other power mode: the wall-powered ESP32-C6 with a DS3231 and a piezo, on Thread, always on. Its clock is a
part, so it has no predicted clock and no calibration; its wake is the RTC's alarm, so it has no sleep; its strike
is a pattern on a GPIO, so it has a striker. What it shares with Appendix A is the `Dial` — the 48 slots of the
day, the modular ratchet, the boundary arithmetic — and the rule that a correction never rings: a sync advances the
deep-sleep node's count silently in `BellClock>>apply:`, and re-seeds this node's marker silently in `resync`, so
the only path that rings on either node is a boundary arriving on a count a tick set. Unifying the ratchet removed
this node's own midnight case (`slot = 0`, "however it was reached"), which rang eight bells on a clock corrected
back across midnight, and its `slot + 1`, which read 48 at 23:59:55 and logged every midnight countdown as a
watchdog's late ring.

Measured against the linear node it replaces, compiled for the same board: 199,304 bytes of image against
178,784 (+20,520, +11.5 %), 972 methods against 861. Most of the difference is not this node's: the `uses:` pragma
splices the whole library, so the four deep-sleep classes (`RtcRecord`, `Drift`, `BellClock`, `StatsPage`, 73
methods) ride in the wall image unused. The program partition is 512 KB, so the cost is paid in flash the board
has; a per-file split of the library is the remedy when the number matters, and is filed (rete#394). The full
image, with the debugger's hooks and the profiler, links at 487,768 bytes of flash and 73 % of SRAM. It was
flashed to the bell on 2026-10-10 at 07:56 (a daylight act: the piezo sounds for the whole flash), booted with
Thread, NTP, the RTC and the alarm armed within a second, struck eight bells at 08:00:00 and reported the strike
to the gateway on its next check-in; at 08:04 it was stopped in `checkIn` from the gateway, stepped, and released
(§9) [Field].

**`nodus/oevm/zephyr/ShipsBellNodeC6.nsl`**

```
class ShipsBellNodeC6 usingPlatform: platform net: net = (
  | dial rtc trigger striker space timers tick bridge
    lastSlot. haveClock ::= false. midnightPending ::= false. checkIns ::= 0. |
) (
  gwAddr    = ( ^ 'fdc5:9109:9afe:97ea:9541:d963:f8d9:8567' )
  gwPort    = ( ^ 6969 )
  localPort = ( ^ 16971 )
  cadence   = ( ^ 30000 )
  chip      = ( ^ #esp32c6 )
  strikePin = ( ^ 18 )

  public main: platform2 args: args = (
    | bellCh |
    dial::    Dial new.
    rtc::     RtcClock on: platform dial: dial.
    space::   platform space.
    timers::  platform timers.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    tick::    space channel: 'clock/tick'.
    tick whenever: '' do: [ :m | self onTick ].
    bellCh::  space channel: 'bell/stroke'.
    bellCh whenever: '' do: [ :m | striker step ifFalse: [ self afterPattern ] ].
    striker:: Striker on: platform gpio pin: self strikePin timers: timers channel: bellCh.
    trigger:: Trigger on: platform rtcAlarm timers: timers tick: tick dial: dial.
    self joinPorta.
    self ready.
    self resync.
    [ true ] whileTrue: [ platform pumpEvents. space drain ]
  )

  onTick = (
    | secs slot |
    secs:: rtc secondsOfDay.
    secs == nil ifTrue: [ ^ self noClock ].
    haveClock ifFalse: [
      haveClock:: true.
      ('clock ' , (dial hms: secs) , ' — readings good') out.
      ^ self seedAt: secs ].
    slot:: dial slotOf: secs.
    (dial isBehind: slot of: lastSlot) ifTrue: [
      (trigger armedFor == slot) ifFalse: [
        ('watchdog: no alarm for slot ' , slot printString , ' — ringing late') out ].
      lastSlot:: slot.
      ^ self ringSlot: slot at: secs ].
    (trigger nearBoundary: secs) ifTrue: [ trigger countdownAt: secs ].
    ^ self )

  ringSlot: slot at: secs = (
    | n |
    n:: (platform bell) bellsForHalfHour: slot.
    ('bell slot ' , slot printString , ' at ' , (dial hms: secs) , ' — ' ,
      n printString , (self bellWord: n)) out.
    self report: ('slot ' , slot printString , ' bells ' , n printString).
    slot = 0 ifTrue: [ midnightPending:: true ].
    striker ring: n.
    trigger armAfter: secs.
    ^ self )

  afterPattern = (
    midnightPending ifTrue: [ midnightPending:: false. self resync ].
    ^ self )

  resync = (
    | secs |
    rtc sync.
    rtc show.
    secs:: rtc secondsOfDay.
    secs == nil ifTrue: [ ^ self noClock ].
    haveClock:: true.
    ^ self seedAt: secs )

  seedAt: secs = (
    lastSlot:: dial slotOf: secs.
    ('seed slot ' , lastSlot printString , ' at ' , (dial hms: secs) , ' — nothing rung') out.
    trigger armAfter: secs.
    ^ self )

  noClock = (
    haveClock ifTrue: [
      haveClock:: false.
      'clock nil — no DS3231 reading; not striking until it returns' out ].
    trigger retry.
    ^ self )

  joinPorta = (
    | sock beatCh |
    sock:: net udpBindPort: self localPort.
    sock == nil ifTrue: [ 'porta: bind FAILED — ringing on, ticking on, reporting nothing' out ].
    bridge:: platform PortaBridge
      socket: sock space: space id: platform nodeId peer: self gwAddr port: self gwPort
      spans: platform Spans new.
    bridge subscribeStream: 'tel/log'.
    bridge liveness: self cadence misses: 8.
    bridge publishBuild: self chip.
    beatCh:: space channel: 'sys/tick'.
    beatCh whenever: '' do: [ :m | self checkIn ].
    bridge startTicking: timers on: beatCh.
    ('porta: check-in every ' , (self cadence // 1000) printString , ' s as ' , platform nodeId) out.
    ^ self )

  checkIn = (
    striker striking ifTrue: [ ^ self ].
    [ bridge checkIn. checkIns:: checkIns + 1 ]
      on: [ :e | true ]
      do: [ :e | 'porta: check-in raised — ringing on' out ].
    ('porta: check-in ' , checkIns printString ,
      ' misses ' , bridge misses printString ,
      ' cursor ' , bridge cursor printString ,
      ' acked ' , bridge acked printString) out.
    ^ self )

  report: txt = (
    space out: 'tel/log' value:
      (platform Record tag: #Log with: #lvl -> #info with: #txt -> txt).
    ^ self )

  ready = (
    ('shipsbell ready, slot ' , dial slotSec printString ,
      ' s, lead ' , trigger leadSeconds printString ,
      ' s, watchdog +' , (trigger graceMillis // 1000) printString , ' s, ' ,
      striker widthMillis printString , '/' , striker pairGapMillis printString , '/' ,
      striker setGapMillis printString , ' ms') out.
    ^ self )

  bellWord: n = ( ^ n = 1 ifTrue: [ ' bell' ] ifFalse: [ ' bells' ] )
) : (
)
```

<div style="page-break-before: always"></div>

## Appendix C. The library both nodes name

`lib/Bells.nsl`, reached by a `uses:` line in each node's header and spliced into it as nested classes: nothing
joins the kernel and nothing is routed through `platform` (an application library, not a kernel library). Eight
classes, one responsibility each; each header names what the class is given, and that is the whole of what it can
reach. `Dial` is shared; `RtcRecord`, `Drift`, `BellClock` and `StatsPage` are the deep-sleep clock's; `RtcClock`,
`Trigger` and `Striker` are the always-on bell's. A backtrace or a profile row reads `Drift>>record:` or
`Striker>>strikeOn`, not `Node>>recordDrift:` — the observe row of the lifecycle table describes itself.

**`nodus/oevm/zephyr/lib/Bells.nsl`**

```
class Bells = () (

  class Dial = () (

    public slotSec     = ( ^ 1800 )
    public slotsPerDay = ( ^ 86400 // self slotSec )

    public slotOf: secs = ( ^ secs // self slotSec )

    public nextAfter: half = (
      | n |
      n:: half + 1.
      n = self slotsPerDay ifTrue: [ n:: 0 ].
      ^ n )

    public isBehind: trueHalf of: lastHalf = (
      | n fwd |
      n:: self slotsPerDay.
      fwd:: trueHalf - lastHalf.
      fwd < 0 ifTrue: [ fwd:: fwd + n ].
      fwd = 0 ifTrue: [ ^ false ].
      ^ fwd < (n - self holdAheadSlots) )

    public secondsToBoundaryAfter: lastHalf from: predicted = (
      | d |
      d:: ((lastHalf + 1) * self slotSec) - predicted.
      d < 0 ifTrue: [ d:: d + 86400 ].
      d:: d \\ 86400.
      d < 1 ifTrue: [ d:: 1 ].
      ^ d )

    public wrapDrift: d = (
      d > 43200 ifTrue: [ ^ d - 86400 ].
      d < -43200 ifTrue: [ ^ d + 86400 ].
      ^ d )

    public wrapDay: secs = (
      secs < 0     ifTrue: [ ^ secs + 86400 ].
      secs > 86399 ifTrue: [ ^ secs - 86400 ].
      ^ secs )

    holdAheadSlots = ( ^ 3 )

    public tzOffset = ( ^ -25200 )

    public hms: secs = (
      | h rem m s |
      h::   secs // 3600.
      rem:: secs \\ 3600.
      m::   rem // 60.
      s::   rem \\ 60.
      ^ (self pad: h) , ':' , (self pad: m) , ':' , (self pad: s) )

    pad: n = ( ^ n < 10 ifTrue: [ '0' , n printString ] ifFalse: [ n printString ] )
  ) : (
  )

  class RtcRecord = ( | plat | ) (
    public valid = ( ^ plat rtcValid )

    public coldInitCal: seedPpm stale: neverSyncedSec = (
      plat rtcPut: self iLastHalf     value: 0.
      plat rtcPut: self iBootTicks    value: 0.
      plat rtcPut: self iAnchorSec    value: 0.
      plat rtcPut: self iElapsedSec   value: 0.
      plat rtcPut: self iCalPpm       value: seedPpm.
      plat rtcPut: self iStaleSec     value: neverSyncedSec.
      plat rtcPut: self iDrift24Max   value: 0.
      plat rtcPut: self iDrift24Min   value: 0.
      plat rtcPut: self iSyncsWin     value: 0.
      plat rtcPut: self iMagic        value: self magicWord.
      ^ self )

    public lastHalf   = ( ^ plat rtcGet: self iLastHalf )
    public bootTicks  = ( ^ plat rtcGet: self iBootTicks )
    public anchorSec  = ( ^ plat rtcGet: self iAnchorSec )
    public elapsedSec = ( ^ plat rtcGet: self iElapsedSec )
    public calPpm     = ( ^ plat rtcGet: self iCalPpm )
    public staleSec   = ( ^ plat rtcGet: self iStaleSec )
    public drift24Max = ( ^ plat rtcGet: self iDrift24Max )
    public drift24Min = ( ^ plat rtcGet: self iDrift24Min )
    public syncsWin   = ( ^ plat rtcGet: self iSyncsWin )

    public lastHalf: v   = ( plat rtcPut: self iLastHalf   value: v. ^ self )
    public bootTicks: v  = ( plat rtcPut: self iBootTicks  value: v. ^ self )
    public anchorSec: v  = ( plat rtcPut: self iAnchorSec  value: v. ^ self )
    public elapsedSec: v = ( plat rtcPut: self iElapsedSec value: v. ^ self )
    public calPpm: v     = ( plat rtcPut: self iCalPpm     value: v. ^ self )
    public staleSec: v   = ( plat rtcPut: self iStaleSec   value: v. ^ self )
    public drift24Max: v = ( plat rtcPut: self iDrift24Max value: v. ^ self )
    public drift24Min: v = ( plat rtcPut: self iDrift24Min value: v. ^ self )
    public syncsWin: v   = ( plat rtcPut: self iSyncsWin   value: v. ^ self )

    iMagic      = ( ^ 0 )
    iLastHalf   = ( ^ 1 )
    iBootTicks  = ( ^ 2 )
    iAnchorSec  = ( ^ 3 )
    iElapsedSec = ( ^ 4 )
    iCalPpm     = ( ^ 5 )
    iStaleSec   = ( ^ 6 )
    iDrift24Max = ( ^ 7 )
    iDrift24Min = ( ^ 8 )
    iSyncsWin   = ( ^ 9 )
    magicWord   = ( ^ 348000704 )

    setPlat: p = ( plat:: p. ^ self )
  ) : (
    public on: platform = ( ^ self new setPlat: platform )
  )

  class Drift = ( | record | ) (

    public disciplined: target = (
      | p mag corr |
      p:: record calPpm.
      p >  50000 ifTrue: [ p::  50000 ].
      p < -50000 ifTrue: [ p:: -50000 ].
      mag:: p.
      p < 0 ifTrue: [ mag:: 0 - p ].
      target < 20000
         ifTrue:  [ corr:: ((target * mag) + 500000) // 1000000 ]
         ifFalse: [ corr:: (((target // 8) * mag) + 62500) // 125000 ].
      p < 0 ifTrue: [ ^ target + corr ].
      target - corr < 1 ifTrue: [ ^ 1 ].
      ^ target - corr )

    public ppmFor: driftSec over: elapsedSec = (
      elapsedSec = 0 ifTrue: [ ^ 0 ].
      ^ (driftSec * 1000000) // elapsedSec )

    public record: ppm = (
      | n mx mn |
      record calPpm: record calPpm + ppm.
      n:: record syncsWin + 1.
      n >= 6 ifTrue: [ n:: 0 ].
      n = 0 ifTrue: [ record drift24Max: ppm. record drift24Min: ppm ]
            ifFalse: [
              mx:: record drift24Max. mn:: record drift24Min.
              ppm > mx ifTrue: [ record drift24Max: ppm ].
              ppm < mn ifTrue: [ record drift24Min: ppm ] ].
      record syncsWin: n.
      ^ self )

    public seedPpm = ( ^ -10513 )

    setRecord: r = ( record:: r. ^ self )
  ) : (
    public on: record = ( ^ self new setRecord: record )
  )

  class BellClock = ( | record drift ntp dial basisMillis lastDrift syncOk | ) (

    public nextHalf = ( ^ dial nextAfter: record lastHalf )

    public slotSec = ( ^ dial slotSec )

    public predictedNow = ( ^ (record anchorSec + record elapsedSec) \\ 86400 )

    predictedAtSync = (
      | b |
      b:: basisMillis.
      b == nil ifTrue: [ b:: 0 ].
      ^ (self predictedNow + (b // 1000)) \\ 86400 )

    public awakeSinceBasis = (
      | b |
      b:: basisMillis.
      b == nil ifTrue: [ b:: 0 ].
      ^ ((0 millis) - b) // 1000 )

    public lastDrift = ( ^ lastDrift )
    public syncOk    = ( ^ syncOk )

    public trySync = (
      | secs |
      3 timesRepeat: [
        secs:: ntp ntpSyncTz: dial tzOffset.
        secs == nil ifFalse: [
          basisMillis:: 0 millis.
          syncOk:: true.
          self apply: secs.
          ^ true ].
        'ntp miss' out ].
      syncOk:: false.
      self markSyncFailed.
      ^ false )

    apply: trueSec = (
      | trueHalf lastHalf predicted d elapsed |
      trueHalf:: dial slotOf: trueSec.
      lastHalf:: record lastHalf.
      predicted:: self predictedAtSync.
      (self neverSynced or: [ dial isBehind: trueHalf of: lastHalf ])
         ifTrue:  [
                    ('sync true ' , trueSec printString , ' predicted ' , predicted printString , ' -> advance half ' , trueHalf printString) out.
                    record lastHalf: trueHalf ]
         ifFalse: [
                    d:: dial wrapDrift: (trueSec - predicted).
                    lastDrift:: d.
                    ('sync true ' , trueSec printString , ' predicted ' , predicted printString , ' -> hold half ' , lastHalf printString) out.
                    elapsed:: record elapsedSec.
                    elapsed < self driftFloorSec
                       ifTrue:  [ ('drift window ' , elapsed printString , ' s below floor ' , self driftFloorSec printString , ' s; ppm not recorded') out ]
                       ifFalse: [ drift record: (drift ppmFor: d over: elapsed) ] ].
      record anchorSec: trueSec.
      record elapsedSec: 0.
      record staleSec: 0.
      ^ self )

    neverSynced = ( ^ record staleSec >= self neverSyncedSec )
    public neverSyncedSec = ( ^ 999999 )

    markSyncFailed = (
      record staleSec: record staleSec + (dial slotSec * 8).
      'sync failed, stale grows' out.
      ^ self )

    public sleepTarget = (
      | awakeSec sleepSec |
      awakeSec:: self awakeSinceBasis.
      sleepSec:: (dial secondsToBoundaryAfter: record lastHalf from: self predictedNow) - awakeSec.
      sleepSec < 1 ifTrue: [ sleepSec:: 1 ].
      record elapsedSec: record elapsedSec + sleepSec + awakeSec.
      ('sleep: target ' , sleepSec printString , ' s cal ' , (self signed: record calPpm)
          , ' awake ' , awakeSec printString , ' s') out.
      ^ sleepSec )

    driftFloorSec = ( ^ dial slotSec * 2 )

    public signed: n = (
      n < 0 ifTrue: [ ^ '-' , (0 - n) printString ].
      ^ '+' , n printString )

    setRecord: r drift: d ntp: n dial: l = ( record:: r. drift:: d. ntp:: n. dial:: l. ^ self )
  ) : (
    public on: record drift: drift ntp: ntp dial: dial = (
      ^ self new setRecord: record drift: drift ntp: ntp dial: dial )
  )

  class StatsPage = ( | disp record clock heap | ) (

    public paint = (
      self row: 0 s: (self line: 'oevm' val: '').
      self row: 1 s: '----------'.
      self row: 2 s: (self line: 'heap' val: self heapField).
      self row: 3 s: (self line: 'ppm'  val: (clock signed: record calPpm)).
      self row: 4 s: (self line: 'up'   val: self upField).
      self row: 5 s: '----------'.
      self row: 6 s: (self line: 'ntp'  val: self ntpField).
      self row: 7 s: (self line: '24h'  val: (clock signed: record drift24Max)).
      self row: 8 s: (self line: ''     val: (clock signed: record drift24Min)).
      self row: 9 s: (self line: 'sync' val: (self staleField: record staleSec)).
      [ disp refresh ] on: [ :e | (e tag = #displayFault) or: [ e tag = #displayUnavailable ] ]
                       do: [ :e | ('refresh skipped: ' , e tag printString) out ].
      ^ self )

    row: r s: s = (
      [ disp at: r col: 0 put: s. ('| ' , s) out ]
        on: [ :e | (e tag = #displayBounds) or: [ e tag = #displayFault ] ]
        do: [ :e | ('row ' , r printString , ' skipped: ' , e tag printString) out ].
      ^ self )

    heapField = ( ^ (self kRound: heap liveBytes) , '/' , (heap heapSize // 1024) printString , 'K' )

    ntpField = (
      clock syncOk == nil ifTrue: [ ^ '--' ].
      clock syncOk ifFalse: [ ^ 'FAIL' ].
      clock lastDrift == nil ifTrue: [ ^ 'sync' ].
      ^ (clock signed: clock lastDrift) , '.0s' )

    upField = (
      | mins d r h m |
      mins:: record bootTicks * (clock slotSec // 60).
      d:: mins // 1440.
      r:: mins - (d * 1440).
      h:: r // 60.
      m:: r - (h * 60).
      ^ d printString , 'd' , (self pad: h) , ':' , (self pad: m) )

    staleField: sec = (
      | mins d r h |
      sec >= clock neverSyncedSec ifTrue: [ ^ '--' ].
      mins:: sec // 60.
      d:: mins // 1440.
      r:: mins - (d * 1440).
      h:: r // 60.
      ^ d printString , 'd/' , (self pad: h) , 'h' )

    line: label val: v = (
      | pad |
      pad:: 12 - label size - v size.
      pad < 0 ifTrue: [ pad:: 0 ].
      ^ label , (self spaces: pad) , v )

    spaces: n = ( | s | s:: ''. n timesRepeat: [ s:: s , ' ' ]. ^ s )
    kRound: bytes = ( ^ ((bytes + 512) // 1024) printString )
    pad: n = ( (n < 10) ifTrue: [ ^ '0' , n printString ]. ^ n printString )

    setDisp: d record: r clock: c heap: h = ( disp:: d. record:: r. clock:: c. heap:: h. ^ self )
  ) : (
    public on: display record: record clock: clock heap: heapStats = (
      ^ self new setDisp: display record: record clock: clock heap: heapStats )
  )

  class RtcClock = ( | plat dial | ) (

    public secondsOfDay = ( ^ plat wallClock )

    public sync = (
      | secs date rc |
      secs:: plat ntpSyncTz: dial tzOffset.
      secs == nil ifTrue: [ 'ntp miss — keeping the RTC' out. ^ false ].
      date:: plat ntpDate.
      date == nil ifTrue: [ 'ntp date miss — keeping the RTC' out. ^ false ].
      rc:: plat wallClockSetDate: date time: secs.
      rc == nil ifTrue: [ 'rtc write REFUSED — keeping the RTC' out. ^ false ].
      ('ntp ok ' , date printString , ' ' , (dial hms: secs)) out.
      ^ true )

    public show = (
      | secs |
      secs:: self secondsOfDay.
      secs == nil
        ifTrue:  [ 'rtc nil (no DS3231, or never set)' out ]
        ifFalse: [ ('rtc ' , (dial hms: secs) , '  slot ' , (dial slotOf: secs) printString) out ].
      ^ self )

    setPlat: p dial: d = ( plat:: p. dial:: d. ^ self )
  ) : (
    public on: platform dial: dial = ( ^ self new setPlat: platform dial: dial )
  )

  class Trigger = ( | alarm timers tick dial armedFor. alarmOk ::= false. | ) (
    public alarmOk  = ( ^ alarmOk )
    public armedFor = ( ^ armedFor )

    public armAfter: secs = (
      | toBoundary |
      armedFor:: nil.
      toBoundary:: dial secondsToBoundaryAfter: (dial slotOf: secs) from: secs.
      alarmOk:: alarm at: (dial wrapDay: secs + toBoundary - self leadSeconds)
                      post: #alarm on: tick.
      alarmOk ifFalse: [
        'trigger=WATCHDOG ONLY — rtcAlarm refused (image built without prj_esp32c6_rtcalarm.conf?)' out ].
      timers after: (toBoundary * 1000) + self graceMillis post: #watchdog on: tick.
      ^ self )

    public nearBoundary: secs = (
      ^ (dial secondsToBoundaryAfter: (dial slotOf: secs) from: secs) <= self leadSeconds )

    public countdownAt: secs = (
      (armedFor == nil) ifFalse: [ ^ self ].
      armedFor:: dial nextAfter: (dial slotOf: secs).
      timers after: ((dial secondsToBoundaryAfter: (dial slotOf: secs) from: secs) * 1000)
                    + self strikeMarginMillis
             post: #countdown on: tick.
      ^ self )

    public retry = ( timers after: self retryMillis post: #retry on: tick. ^ self )

    public leadSeconds = ( ^ 5 )
    public graceMillis = ( ^ 2000 )
    strikeMarginMillis = ( ^ 200 )
    retryMillis        = ( ^ 60000 )

    setAlarm: a timers: t tick: c dial: d = ( alarm:: a. timers:: t. tick:: c. dial:: d. ^ self )
  ) : (
    public on: alarm timers: timers tick: tick dial: dial = (
      ^ self new setAlarm: alarm timers: timers tick: tick dial: dial )
  )

  class Striker = ( | gpio timers chan pin
                     striking ::= false. strokeCount ::= 0. strokeIndex ::= 0. pinOn ::= false.
                     gpioMissing ::= false. | ) (
    public striking = ( ^ striking )

    public ring: n = (
      striking ifTrue: [
        ('bells ' , n printString , ' REFUSED — a pattern is already in flight') out.
        ^ false ].
      ((n < 1) or: [ n > 8 ]) ifTrue: [
        ('bells ' , n printString , ' REFUSED (allowed 1..8)') out.
        ^ false ].
      strokeCount:: n.
      strokeIndex:: 0.
      striking:: true.
      self strikeOn.
      ^ true )

    public step = (
      pinOn ifTrue: [ self strikeOff ] ifFalse: [ self strikeOn ].
      ^ striking )

    strikeOn = (
      strokeIndex:: strokeIndex + 1.
      pinOn:: true.
      self pin: true.
      timers after: self widthMillis post: #stroke on: chan.
      ^ self )

    strikeOff = (
      pinOn:: false.
      self pin: false.
      strokeIndex = strokeCount ifTrue: [
        striking:: false.
        'bells done' out.
        ^ self ].
      timers after: (self gapAfter: strokeIndex) post: #stroke on: chan.
      ^ self )

    public gapAfter: i = (
      (i \\ 2) = 1 ifTrue: [ ^ self pairGapMillis ].
      ^ self setGapMillis )

    pin: aBool = (
      [ gpio set: pin to: aBool ]
        on: [ :e | e tag = #gpioUndeclared ]
        do: [ :e |
          gpioMissing ifFalse: [
            gpioMissing:: true.
            ('bell: GPIO' , pin printString ,
              ' not declared on this board — keeping time, ringing nothing') out ] ].
      ^ self )

    public widthMillis   = ( ^ 10 )
    public pairGapMillis = ( ^ 500 )
    public setGapMillis  = ( ^ 1600 )

    setGpio: g pin: p timers: t channel: c = ( gpio:: g. pin:: p. timers:: t. chan:: c. ^ self )
  ) : (
    public on: gpio pin: pin timers: timers channel: channel = (
      ^ self new setGpio: gpio pin: pin timers: timers channel: channel )
  )
) : (
)
```

## Appendix D. The agent's HTTP surface

custos serves these routes today. Every board's verbs are also served under `/board/<name>/`, and with one board
configured the board-less paths are its shorthand (peer design D15). `GET /boards` lists the boards and the verbs
each serves. The build, load, debug and profile verbs each also have a page for a person [Peer].

| Route | What it answers |
|---|---|
| `GET /boards` | JSON: every board custos serves, its target (cable, host VM, UDP sink, or a node through the gateway) and the verbs it has |
| `POST /build?for=<board>&name=F.nsl` | the `.nsl` as the body; compiles it for that board and answers `Board`, `Image`, `SHA256` and `Size`, or the compiler's own error. `GET /build` is the page |
| `GET /build/last.oimg`, `/build/last.oimg.map.json` | the last build's image and its map, for `POST /load` |
| `POST /run?for=<host board>&name=F.nsl` | host boards only: builds the `.nsl`, runs it once on the host VM and answers `Output`, `Exit` and `TimedOut` |
| `POST /load?name=F.oimg` | the image as the body; loads it onto the board or host VM and answers `LoadedMs`, `Result` and `Debug`. On a node through the gateway it is a push: the gateway signs the image and holds it as the node's goal, and the answer comes at once, in the same shape with `Push` added (`SHA256`, the goal's `Seq`, `Running`, the image the node runs, and `Converged`). `GET /load` is the page; `GET /load/last` answers the last load, re-reading the node on each call |
| `GET /debug/state?after=N` | the debug `State` once its `Seq` passes N: a long-poll, 25 s at most. `GET /debug` is the page |
| `POST /debug/do` | one debug command as the body (`break`, `continue`, `step`, `next`, `finish`, `bt`, `locals`, `pause` and the rest); answers `OK`, `Error` and `State` |
| `GET /debug/source` | the app's lines and which of them have code |
| `POST /debug/patch?method=Class>>selector` | the edited `.nsl` as the body; patches one method of a program stopped in a debug build and answers as `/debug/do` does |
| `POST /profile/capture?ms=N` | captures a window of N ms and answers the ranked methods in `Report`. `GET /profile` is the page |
| `GET /profile/last` | the last capture |
| `GET /log`, `GET /runs`, `GET /panics`, `GET /panics/{n}/bt` | the collected console: lines (`?since=` or `?tail=`), one row per boot with its build id, the panics, and one panic's backtrace |
| `POST /lend?s=N`, `POST /return` | closes the board's cable so another tool (a flasher) can use it for up to N seconds, and takes it back [BoardTut] |
| `GET /health` | per board, the console cable's state (for example `up` or `lent`); the board-less path is also the proxy to porta's health |
| `GET /q/{name}`, `GET /fleet`, `GET /node/{id}` | with a porta store configured: the named queries `liveness`, `convergence` and `node` (`?id=`) as JSON; `/fleet` and `/node/{id}` are the pages. |

An agent uses `build`, `run`, `load`, `debug` and `profile` in that order (on a node through the gateway, it polls `load/last` until `Converged`), reads `log` and `runs` for what the board
did on its own, and uses `lend` only when a tool outside custos must hold the cable.

<div style="page-break-before: always"></div>

## References

- **[ZT10]** B. Trinkenreich, A. Lawson and H. Carter, *Zephyr Turns 10: A Decade of Adoption, Maturity, and Ecosystem
  Evolution*, The Linux Foundation, March 2026, CC BY-ND 4.0 —
  [copy in this repository](../../../docs/ZephyrTurns10_Report_Revised_051826.pdf). 900+ boards; the learning curve;
  Figure 35 (p. 30), GenAI.
- **[Clo96]** K. Clohessy, *Using object-oriented programming tools to build real-time embedded systems*, Real-Time
  Engineering, Fall 1996 (Object Technology International / IBM). Cited only; not copied into the repository.
- **[Bar89]** B. M. Barry (Defence Research Establishment Ottawa), *Prototyping a Real-Time Embedded System in
  Smalltalk*, Proceedings of OOPSLA '89, New Orleans, ACM SIGPLAN 1989, pp. 255–265; Clohessy's own reference [Clo96].
  Read for §2 and §11. Its "five times the effort in C or Ada" is the author's estimate without a control and is not
  used here.
- **[Spans]** The job-latency instrumentation rung (#61, closed 2026-07-21): the entry in
  [the program ladder](../../../review/program-sequence.md) and
  [its design](../../../docs/design/2026-07-21-job-latency-instrumentation-design.md).
- **[Perf]** [Optimize the right thing](../../../review/performance_slides.html), review deck, 2026-07-22: the
  obstacle course.
- **[Script]** [Script Zephyr, don't replace it](../../../review/slides.html), review deck, 2026-07-11: the comparator
  posture.
- **[MP]** [rete #383](https://github.com/davidg238/rete/issues/383), the same C6 node in MicroPython, scored on
  measured rows.
- **[Lang]** [nsl: a Newspeak dialect in 112 KB — what it keeps, what it drops, and why](nsl-design-paper.md), the
  companion paper: the RAM tables, the VM, capabilities, the dialect, and the fit of the language to an agent.
- **[Ledger]** [The nsl dialect ledger](NEWSPEAK_DIALECT.md): every divergence from Newspeak, its reason and what
  would revisit it (rows 1–32, audited 2026-10-06).
- **[VM]** [oevm — what the VM is](../../oevm/docs/vm.md).
- **[Pivot]** [The July 2026 rewrite's design](design/2026-07-03-nsl-tuvm-first-step-design.md): no non-local
  return, no fibers, the reactor; the 256 KB target.
- **[MD]** [Milestone D](../../oevm/docs/MILESTONE_D.md) (the supervisor).
- **[Boards]** The board profiles in [`run.sh`](../../oevm/zephyr/run.sh) and
  [the board ports](../../oevm/zephyr/docs/BOARD_PORTS.md); the nRF52840 RAM split from that build's linker symbols.
- **[RP2350]** [First numbers on the RP2350](../../../docs/results/2026-10-01-rp2350-first-numbers.md).
- **[Image]** [The image and its map](../../../protocol/IMAGE-ARTIFACTS.md).
- **[Porta]** [porta's architecture](../../../porta/docs/ARCHITECTURE.md), §5: power modes and app lifecycle.
- **[Serial]** [Adding the serial device](design/2026-06-22-ns-oevm-stdlib-step3c3b-serial-design.md): a device
  target is purely additive.
- **[Checkin]** [The check-in after the codec fix](../../../docs/results/2026-08-09-checkin-after-the-codec-fix.md):
  `pushEncode` 124 → 53 ms on the C6.
- **[CborShape]** [What shape is the encode's cost?](../../../docs/results/2026-08-09-cbor-encode-shape.md)
- **[CborArms]** The #297 spike, `docs/spikes/2026-10-04-cbor-prim-arms/`, on branch `297-cbor-spike` (throwaway; not on master).
- **[CborNative]** The #297 design, `docs/design/2026-10-04-297-cbor-native-walk-design.md`, on branch
  `297-cbor-native-walk` (not yet on master).
- **[Hanoi]** [Native substitution, the Hanoi spike](../../../docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md).
- **[Liveness]** [The liveness observer](../../../docs/design/2026-08-16-vm-liveness-observer-design.md).
- **[Peer]** [custos, the developer's peer](../../../docs/design/2026-10-04-custos-peer-design.md): agent first
  (D11–D13), load, patch, build and run (D14, D17, D18), the console collector (D16), remote debug (D9, D19).
- **[Console]** [custos as the console collector](../../../custos/docs/console.md).
- **[BoardTut]** [Tutorial 2, a board](../../../docs/tutorials/02-board.md): the collector, `runs`, `lend` and `return`.
- **[Debugger]** [Tutorial 4, the debugger](../../../docs/tutorials/04-debugger.md): the executed HTTP shapes and the
  C6 transcript of 2026-10-05.
- **[Patch]** [Stop, patch, restart, resume](../../../docs/results/2026-10-06-313-patch-restart-spike.md).
- **[Profile]** [Profiling over USB](../../../docs/design/2026-10-05-custos-profile-over-usb.md).
- **[Spike]** [Debugging and profiling a sleepy node through porta, host spike for rete#342](../../../docs/results/2026-10-07-342-sleepy-node-debug-spike.md).
- **[CborAot]** [What would AOT-ing `Cbor>>encode:` return?](../../../docs/results/2026-10-09-cbor-aot-spike.md),
  the spike of 2026-10-09/10 on c6spare (rete#395).
- **[Field]** [A debugger through porta, on two boards](../../../docs/results/2026-10-10-380-debug-through-porta-on-boards.md):
  the MagTag and the wall C6, 2026-10-09 and 2026-10-10 (rete#379, rete#380).
- **[Example]** [`MagTagDeepSleepNode.nsl`](../../oevm/zephyr/MagTagDeepSleepNode.nsl) and
  [`lib/Bells.nsl`](../../oevm/zephyr/lib/Bells.nsl), the program in Appendices A and C; its measurements are on
  [rete#380](https://github.com/davidg238/rete/issues/380).
- **[Wall]** [`ShipsBellNodeC6.nsl`](../../oevm/zephyr/ShipsBellNodeC6.nsl), the always-on bell of Appendix B, over
  the same library (rete#380).
- **[Display]** [The MagTag e-paper display design](../../../docs/design/2026-07-23-magtag-epaper-display-seam-design.md)
  and [`MagTagDisplayNode.nsl`](../../oevm/zephyr/MagTagDisplayNode.nsl), the program Figure 3 quotes.
- **[Clock]** [`MagTagClockNode.nsl`](../../oevm/zephyr/MagTagClockNode.nsl), the program in Figure 4.
- **[Manifest]** [`MANIFEST.cddl`](../../../protocol/MANIFEST.cddl), the normative catalogue of paths, kinds and
  shapes, and [its design](../../../docs/design/2026-08-24-kind-schema-manifest-design.md).
