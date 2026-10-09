# nsl: a Newspeak dialect in 112 KB — what it keeps, what it drops, and why

_DRAFT 4, 2026-10-09, for iteration with David. The companion paper, for the embedded developer, is [nsl-embedded-paper.md](nsl-embedded-paper.md); each topic has one owner and the other paper cites it._

Sources are bracketed keys, e.g. [VM], resolved in the References; in this Markdown they are links, and only there.
Anything that is designed but not built says so in the sentence that describes it.

## Abstract

Zephyr's reach, more than 900 boards with connectivity, security and drivers built in, means a VM ported once can go
a long way; its learning curve, which its own community names its most persistent challenge [ZT10], is what an
approachable language might ease.

nsl is a Newspeak dialect that puts an object-capability language over Zephyr. Zephyr carries the freight, nsl directs
traffic: the radio, the crypto and the drivers stay in C, and the interpreter pays mostly for decisions. A host
compiler produces a read-only image that a small C VM runs in place from flash, in the roughly 112 KB that Zephyr,
OpenThread and mbedTLS leave on a 256 KB nRF52840. nsl keeps Newspeak's structure — modules as objects, no globals,
capabilities as the unit of authority — and drops most of its dynamism, each drop with its reason and its cost.

Nothing here is new. Modules as objects and capabilities are Newspeak's, the object model is Piumarta and Warth's,
the space is Linda's, and image-based deployment is Smalltalk's. This paper is an engineering account of fitting those
techniques into what Zephyr leaves of 256 KB, and of what each one cost. It is written for a small audience that
knows them, and it wants to be solid rather than exceptional.

Last, what makes a language fit for an agent. Agents can't smell trouble, but they can find patterns in data, so the
language is judged on what it puts in front of one: it fits in a context window, its authority is visible in a
header, its primitive set is closed, and its error lines are written for a reader that is a program. That the
language suits an agent is an argument, not a measurement (§8). Whether you can trust what an agent wrote is a
different question, and the companion paper owns it [Embedded].

![The system](img/nsl-system.svg)

_Figure 1. Field nodes (A′, B′) run commissioned images and reach the gateway over Thread or WiFi; porta mirrors
each node's state and keeps the images it commissioned. Each developer works on a twin on the desk (A, B) over one
USB cable, and on test nodes running on the desktop's VM, through custos — the same HTTP calls whether a person or an
agent makes them. A and A′ drift apart while a bug is chased on both, which is why porta, not the developer's
machine, is to be the custodian of what runs in the field (decided; the commissioning step is not yet built).
Reaching a field node's debugger through porta is designed, not built. porta and custos share only the contract in
`protocol/` [Porta], [Peer]._

## 1. The target

nsl's floor is a 256 KB-RAM microcontroller with a radio. That is a hard target but not an extreme one: it is about
as small as a part can be and still hold Zephyr, a Thread stack and an application, and it covers common radio parts,
the nRF52840 and the ESP32-C6 among them. Smaller parts are not a goal. Size was the target, but simplicity was the
mechanism: the July 2026 rewrite removed non-local return, fibers and growable stacks because they had become "a
cascade of interacting complexity … with the VM ballooning far past its simple intent" [Pivot], and the bytes it
saved were modest — 3 921 B of code, 676 B of RAM [MA]. Most of the choices in §5 would hold on a large board too
(Figure 2).

**What 256 KB leaves.** Measured from the linker symbols of the nRF52840 dongle's Thread build (2026-10-03):

| Use | RAM |
|---|---:|
| VM arena: objects, the VM's own 16 KB stack, floats | 112 KB |
| Zephyr kernel, drivers, USB, thread stacks | 57 KB |
| Zephyr system heap | 32 KB |
| OpenThread and the 802.15.4 radio driver | 25 KB |
| Network buffers | 14 KB |
| mbedTLS | 10 KB |
| Spare | 4 KB |

