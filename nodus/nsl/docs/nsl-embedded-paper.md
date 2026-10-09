# Objects on a 256 KB radio node: remote debug, profiling and agent-written code over Zephyr

_DRAFT 1, 2026-10-08, for iteration with David. The companion paper, on the language, is [nsl-design-paper.md]; each topic has one owner and the other paper cites it._

Sources are bracketed keys, e.g. [VM], resolved in the References; in this Markdown they are links, and only there.
Anything that is designed but not built says so in the sentence that describes it.

## Abstract

Zephyr reaches more than 900 boards, with connectivity, security and drivers built in; its community names the
learning curve as its most persistent cost [ZT10]. This paper asks what a small object language on top of Zephyr
could buy a Zephyr engineer, and what it costs.

The case has three parts. A program can be changed without reflashing the firmware. Debugging and profiling can be
driven from a host over the one USB cable already on the bench. And, designed but not built, the same tools could
reach a node over the radio, including one that is asleep. A fourth follows: the loop is short enough to check, by
running it, the code an agent wrote.

The costs come first. The interpreter is about a hundred times slower than C on the one method measured both ways, the
VM takes 112 KB of a 256 KB part, and no model was trained on nsl. What is built and gated today is the desktop and
USB half. Remote debug through a gateway is a host spike that passes its gate (`SLEEPY DEBUG OK`), not a board
result. Program over-the-air load is likewise designed, not built.

## 1. The problem

Three problems sit under this paper. None is new or claimed solved.

**The learning curve.** Zephyr's ten-year survey reports it as the most persistent challenge for contributors
[ZT10]. A newcomer who wants a sensor reading on a radio node meets devicetree, Kconfig, west and the
network stack's configuration before any application logic runs. This paper does not argue Zephyr's layers should be
smaller; it asks whether the application on top can be easier to start.

**The field node you cannot reach with a probe.** On the bench, a node has a debug probe or a cable. In a wall, on a
roof or in a box, it has neither. It reports through a weak, slow radio and often sleeps for minutes or hours between
exchanges. The usual tools are the log it chose to send and a site visit. A fault that appears only in the field,
after a week, on the node that is hard to reach, is the one you would most like to stop at a line.

**Code an agent wrote, and no way to trust it.** Zephyr's contributors already use AI to write code and do not trust it. Of 125 contributors surveyed, 54 % say it helps them write code faster, and 54 % say it gives unexpected or
incorrect output they must fix frequently; 33 % spend more time validating its output than they would writing from
scratch; 18 % say it helps them write code with fewer errors; and 78 % do not trust its suggestions without heavy
review ([ZT10], Figure 35). This is a verification problem: an engineer who must read every line of generated C closely has gained little.

## 2. Why objects, on a microcontroller?

A Zephyr `struct device` carries a `const void *api` pointer. For a sensor, it points at a struct of function
pointers that the driver fills in, and the caller calls through that struct: dispatch through a vtable, written by
hand, with the class defined by convention. Object languages make the same move uniform: every operation,
including arithmetic, is a send, and a send finds its method in a table.

**The cost comes first.** One method activation in nsl costs about 12 µs: 11.83 µs on the ESP32-C6 and 11.13 µs on
the RP2350 [RP2350]. The one direct comparison, the Hanoi spike, found C about a hundred times faster on that method [Hanoi]. There is no JIT, because a 256 KB part
has no room for a code cache. The radio, the crypto and the drivers stay in C, under Zephyr, and nsl makes decisions at the rate events arrive. A
timer, a packet and a button are slow enough. A codec that walks every value is not, and it is the one place the
cost has been felt; the worked example below is that codec.

**What a uniform send buys.** Three consequences of one mechanism.

- **Native substitution.** Whether a method is interpreted or C is one field of its binding, so a hot method moves
  to C without its callers, the interpreter or the image format changing. On a Towers of Hanoi spike, a best case with
  all the work in one method, the C version ran 116–119× faster [Hanoi]; the CBOR encoder below is the real workload.
- **A swapped `platform`.** A program reaches every device through one object, `platform`, so a different `platform`
  can carry a simulated device without a line of the program changing. Version 0.1 does not use this: `platform`
  hands out every device, and there is one kernel (ledger row 32 [Ledger]).
- **Capabilities.** For the same reason, a narrower `platform` can be handed down, so a module given only the
  network can use only the network, and says so in its header. The companion paper owns this argument [Lang]; §9 uses one consequence.

**A worked example: the CBOR encoder.** Every node encodes what it sends to the gateway as CBOR. The encoder is nsl,
with C only at the leaves: integers, floats and the decode heads have been primitives since the codec's first version (2026-07-09). On
an ESP32-C6, encoding the gateway's real 425-byte push frame took 124 ms; a fix in nsl alone (less copying, a
cheaper byte buffer) halved it, to 53 ms, about a quarter of a check-in [Checkin]. Measuring where the rest went
showed that the cost is the walk itself: about nine interpreted activations per byte, of which the CBOR logic is half a
percent [CborShape]. At a check-in every two seconds that is tolerable. At a 50 Hz control loop it is the whole tick.

A spike then tried four ways of encoding the same frame, all producing identical bytes, on the host (2026-10-04)
[CborArms]:

| Arm | ns per encode, host | Against today |
|---|---:|---:|
| Today: the walk in nsl, C leaves | 259 950 | 1× |
| More of the leaves in C, the walk still in nsl | 86 595 | 3× |
| The whole walk in C, which knows the container classes | 1 904 | 137× |
| The walk in C, finding each container's encoding through its class's method table | 2 045 | 127× |

Moving more bytes into C buys only 3×, because what remains is roughly 400 ns of interpreted sends per element. The
walk has to move. The last two rows are the choice, and it is the reason this section is about dispatch.

The pure C walk is the fastest, and it is what a C programmer would write: a switch on the four container classes the
encoder knows about. That switch is a copy of the class hierarchy, held in C, which has to change whenever an nsl
class does. The chosen design instead has each container class declare, in one line of nsl, that its encoding is
native (`cborWalk = native cborList`, and so on), and the C walk asks each container's method table for that binding.
It costs 4–7 % on this frame, and up to 12 % on a small map, over the pure C walk, and the spike's emulation of the
lookup overstates it [CborNative]. What it buys:

- **C does not know the class hierarchy.** A class's method table says whether its encoding is native; nothing in C
  names List, Map or Record.
- **The nsl encoder stays, as the authority.** Anything the C walk does not recognise — an unknown class, nesting
  deeper than 32, a NaN — it refuses, and the unchanged nsl walk produces the exact bytes or the exact error. Error
  texts and edge cases stay defined in one place.
- **A shape change is caught at build time.** Each native declares the instance variables its C reads; the image
  build refuses a class whose declaration does not match, naming the class.
- **Callers and the image format do not change.** It is native substitution (above), applied to a real workload.