The rows sum to 254 KB; the other 2 KB is not yet accounted for (rete #374). Zephyr and its radio stack take
about 140 KB and nsl gets the rest, with 4 KB to spare at link time. The VM's code is not the tight axis — about 42 KB
of a 1 MB flash. RAM is.

**The tightest arena is not on the smallest part.** Across the boards the VM runs on [Boards]:

| Board | RAM | VM arena | What sets it |
|---|---|---:|---|
| ESP32 (WROOM) | 520 KB, of which Zephyr maps ~192 KB as data RAM | 48 KB | WiFi's driver heap and TCP buffers take the rest |
| ESP32-S2 (MagTag) | 320 KB | 64 KB | WiFi; 92 KB links, 64 KB is what has booted |
| nRF52840 | 256 KB | 112 KB | Zephyr, OpenThread and mbedTLS; 4 KB spare |
| ESP32-C6 | 512 KB | 128 KB | a chosen default with margin; 256 KB links with the radio |

WiFi, not the size of the part, is what squeezes the classic ESP32 to 48 KB.

**An interpreter, with no JIT.** There is no room here for a JIT's code cache, so every method runs in the
interpreter. One method activation costs about 12 µs (11.83 on the C6, 11.13 on the RP2350 [RP2350]). The one direct
comparison, the Hanoi spike, found C about a hundred times faster on that method (116–119×) [Hanoi]. That is cheap at
the rate events arrive — a timer, a packet, a button — and expensive per byte, which is why the radio, the crypto and
the drivers stay in C, and why code that turns out hot can follow them (§2).

**A program that cannot be written at run time.** The program is a read-only image in its own flash partition,
executed in place [Image]. The firmware is flashed once; programs are loaded beside it.

**A peer that is not nsl.** A gateway written in Go mirrors each node's state over UDP and CBOR, so the wire format is
a contract between two languages, not an nsl object serialisation [Porta].

## 2. The VM

oevm is a register VM in C, built on Piumarta and Warth's Open Extensible Object Model [OEOM]: object, symbol and
vtable are the only primitive kinds, and everything else — arithmetic included — is a message that a vtable answers.
Instructions are 32 bits and fixed length, 31 of them; small integers take a fast path and fall back to a send. One
instruction carries Newspeak's implicit-receiver send, which looks up a name in the lexically enclosing classes before
the superclasses. The interpreter is one flat, non-recursive loop over one fixed-capacity stack, and a context lives
on the heap so that a block can outlive the frame that made it — which is the whole reactive model (§4). Memory is a
fixed arena with a precise mark-sweep collector and no `malloc` once running [VM]. The collector is
stop-the-world and the heap reports itself as data: every check-in the node publishes its live bytes, high-water
mark, largest free block and the number of collections since boot, so a shortage of contiguous space can be told
from a shortage of space. What it does not yet report is how long a collection takes: no pause
has been timed, on the host or a board, and that is the first number an embedded reader would ask for (rete #382).

The kernel is nsl, not C: collections, the value algebra, the CBOR codec (with C leaves for integers and floats), channels, the space, timers, the supervisor
and the gateway bridge are all nsl source. C carries data, nsl carries policy.

### 2.1 Three kinds of C, and the line between them

Three different things are easy to run together, because each is "write some C". They differ in what changes and in
what has to be rebuilt:

| Kind | What changes | What is rebuilt | Who |
|---|---|---|---|
| **A primitive** (the closed list) | one name appended to the list, one C function, one kernel method that hands the device out through `platform` | the firmware — a reflash | the firmware's author |
| **A native method** (`cborWalk = native cborList`) | the *binding* of an existing selector, plus one primitive as its entry; the callers, the interpreter and the image format do not change; the nsl walk stays, as the oracle C falls back to | the firmware | the firmware's author |
| **nsl source — the application or the kernel** | the `.nsl` | the image only: a whole-program bake by the host compiler, loaded beside the firmware | anyone |

A primitive is a selector on a closed list, 160 of them today, each backed by one C function, as the serial device was
added [Serial]. The change is additive: nothing already on the list moves, and no app can add a primitive of its own.
The companion paper walks one such binding through every layer, for an e-paper display on a MagTag [Embedded].

A native method rests on the fact that every operation is a send through a vtable, so a method is a binding from a
selector to a closure, and whether that closure is interpreted or native is one field. To move a hot method to C,
write the C and bind it. A spike did this for the Towers of Hanoi on an ESP32, and the native method ran 116–119 times
faster than the interpreted one at two problem sizes [Hanoi]. That is a best-case ceiling — all the work in one
method, the recursion fully native — not a steady-state figure for a whole program. The spike ran. The use that would
make it a production mechanism, a native walk for the CBOR encoder, is designed on a branch (rete #297) and not
built; the points below about declared shapes, refusal and the unsendable selector are that design's, not measured
behaviour.

**The boundaries.** What a native may touch, and what it may not:

- C never names an nsl class. A native is found by asking the receiver's method table for the binding, so List, Map
  and Record are not named in C. The alternative, C comparing against the classes it is passed, would hard-code class
  identity in C (rete #297, D1).
- A native declares, in its registry row, the instance variables its C reads, in order. The image build refuses,
  naming the native and the class, an image where the class's declaration does not match. A shape change is therefore
  a registry edit.
- The C walk refuses anything it does not recognise (another binding, nesting deeper than 32, a NaN) by answering one
  sentinel. The nsl walk, kept as the oracle, then produces the exact bytes or the exact error, so error texts and
  edge cases stay defined in one place.
- A native's selector cannot be sent from nsl. The single entry is one new primitive, so a native costs one row on
  the closed list as well as its binding.
- The image never contains C. A primitive or a native is C compiled into the firmware, and the image refers to it by
  a registry reference.
- "Recompiling all source" is the ordinary image build: every program is a whole-program bake of the kernel plus the
  application, so an nsl change to the kernel costs the same as one to the application. The costly rebuild is the
  firmware, and only C triggers it.

**Why there is no FFI.** Four reasons. An FFI is ambient authority, and a module's authority is meant to be visible in
its header (§3). C that holds an object reference across an allocation is the classic collector bug; with a closed
list every such site is reviewed once, in the VM, rather than in each application. A read-only image executed in
place has no dynamic linker to resolve a call through. And an agent cannot wander into C to get around the first two.
The cost is plain: someone must write the C, and moving a hot method to C is a firmware change, not an application
change. The Smalltalk that OTI shipped to embedded targets in the 1990s had the easy C call and "fixed" non-moving
objects for C to reach [Clo96]; nsl drops both on purpose.

## 3. Capabilities

nsl follows the object-capability rule: authority is holding a reference, and there is no ambient authority to fall
back on. A program's `main:` is handed one object, `platform`, and everything it can name — library classes, devices,
the space — it reaches through objects it was handed. That is why a program writes `platform space`,
`platform timers` and `platform Record`: there is no other way to reach them. `platform` is a capability, not a
namespace. The structural half of this comes from Newspeak; what nsl adds is that the capability objects are few
and their protocol is closed (§5).

**Authority is visible in a header.** A module that is given a device says so where the class is declared, and a
class that was not given one cannot name it. The suite pins this with a test that declares only
`usingPlatform: platform` and then tries to use the serial device [Confined]:

```
class SerialConfinedTest usingPlatform: platform = () (
  public main: platform2 args: args = (
    serial write: 'oops'.
```

The compiler accepts it, because `serial` is, to the compiler, an unknown unary send. At run time the send finds no
such name and aborts with "Message 'serial' not understood by receiver". The test's own comment calls the failure
the passing evidence. A device is a separate grant in the module header (`serial:` and `net:`, ledger rows 13 and 17
[Ledger]), so a module given only `net` can use the network and nothing else.

**A worked example of attenuation.** The space (§4) is reached through channels, and a channel is a capability: an
object holding a root path prefix. Every operation on it resolves its argument path against that root, and the only
ways to obtain a channel are the module's own `space`, whose root is empty, and `channel:`, which answers a child
whose root is the parent's root plus a validated suffix. There is no accessor for the root and no widening operation
[Kernel]. The suite's attenuation test derives a chain and then probes it [Attenuation]:

```
space:: platform space.
capA:: space channel: 'a'.
capAB:: capA channel: 'b'.
capABC:: capAB channel: 'c'.
capABC out: '' value: 5.
(capABC take: '' ifEmpty: [ nil ]) printString out.
(((capAB channel: 'other') take: '' ifEmpty: [ nil ]) == nil) printString out.
(((capA channel: 'x') take: '' ifEmpty: [ nil ]) == nil) printString out.
```

The test expects `5`, `true`, `true`. The holder of `capABC` addresses exactly `a/b/c` and below, and reads back
what it wrote. Asking `capAB`, which is the leaf's parent, for a sibling path, `a/b/other`, finds nothing; asking
`capA` for a sibling of the chain, `a/x`, finds nothing either. Matching is by whole path segment, so a channel
rooted at `sensor` does not address `sensors/temp` [Kernel]. A subsystem can be handed the narrowest channel it needs
and cannot address anything else. That is the whole of the security argument for the space, and it is small enough to
read in one sitting.

**The mirror is confined.** Reflection — walking a stack, reading a frame, hot-swapping a method — is not part of
`Object`. It is reachable only through a `mirror` object, and the reflectee is always an argument, never the receiver.
Only the Supervisor holds one (ledger rows 15, 16), so application code cannot name it (§7).

**Where `platform` comes from, and how to narrow it.** `platform` is not declared anywhere a program can see. The
Supervisor builds the kernel instance once and passes it as the first argument of the program's `main:`; a module that
needs it names it in its header (`usingPlatform: platform`), and a class nested inside the program cannot see it unless
it is handed it, because it is a method argument, not a slot. Narrowing it therefore needs no language feature: it is
an object that holds `platform` and answers only what it chooses to hand on. A suite test [Narrowed] does exactly that:

```
class SpaceOnly on: platform = (
| private p = platform. |
)(
  public space = ( ^ p space channel: 'sensor' )   (* only the space, and only under sensor/ *)
)

class Sensor given: cap = (
| private cap_ = cap. |
)(
  public report: v = ( cap_ space out: 'temp' value: v )
  public reachForTimers = ( ^ cap_ timers )
)

public main: platform args: args = (
  | sensor |
  sensor:: Sensor given: (SpaceOnly on: platform).
  sensor report: 21.                                  (* lands at sensor/temp *)
  sensor reachForTimers.                              (* aborts: 'timers' not understood *)
  ^ 0
)
```

The write lands at `sensor/temp`, and the reach for the timers aborts with "Message 'timers' not understood by receiver
a SpaceOnly". Giving `SpaceOnly` a `timers` method makes the test fail, which is the check that the test can fail.
Two things this does not do. It is a convention, not an enforcement: `main:` still holds the full `platform`, and
nothing stops it from handing that out instead. And a facade costs one extra send per access.

**What is not yet true.** Today `platform` is one object, the whole kernel instance, which hands out every device and
library class (ledger row 32). Only the space attenuates; the confinement that is real is lexical, because a nested
class without `platform` reaches nothing, plus the space's prefix rule. Any code that holds `platform` holds every
device the image was built with. That is a convenience of the first release, not part of the design: since `platform`
is only an object, a narrower one can be handed down, and a module that receives only the display cannot touch the
radio. For the same reason a different `platform` could carry different implementations, collections tuned for size
rather than speed or a simulated device in place of a real one, without a line of the program changing. The language
allows this; version 0.1 does not use it, and has one kernel. The consequence for a reader deciding what an agent's
code can touch is in the companion paper [Embedded].

## 4. A reactor over a space

Zephyr is multithreaded; nsl is not. The VM runs on one Zephyr thread and, inside nsl, there is one reaction at a time,
each run to completion.

State lives in a **space**, nsl's descendant of the tuple spaces of Linda, simplified: a value is found by its
name, not by matching its contents. The names are paths, and a node is programmed against names, not addresses. A
*cell* holds the latest value; a *stream* is append-only. The paths a node shares with its gateway follow a convention:
`goal/…` is intent, written by the gateway; `obs/…` is the node's answer; `tel/…` is a stream of rows; paths under
`sys/` stay on the node, and carry its timers. Every path that crosses the wire has its kind and its record shape
declared once, in the protocol's manifest, and the gateway refuses a write that disagrees [Manifest].

A program registers a reaction with `whenever:do:`; a write to a path queues the reactions that care, and the reactor
runs them one by one. Timers post from their interrupt to a mailbox, and the reactor blocks on it when there is
nothing to do [VM]. The gateway sees the same names: porta mirrors parts of each node's space, so `goal/name` written
at the gateway arrives as a write in the node's space, and no part of the program knows which side of the radio a
value came from.

**Why not actors or processes**, the usual answers. A reaction cannot be interrupted by another, so state shared
between reactions needs no protection, and the order of reactions is a function of the order of writes. That is the
determinism the language wants, and a process model would forfeit it. Here there is one queue and one stack, which is why the July 2026
rewrite that removed fibers removed their per-fiber stacks with them [MA]. And a failed reaction is a value, not a
dead process: the supervisor catches it, keeps the heap and the reactor running, and restarts or quarantines the job
[MD]. The cost is the one every event loop has: a long computation starves the timers, and the answer is to split it
across reactions. The mechanism that notices a stall, the practical rules for writing reactions, and the event flow
from interrupt to reaction are the companion paper's [Embedded].

**Power modes, as semantics.** A node's power mode is a value at a path, written by the gateway and acknowledged by
the node — not a command [Porta]. An *always-on* node runs a loop that never returns. A *deep-sleep* node wakes,
takes its reading, exchanges with the gateway and sleeps again; `platform deepSleep:` never returns, and the next wake
is a full reboot. Its program is declared *run-once*, because a loop that expected to keep running would be killed by
each sleep. Deep sleep runs on the MagTag today. What makes it work is that every exchange is started by the node: a
sleeping node has no address to be called on, so the gateway holds what it wants — a new setting, a new program —
until the node next wakes and asks.

## 5. The dialect against Newspeak

![What drives each divergence](img/nsl-divergence-drivers.svg)

_Figure 2. Each group of divergences from Newspeak against the reason its record gives. Numbers are rows of the
dialect ledger [Ledger]._

### 5.1 Kept

- **No global namespace.** Every name is reached through an object handed in: `platform List`, `platform space`. A
  class that was not given `platform` cannot reach the library at all (ledger rows 8, 9).
- **Modules are objects.** `class App packageUsing: manifest = (…) ( public main: platform args: args = (…) )`; the
  module is an instance parameterised by what it was given.
- **Nested classes and implicit-receiver sends**, with Newspeak's lookup order (lexical first), so an upgraded
  superclass cannot capture a name the enclosing module defines.
- **Slots with `::=` (mutable) and `=` (immutable), `::` assignment, keyword selectors, blocks, class-side bodies.**

### 5.2 Changed or removed

| Area | Newspeak | nsl | Primary driver |
|---|---|---|---|
| Class references | virtual (late-bound slots) | static, resolved at compile time (row 1) | no JIT: production Newspeak makes virtual lookup free on Truffle/Graal |
| Composition | mixin application | single inheritance, one `delegate` (row 2) | as above; mixins are how virtual classes compose |
| Non-local return | `^` from any block unwinds to its home | compile error from an escaping closure (row 21) | simplicity: non-local return forced the reify/unwind/cross-fiber cascade |
| Concurrency | actors / processes | a run-to-completion reactor over a tuple space (row 22) | the same rewrite; determinism, predictable RAM |
| Exceptions | class hierarchy, resumable | `Record {#tag,#text}` values, predicate `on:do:`, `ensure:`, terminate-only (row 24) | errors cross a channel and the wire, so they must be values |
| Reflection | ambient | a `mirror` capability held only by the supervisor (rows 15, 16); no `class`, `perform:`, `thisContext` (row 26) | capability security; a closed-world image |
| Integers | unbounded | one type, ±(2^64−1), overflow traps (row 29) | one range for host, board and CBOR |
| Strings | collections of characters | immutable UTF-8 bytes, no `Character` (row 10) | one allocation, a GC leaf |
| Symbols | interned at run time | interned at compile time only (row 30) | the image is read-only |
| Methods | recompiled in place | read-only image; a debug build can override a method from RAM and restart its frame (row 31) | the image lives in flash |
| Syntax | cascades, literal arrays, `$a`, radix numbers | a 109-line grammar without them (row 27), given in full in the Appendix | simplicity |
| Access modifiers | enforced | parsed, not enforced (row 25) | **undecided** — a finding of the 2026-10-06 audit |

### 5.3 Why most of this is reversible

The structural half (no globals, modules as objects, confined privilege) was paid for first because it cannot be
retrofitted, and most subtractions are additive to restore: static binding is the special case of late binding in
which no class slot is overridden, so nothing written for nsl becomes wrong if virtual classes come back. The
exceptions are rows 21–24: restoring non-local return, processes or resumption would bring back the machinery the
rewrite deleted, and they are not on any plan.

## 6. Against Smalltalk-80 and Pharo

nsl's ancestry runs through Smalltalk — this system has been rewritten wholesale four times on the way here — and a
Smalltalk programmer reads it easily. The differences are mostly about *where authority and state live*, not about
expression.

| | Smalltalk-80 / Pharo | nsl |
|---|---|---|
| Everything is an object; computation is message sends | yes | yes — including arithmetic, which falls back to a send off the integer fast path |
| Keyword selectors, blocks as closures | yes | yes |
| Control flow | `ifTrue:` etc. are messages, inlined by the compiler | the same, and the inlining is now the only implementation (row 19) |
| Namespace | one global `Smalltalk` dictionary; any method names any class | none; everything through `platform` (rows 8, 9) |
| The image | a live, mutable heap you develop in and ship | a frozen image built from source; the VM runs it from flash |
| `thisContext`, `become:`, `perform:`, `instVarAt:` | yes, ambient | no; frame access only through the confined mirror (rows 15, 26) |
| Processes and semaphores | yes | no; reactions that run to completion (row 22) |
| Non-local return | yes | only from inlined blocks (row 21) |
| Exceptions | class-based, resumable | value-based, predicate-filtered, terminate-only (row 24) |
| Class extension (adding methods to `Object`) | yes, from any package | no; the kernel is one module, and an app cannot reach its classes to extend them |
| Debugging | in-image, by anyone | over a line protocol to the confined debugger, from a host (§7) |

What nsl takes from Newspeak rather than Smalltalk is the point of it: a module cannot reach anything it was not
given, and that is what makes a capability *mean* something. What it gives up relative to Smalltalk is liveness: the
system is developed on a host and deployed as an image, and hot patching is a debugging act, not the way programs
are built.

## 7. Liveness given up, and the part won back

Smalltalk's best property is that the program and the tool are the same live heap. nsl gives that up, and the reason is
physical: the image is a read-only blob in flash on a board and a read-only mapping on the host, so a method cannot be
recompiled in place. Programs are built from source on a host, run first on the host VM, and then loaded as a whole
image beside the firmware. Editing a running method is a debugging act, and it exists only in a debug build.

**A method overridden from RAM, and a frame restarted.** Because the image's vtables cannot be written, a replacement
goes into a table in RAM that is consulted before a vtable's own bindings, at each level of the delegate walk;
installing one flushes the method cache, and the collector roots it (ledger row 31). The replacement is passed in as
text, as one debug line, the same on the desktop and over a board's cable. Replacing the method is not enough: the
stopped frame is still running the old code, and the spike shows it printing the old result until it is restarted. So
a second request restarts that frame from its first instruction, keeping the receiver and arguments and clearing every
other register. The interpreter applies the request only when the debugger hook returns, because only then is the
stopped frame the top of the stack and can grow in place if the new method needs more registers [Patch]. The spike ran
on the desktop and on one ESP32-C6, once each, and was falsified by disabling the restart. It is a demonstration, not a
survey of patches.

**What it cannot do.** The new method may use only constants the running image already holds: no new symbol, string,
class or float, because symbols are interned only at compile time (row 30). It cannot make a block, and only the
stopped frame restarts. Each is refused by name. The mechanism is compiled out of a standard image, which is a
decision, not an accident: hot patching is a debug facility. In the hooks build it raised the board's code from 40 871 B to 43 244 B (2.4 KB), and
that turned the footprint gate red [Patch].

**The debugger is a mirror held by the Supervisor.** Reflection is not on `Object`; it lives in a `mirror` capability
(row 15), and the only holder is the Supervisor, which carries the debugger's facade nested inside its own class
(row 16). Application code cannot name a mirror, so a debugger adds no ambient authority. Breakpoints are a table of
method and offset matched at the top of the interpreter loop, which fires a hook written in nsl; they do not patch
bytecode, because the methods sit in read-only memory. A second mirror implementation, for a remote or post-mortem
debugger, needs no change of protocol, because the reflectee is always an argument.

The workflow built on these mechanisms, the verbs, their HTTP calls and the remote case, belongs to the companion
paper [Embedded].

## 8. What makes a language fit for an agent

The question here is whether the language gives an agent something to work with, not whether its output can be
trusted; the companion paper owns that, and the survey of how Zephyr's contributors regard AI-written code, in its §1
and the loop an agent runs in its §9 [Embedded]. The argument, with what is measured and what is not:

1. **Keyword selectors name every argument at the call site.** `aSocket sendTo: host port: p data: bytes` cannot be
   called with the port and the host swapped without the code saying so; `sendto(fd, buf, len, 0, addr, alen)` can.
   The selector is the documentation, and it is the part of the code an agent pattern-matches on. *Not measured*: we
   have no count of argument-order errors by agents in nsl versus C or Python.
2. **The whole language fits in a context window.** Measured 2026-10-06 (bytes; tokens estimated at 3.5 bytes per
   token, not counted by a tokenizer):

   | Piece | Bytes | ≈ tokens |
   |---|---:|---:|
   | Grammar (109 lines) | 3 416 | 1 000 |
   | User's guide [Guide] | 27 385 | 7 800 |
   | Dialect ledger [Ledger] | 36 374 | 10 400 |
   | Kernel's public selectors (438 signatures) | 6 397 | 1 800 |
   | Kernel source without comments | 91 815 | 26 200 |
   | Kernel source as written | 247 689 | 70 800 |

   Grammar, guide, ledger and the comment-free kernel come to about 160 KB, roughly 45 000 tokens: the complete
   language, standard library included, fits a 200 k window with room for the program, and the grammar plus guide plus
   public selectors fit easily in the 64 k window of a local model. A Python or C agent works from memory of a library
   it cannot see; an nsl agent can be given all of it.
3. **Authority is visible in signatures** (§3), so a reviewer of an agent's work can read what a piece of code is able
   to touch without reading its body. It is weakened today by the all-powerful `platform`.
4. **A small, closed primitive set.** 160 primitives, all other code is nsl; there is no FFI to wander into (§2).
5. **Errors written for a reader that is an agent.** A message not understood names the receiver's class and the
   nearest selector it does understand; about 1 KB of flash is spent on that on purpose, because "an operator that is
   an agent is exactly as good as the error line it is fed".

**Against it, honestly.** Dynamic typing gives an agent nothing at compile time: a misspelt selector is found when it
runs, which is why item 5 exists. nsl is not in any model's training data, so an agent writes it from the context it
is given, not from habit — which is the bet item 2 makes and has not yet tested. What would test it: the same small
board tasks given to an agent in nsl (with the language in context) and in Zephyr C, scored on first-run correctness.
We will run that comparison and publish the result: shown, not told (rete issue #373).

## 9. Costs, and what would lift each

| Cost | Felt as | Would lift it | Ledger row |
|---|---|---|---|
| No virtual classes or mixins | a library cannot be specialised per target except by choosing a module at boot | restore late-bound class slots on the same vtable substrate | 1, 2, 6 |
| No non-local return | loops that would `^` out of a `do:` are rewritten as `detect:ifNone:` or a flag | not planned | 21 |
| No processes | a long computation starves timers | split it across reactions; the liveness observer shows the stall | 22 |
| No resumable exceptions | a handler cannot supply a value and continue at the raise | `on:do:` answering the default | 24 |
| Per-byte work is interpreted | the CBOR codec is nsl, and every value it walks costs method activations | move the hot methods to C by native substitution (§2); the encode walk is designed on a branch for C (rete #297), not built | — |
| `platform` is all-powerful | any app holds every device | narrowed platforms, or `serial:`/`net:`-style header grants for devices | 32 |
| Access modifiers unenforced | `private` documents intent and nothing more | emit them and check, or delete the check (#372) | 25 |
| Hot patch limited to known constants | a fix that names a new selector or string cannot be patched | intern in RAM; carry offsets in the image map | 30, 31 |
| No `Character`, byte strings | text is bytes | a rune-aware protocol | 10, 12 |

The costs of running this on a Zephyr part, set against C, are the companion paper's [Embedded].

<div style="page-break-before: always"></div>

## Appendix. The grammar, in full

It is the Ohm grammar [Grammar], 109 lines, and the dialect's best evidence of its own size.

```
NsLite {
  Program  = ClassDef+                -- module
           | Temps? Statements         -- script

  ClassDef    = visibility? classKw identifier CtorSig "=" SuperClause? ClassBody (":" ClassSide)?
  ClassBody   = "(" SlotSection? Statements ")" "(" Member* ")"
  ClassSide
    = ClassBody             -- nested
    | "(" Member* ")"       -- flat
  Member      = ClassDef | MethodDef
  // A superclass clause is a class-valued expression: a head identifier
  // (a class name, or `super`/`self`/`outer`) followed by unary messages that
  // navigate to a nested class, then an optional keyword message that is the
  // parent's initializer (superInit). E.g. `SuperHierarchy`,
  // `super Inner1 new: x`, `outer InnerHierarchy Inner1 new: x`, `self Inner0 new: x`.
  // The leading identifier (never `(`) keeps this unambiguous against the
  // ClassBody that follows.
  SuperClause = identifier unaryMessage* SuperInit?
  SuperInit
    = (keyword BinarySend)+ -- kw
  classKw     = "class" ~identifierPart
  CtorSig
    = unarySelector         -- un
    | KwPair*               -- kw
  KwPair      = keyword identifier
  SlotSection = "|" SlotDecl* "|"
  SlotDecl    = visibility? identifier SlotInit? "."?
  SlotInit    = slotOp Expression
  slotOp      = "::=" | "="
  visibility  = ("public" | "private" | "protected") ~identifierPart
  MethodDef   = visibility? MethodPattern "=" "(" Temps? Statements ")"
  MethodPattern
    = KwPair+               -- kw
    | binaryOp identifier   -- bin
    | unarySelector         -- un
  unarySelector = identifier ~":"

  Temps    = "|" TempDecl* "|"
  TempDecl = identifier SlotInit? "."?
  Statements = ListOf<Statement, ".">  "."?
  Statement  = Return | Expression
  Return     = "^" Expression

  Expression = Assignment

  Assignment
    = identifier assignOp Expression    -- assign
    | KeywordSend                       -- pass

  assignOp = "::" ~"="

  KeywordSend
    = BinarySend (keyword BinarySend)+  -- kw
    | (keyword BinarySend)+             -- implicitKw
    | BinarySend                        -- pass

  BinarySend
    = BinarySend binaryOp UnarySend     -- bin
    | UnarySend                         -- pass

  UnarySend
    = UnarySend unaryMessage            -- un
    | Primary                           -- pass

  unaryMessage = identifier ~":"

  Primary
    = "(" Expression ")"                -- paren
    | Block
    | DynArray
    | Literal
    | identifier                        -- var

  DynArray    = "{" ListOf<Expression, "."> "."? "}"

  Block       = "[" BlockParams? Temps? Statements "]"
  BlockParams = blockParam+ "|"
  blockParam  = ":" identifier

  Literal
    = float
    | integer
    | string
    | symbol
    | trueKw   -- true
    | falseKw  -- false
    | nilKw    -- nil

  trueKw  = "true"  ~identifierPart
  falseKw = "false" ~identifierPart
  nilKw   = "nil"   ~identifierPart

  float   = "-"? digit+ "." digit+
  integer = "-"? digit+
  string  = "'" (~"'" any | "''")* "'"
  symbol  = "#" symbolName
  symbolName = keyword+ | binaryOp | identifier

  keyword    = identifier ":"
  identifier = identifierStart identifierPart*
  identifierStart = letter | "_"
  identifierPart  = letter | digit | "_"

  binaryOp   = binaryChar+
  binaryChar = "+" | "-" | "*" | "/" | "\\" | "~" | "<" | ">" | "=" | "&" | "|" | "@" | "%" | "," | "?" | "!"

  space    += comment
  comment   = "(*" (~"*)" any)* "*)"
}
```

<div style="page-break-before: always"></div>

## References

- **[ZT10]** B. Trinkenreich, A. Lawson and H. Carter, *Zephyr Turns 10: A Decade of Adoption, Maturity, and Ecosystem
  Evolution*, The Linux Foundation, March 2026, CC BY-ND 4.0 —
  [copy in this repository](../../../docs/ZephyrTurns10_Report_Revised_051826.pdf). 900+ boards; the learning curve.
- **[Embedded]** [Objects on a 256 KB radio node: remote debug, profiling and agent-written code over Zephyr](nsl-embedded-paper.md),
  the companion paper, for the embedded developer.
- **[OEOM]** I. Piumarta and A. Warth, *Open, Extensible Object Models* (2007).
- G. Bracha et al., *Modules as Objects in Newspeak* (ECOOP 2010); G. Bracha and D. Ungar, *Mirrors: Design
  Principles for Meta-level Facilities of Object-Oriented Programming Languages* (OOPSLA 2004); A. Goldberg and D.
  Robson, *Smalltalk-80: The Language and its Implementation* (1983).
- **[Ledger]** [The nsl dialect ledger](NEWSPEAK_DIALECT.md): every divergence from Newspeak, its reason and what
  would revisit it (rows 1–32, audited 2026-10-06).
- **[Guide]** [Programming with nsl](programming_with_nsl.md), the user's guide.
- **[Grammar]** [`ns.ohm`](../grammar/ns.ohm), the Ohm grammar of the dialect.
- **[VM]** [oevm — what the VM is](../../oevm/docs/vm.md).
- **[Pivot]** [The July 2026 rewrite's design](design/2026-07-03-nsl-tuvm-first-step-design.md): no
  non-local return, no fibers, the reactor; the 256 KB target.
- **[MA]**, **[MD]** [Milestone A](../../oevm/docs/MILESTONE_A.md) (what the rewrite deleted, measured) and
  [Milestone D](../../oevm/docs/MILESTONE_D.md) (the supervisor).
- **[Boards]** The board profiles in [`run.sh`](../../oevm/zephyr/run.sh) and
  [the board ports](../../oevm/zephyr/docs/BOARD_PORTS.md); the nRF52840 RAM split from that build's linker symbols.
- **[RP2350]** [First numbers on the RP2350](../../../docs/results/2026-10-01-rp2350-first-numbers.md).
- **[Image]** [The image and its map](../../../protocol/IMAGE-ARTIFACTS.md).
- **[Porta]** [porta's architecture](../../../porta/docs/ARCHITECTURE.md), §5: power modes and app lifecycle.
- **[Serial]** [Adding the serial device](design/2026-06-22-ns-oevm-stdlib-step3c3b-serial-design.md): a device
  target is purely additive.
- **[Clo96]** K. Clohessy, *Using object-oriented programming tools to build real-time embedded systems*,
  Real-Time Engineering, Fall 1996.
- **[Hanoi]** [Native substitution, the Hanoi spike](../../../docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md).
- **[Peer]** [custos, the developer's peer](../../../docs/design/2026-10-04-custos-peer-design.md): agent first
  (D11–D13).
- **[Patch]** [Stop, patch, restart, resume](../../../docs/results/2026-10-06-313-patch-restart-spike.md).
- **[Manifest]** [`MANIFEST.cddl`](../../../protocol/MANIFEST.cddl), the normative catalogue of paths, kinds and
  shapes.
- **[Kernel]** [`kernel.nsl`](../lib/kernel.nsl), the comment on `Channel`: a rooted-prefix capability, `channel:` as
  monotonic attenuation.
- **[Narrowed]** [`67_narrowed_platform.nsl`](../../tests/04_somns_basic/67_narrowed_platform.nsl): a narrowed platform as a facade, and the reach it refuses.
- **[Attenuation]** [`03_attenuation.nsl`](../../tests/09_channels/03_attenuation.nsl), the suite's attenuation test,
  quoted in §3.
- **[Confined]** [`39_serial_confined.nsl`](../../tests/04_somns_basic/39_serial_confined.nsl), the confinement pin,
  quoted in §3.