The price on the device has not been measured. The issue estimates about 1 ms on the C6 for the frame that takes 53 ms
today; the host ratio would suggest less, and the design says plainly that host ratios are not device numbers. **This
is designed and planned (rete #297), not built.**

A vtable costs time on a part with little of it, and returns substitution, simulation and attenuation. For a tight
sensor loop, C is the better answer, and nsl would call it.

## 3. The system at a glance

![The system](img/nsl-system.svg)

_Figure 1. Field nodes (A′, B′) run commissioned images and reach the gateway over Thread or WiFi; porta mirrors
each node's state and keeps the images it commissioned. Each developer works on a twin on the desk (A, B) over one
USB cable, and on test nodes on the desktop's VM, through custos, with the same HTTP calls whether a person or an
agent makes them. A and A′ drift apart while a bug is chased on both, which is why porta, not the developer's
machine, is to be the custodian of what runs in the field (decided; the commissioning step is not yet built).
Reaching a field node's debugger through porta is designed, not built [Porta], [Peer]._

porta (Go, always on), custos (Go, on the desk) and the node meet only in the wire format `protocol/` [Porta], [Peer].

![The node's stack](img/e-node-stack.svg)

_Figure 2. One node. Zephyr, OpenThread and mbedTLS are C and unchanged. The VM is one Zephyr thread running a
read-only program image in place from its own flash partition; the liveness observer is a second C thread watching
its progress. The console cable carries the log, the loader and the debugger's lines._

RAM is the tight resource. On the nRF52840, with the Thread build of 2026-10-03, Zephyr, OpenThread and
mbedTLS take about 140 KB of 256 KB, the VM's arena gets 112 KB, and 4 KB are spare at link time [Lang], [Boards]. The VM's code is about 42 KB of a 1 MB flash, so flash is not the
axis that binds.

## 4. What you write

An nsl program is a class whose header names what it was given. It keeps state under path names in a **space** and
reacts to writes at those paths. Appendix A shows a whole Thread node in 36 lines; two parts follow.

The first is the reaction that answers the gateway:

```
(space channel: 'goal/name') whenever: '' do: [ :v | space put: 'obs/name' value: v ].
```

The gateway writes `goal/name`, the reaction runs, and the node answers at `obs/name`: the program names paths, not
addresses. The second is the method that runs once per cadence:

```
tick = (
  ticks:: ticks + 1.
  bridge checkIn.
  space out: 'tel/log' value: (platform Record tag: #Log
    with: #lvl -> #info with: #txt -> ('tick ' , ticks printString)).
  ^ self
)
```

It exchanges with the gateway and appends one row to a log stream. The node in Appendix A uses five paths:

| Path | Kind | Who writes it | Who reads it |
|---|---|---|---|
| `goal/name` | cell | the gateway, as intent | the node, which reacts |
| `obs/name` | cell | the node, as its answer | the gateway |
| `tel/log` | stream | the node, a row per tick | the gateway, every row |
| `sys/timer` | stream | timer interrupts | the timers' dispatcher |
| `sys/tick` | stream | the node's own timer | the node's `tick` |

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

## 5. The reactor in practice

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
wants, a new setting or a new program, until the node next wakes and asks. §8 depends on this.

## 6. Develop without reflashing

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

## 7. Debug, patch and profile over USB

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

## 8. Remote: through the gateway, to a node that is asleep

**Everything in this section is designed, not built, except one host spike** that runs a real porta and a host VM on
the desktop and prints `SLEEPY DEBUG OK`. No board has been debugged through porta. Issues #375, #376, #377 and #380
are the work that would change that; program over-the-air load (#233) is also unbuilt on boards.

![Remote debug sequence](img/e-remote-debug-seq.svg)

_Figure 6. Designed, not built on a board. A breakpoint set through custos is held by porta as intent and delivered
when the sleeping node next asks; the node reports the stop and stays awake while the developer steps._

**The design (peer design D9, D19).** A breakpoint is intent, so it is written under the node's `goal/` paths, and
porta holds it until the node next wakes and asks. On a hit, the node reports the stop and stays awake, one exchange per command. The
liveness observer already knows not to reset a node held at a breakpoint over USB [Debugger], and the VM would hold
the same state whichever link serves the command loop (#377). Two demos are planned, both also loading a program
through porta: a MagTag, which deep-sleeps, and the wall-powered ShipsBell C6, which runs continuously [Peer]. The
`Debug` record in the manifest would carry the breakpoints and the program's hash, because a breakpoint is an offset
in one image, and an `obs/debug` cell would report a held node (#375).

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

These are loopback numbers; a Thread round trip costs a radio exchange, so the rate on a board will be far lower, by
an unmeasured amount. The spike also falsifies its own gate: breaking the `dbg/req` intercept turns leg A red, breaking the
`goal/debug/main` intercept turns leg B red, and a control leg sending the same rows to a standard image gets no
answer.

**The costs.** Holding a node costs a busy CPU, which a battery node cannot afford. The
spike's own proposal, a long-poll at porta, was not accepted, and what an abandoned session should do is open (#378).
The store growth of about 4 MB in 39 s is a cost on the gateway. A held node also reads as `#neverSeen` to porta, which has no held verdict; the `obs/debug` cell is meant to fix
that. A debug image waits at boot for
a cable that a field node does not have, so that wait must go (#376). And no sleepy node that talks to porta exists today: the MagTag deep-sleeps without a porta bridge, and every
bridged node is always on [Spike].

**Program over the air (#233).** A program push into the flash region the USB loader writes is proven on the desktop in a fleet-simulation gate and
is inert on boards [Peer]. Until then, a sleepy node can be debugged only if its program is already the one the breakpoints were built against.

## 9. Trusting code an agent wrote

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
writes more correct nsl than C. The bet is that a short, repeatable loop makes each check cheap, and cheap checks are
what the 33 % in §1 are missing. The test that would settle it gives the same small board tasks to an agent in nsl,
with the language in its context, and in Zephyr C, scored on first-run correctness. It has not been run (rete issue
#373); we will publish the result either way.

One more caution. Verification by running catches only what running reveals. A reaction that is wrong only under a
timing or radio condition the host VM does not reproduce passes on the host and fails on the board; the host suite is
blind to the interrupt seam, and nothing in this loop closes that gap.

## 10. Costs compared with Zephyr C

| Cost | Compared with Zephyr C | Would lift it |
|---|---|---|
| Speed per byte | about 12 µs per method activation; about a hundred times slower than C on the one method measured both ways [Hanoi]; no JIT | native substitution of a hot method (§2); or leave it in C |
| RAM given to the arena | 112 KB of 256 KB on the nRF52840, with 4 KB spare at link [Boards] | a larger part; the C6 has 128 KB of arena by choice, and WiFi sets the classic ESP32 to 48 KB |
| No compile-time types | a misspelt selector is found when it runs, not compiled | the error line names the nearest selector, which softens the cost [Lang] |
| A language in no model's training data | an agent writes nsl from context, not habit | the test in §9 (#373); not yet run |
| One kernel | no virtual classes, no per-target library, `platform` hands out everything [Ledger] | narrowed platforms (row 32) |
| Tooling at version 0.1 | the compiler is a Python tool, and the loader exists for one board, the ESP32-C6 | more loaders; each board needs its own region and flasher |
| Debug images are separate | 11.6 % per bytecode and about 500 B of flash [Debugger] | not planned; the standard image stays lean |
| Remote debug is unbuilt | no board has been debugged through porta; the spike is host only [Spike] | #375–#377 and the C6 demo #380, then the MagTag demo #379 |
| A single maintainer | one maintainer for the VM, kernel and tools | not planned |

For nodes that decide rather than compute, the cost of interpretation is small beside a reflash cycle, a missing
debugger and a site visit. That is shown on the desktop and over USB on one board. It is not shown on a field node,
over a radio, or against an agent writing C; those are next.

<div style="page-break-before: always"></div>

## Appendix A. A Thread node in 36 lines of code

A node that joins the gateway over Thread, checks in every two seconds, publishes a log line each time, and answers
a name the gateway gives it. Thread itself is attached by the firmware before the VM starts; the program sees an IPv6
UDP socket. It compiles for the nRF52840; it is cut down from the bench's porta node, which runs, and has not itself
been run [Example].

```
(* The header is the program's authority: the platform and the network, nothing else. *)
class ThreadSensorNode usingPlatform: platform net: net = (
  | space bridge timers ticks ::= 0. |
) (
  gwAddr  = ( ^ 'fdc5:9109:9afe:97ea:9541:d963:f8d9:8567' )   (* the gateway, mesh-local *)
  gwPort  = ( ^ 6969 )
  cadence = ( ^ 2000 )                                         (* ms between check-ins *)

  public main: platform2 args: args = (
    | sock tickChan |

    (* The space: this node's state, kept under path names rather than addresses. *)
    space:: platform space.

    (* The bridge mirrors parts of the space to porta: it pulls what porta wants
       (goal/...) and pushes what the node reports (obs/..., tel/...). *)
    sock:: net udpBindPort: 16970.
    sock == nil ifTrue: [ 'bind failed' println. ^ 1 ].
    bridge:: platform PortaBridge
      socket: sock space: space id: platform nodeId peer: self gwAddr port: self gwPort
      spans: platform Spans new.
    bridge subscribeStream: 'tel/log'.               (* push every row of this stream *)
    bridge liveness: self cadence misses: 16.        (* tell porta when to call us silent *)
    bridge publishBuild: #nrf52840.

    (* porta writes a name at goal/name; the node answers at obs/name. *)
    (space channel: 'goal/name') whenever: '' do: [ :v | space put: 'obs/name' value: v ].

    (* Timers, like everything else, come from the platform. Their interrupts post to
       sys/timer; one reaction hands each to the timer that owns it. *)
    timers:: platform timers.
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    tickChan:: space channel: 'sys/tick'.
    tickChan whenever: '' do: [ :m | self tick ].
    bridge startTicking: timers on: tickChan.        (* one tick per cadence *)

    (* The reactor: wait for an event, run the reactions it queued, repeat. *)
    [ true ] whileTrue: [ platform pumpEvents. space drain ].
    ^ 0
  )

  (* Each tick: exchange with porta, then add one row to the log stream. *)
  tick = (
    ticks:: ticks + 1.
    bridge checkIn.
    space out: 'tel/log' value: (platform Record tag: #Log
      with: #lvl -> #info with: #txt -> ('tick ' , ticks printString)).
    ^ self
  )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

The node never names an address inside the program except the gateway's. Everything else is a path in its space:
porta writes `goal/name` and reads `obs/name` and `tel/log` at the node's next check-in, and the timers reach the
program at `sys/timer` and `sys/tick` the same way (§5).

<div style="page-break-before: always"></div>

## Appendix B. The agent's HTTP surface

custos serves these routes today. Every board's verbs are also served under `/board/<name>/`, and with one board
configured the board-less paths are its shorthand (peer design D15). `GET /boards` lists the boards and the verbs
each serves. The build, load, debug and profile verbs each also have a page for a person [Peer].

| Route | What it answers |
|---|---|
| `GET /boards` | JSON: every board custos serves, its target (cable, host VM, or UDP sink) and the verbs it has |
| `POST /build?for=<board>&name=F.nsl` | the `.nsl` as the body; compiles it for that board and answers `Board`, `Image`, `SHA256` and `Size`, or the compiler's own error. `GET /build` is the page |
| `GET /build/last.oimg`, `/build/last.oimg.map.json` | the last build's image and its map, for `POST /load` |
| `POST /run?for=<host board>&name=F.nsl` | host boards only: builds the `.nsl`, runs it once on the host VM and answers `Output`, `Exit` and `TimedOut` |
| `POST /load?name=F.oimg` | the image as the body; loads it onto the board or host VM and answers `LoadedMs`, `Result` and `Debug`. `GET /load` is the page; `GET /load/last` answers the last load |
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

An agent uses `build`, `run`, `load`, `debug` and `profile` in that order, reads `log` and `runs` for what the board
did on its own, and uses `lend` only when a tool outside custos must hold the cable.

<div style="page-break-before: always"></div>

## References

- **[ZT10]** B. Trinkenreich, A. Lawson and H. Carter, *Zephyr Turns 10: A Decade of Adoption, Maturity, and Ecosystem
  Evolution*, The Linux Foundation, March 2026, CC BY-ND 4.0 —
  [copy in this repository](../../../docs/ZephyrTurns10_Report_Revised_051826.pdf). 900+ boards; the learning curve;
  Figure 35 (p. 30), GenAI.
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
- **[Example]** [`ThreadSensorNode.nsl`](examples/ThreadSensorNode.nsl), cut down from
  [`PortaBridgeNodeNrf.nsl`](../../oevm/zephyr/PortaBridgeNodeNrf.nsl).
- **[Display]** [The MagTag e-paper display design](../../../docs/design/2026-07-23-magtag-epaper-display-seam-design.md)
  and [`MagTagDisplayNode.nsl`](../../oevm/zephyr/MagTagDisplayNode.nsl), the program Figure 3 quotes.
- **[Clock]** [`MagTagClockNode.nsl`](../../oevm/zephyr/MagTagClockNode.nsl), the program in Figure 4.
- **[Manifest]** [`MANIFEST.cddl`](../../../protocol/MANIFEST.cddl), the normative catalogue of paths, kinds and
  shapes, and [its design](../../../docs/design/2026-08-24-kind-schema-manifest-design.md).
