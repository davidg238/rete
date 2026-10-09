# Splitting the nsl paper in two: outlines

_DRAFT, 2026-10-08, for iteration with David. Splits [the design paper](../nsl-design-paper.md) (draft 2, 5 489
words). Under each section is its intent and where its material comes from: "§N" means a section of draft 2, and
**new** means it has not been written yet._

## Why two papers

Draft 2 talks to two readers at once. A language reader (Newspeak, Smalltalk, PL) wants the divergences and why
they were made. An embedded reader (a Zephyr engineer) wants to know why they should run an interpreter at all, and
what it gets them that C does not. Each reader skims the other reader's half, so neither paper does its job.

**Rule: every topic has one owner.** The other paper gives it one paragraph and cites the owner. That way the two
papers cannot drift apart, as the drift #374 found already shows.

| Topic | Owner | The other paper gets |
|---|---|---|
| 256 KB, XIP, no JIT (the RAM tables) | L | E: one sentence and the 112 KB figure |
| The VM, primitives, native substitution | L | E: the driver-binding walk-through (App. B) as *your Zephyr driver, plus glue* |
| Capabilities, `platform` | L | E: what an agent's code can touch |
| Reactor and space: semantics | L | E: the reactor in practice — no locks, liveness observer, sleep |
| Newspeak, Smalltalk comparisons | L | E: nothing |
| Debug, patch, profile; remote/OTA | E | L: the language mechanism only (mirror, frame restart, row 31) |
| Agent first | split | L: why the *language* suits an agent. E: why you can *trust* what an agent wrote |
| Costs | split | L: the ledger costs. E: the costs compared with Zephyr C |

**Files.** Draft 2 becomes L, retitled, and keeps its path. E is a new file beside it:
`nodus/nsl/docs/nsl-embedded-paper.md`.

---

## Paper L — the language

**Working title:** *nsl: a Newspeak dialect in 112 KB — what it keeps, what it drops, and why*
**Reader:** someone who knows Newspeak or Smalltalk and is sceptical that a dialect this small is still Newspeak.
**Length target:** about 4 500 words, the size of draft 2 minus §6 and the appendices, plus the grammar.

0. **Abstract.** A Newspeak dialect over Zephyr, run in place from flash. It keeps the structure (modules as
   objects, no globals, capabilities) and drops most of the dynamism, each drop with its reason and its cost. It
   closes on what makes a language fit for an agent. _From the abstract, ¶2–3._
1. **The target.** A 256 KB part with a radio, 112 KB of arena, an interpreter with no JIT at ~12 µs per
   activation, and a read-only image. Simplicity was the mechanism and size was the target ([Pivot], [MA]).
   _§1 in full: both RAM tables._
2. **The VM.** It is built on OEOM: three primitive kinds, everything a send, 31 fixed-length instructions,
   contexts on the heap, and a fixed arena. The kernel is written in nsl, so C carries data and nsl carries
   policy. A device is reached through primitives, not an FFI. Native substitution swaps a method for C by changing
   one field. _§2, minus the MagTag photo, which moves to E._
3. **Capabilities.** `platform` is a capability, not a namespace. Authority is visible in a module's header, and a
   channel can only be narrowed. The mirror is confined to the supervisor. Today `platform` hands out everything
   (row 32), and the cost of that is stated. _§2 ¶3, expanded. **New:** a short worked example of attenuation._
4. **A reactor over a space.** The section covers Linda's lineage simplified to lookup by name, cells and streams,
   `whenever:do:`, and run to completion. It argues why not actors or processes. Liveness and power modes appear
   only as *semantics*: deep sleep is a reboot, and a program can be declared run-once. _§3, minus the operational
   detail, which goes to E §5._
5. **Against Newspeak.** What was kept, what was changed or removed, and why most of it is reversible. _§4 with
   Figure 3, unchanged._
6. **Against Smalltalk-80 and Pharo.** _§5, unchanged._
7. **Liveness given up, and the part won back.** The image is frozen, so development happens on a host. A debug
   build can override a method from RAM and restart its frame (row 31). The debugger is a mirror held by the
   supervisor. This is the *mechanism*; the workflow lives in E. _§5's last paragraph and part of §6.
   **New:** about 300 words on how restarting a frame works when an image is read-only._
8. **What makes a language fit for an agent.** Keyword selectors, the context-window table, the closed primitive
   set, error lines written for an agent, and the counter-argument (dynamic typing; no training data). It ends by
   citing E §9 for the trust argument and #373 for the measurement. _§7 items 1–5 and "Against it"._
9. **Costs, and what would lift each.** _§8's table, minus the rows that are about Zephyr C (those go to E §10)._
- **Appendix: the grammar**, all 109 lines. It is short, and it is the dialect's best evidence. **New** in the paper.

---

## Paper E — the embedded developer

**Working title:** *Objects on a 256 KB radio node: remote debug, profiling and agent-written code over Zephyr*
**Reader:** a Zephyr engineer who is stubborn, conservative and fluent in C, and who has used AI for code and does
not trust it. This paper concedes costs first and labels every piece that is designed but not built.
**Length target:** about 4 000 words, plus diagrams and code.

0. **Abstract.** Zephyr reaches 900+ boards, and its learning curve is the cost ([ZT10]). This paper asks what an
   object language on top buys you: no reflash to change a program, debugging and profiling from a host (and,
   designed, over the radio to a sleeping node), and a loop short enough to check what an agent wrote. _From the
   abstract, ¶1 and ¶3._
1. **The problem.** Three problems: the learning curve; field nodes you cannot reach with a probe; and the survey
   numbers on AI code ([ZT10] Fig. 35: 78 % do not trust it without heavy review). _§7 ¶1 plus the abstract ¶1.
   **New:** the field-node problem stated from the reader's side._
2. **Why objects, on a microcontroller?** It opens with what the reader already does: Zephyr's `struct device` with
   its API struct *is* dispatch through a vtable. A send makes that uniform. It also gives late binding, which is
   what native substitution and a swapped `platform` (a simulated device) rest on. Then the cost, stated first: about
   100× C per activation, so the radio, the crypto and the drivers stay in C, and nsl only makes decisions. _**New.**
   It draws on §1's interpreter paragraph and §2's native substitution (the Hanoi 116–119× ceiling, labelled as a
   ceiling)._
3. **The system at a glance.** **Block diagrams:** Figure 1 (fleet, desk, porta, custos), and **new** the node's
   stack: Zephyr, OpenThread and mbedTLS, the VM thread, the image partition, the liveness observer. The RAM split is
   one row, citing L §1. _Figure 1 and its caption from draft 2._
4. **What you write.** A short excerpt of the Thread node (its `whenever:do:` line and `tick`), with its five
   space paths; the whole node stays in an appendix. Then a driver: *your Zephyr driver, a 13-line kernel class, and
   four primitives*, shown with Figure 4 and the MagTag photo. _§3's path table, Figure 2, and Appendix B; Appendix A
   is cited._
5. **The reactor in practice.** No locks in nsl code; one stack; a failed reaction caught by the supervisor; the
   liveness observer, which dumps a wedged node, reboots it and leaves a note saying why. Always-on or deep sleep, and
   why every exchange is started by the node. **New diagram:** the event flow, from ISR to mailbox to reactor to
   reactions. _§3's operational half._
6. **Develop without reflashing.** A program runs first on the VM on the host (the same program, with a host board
   layer), then is loaded onto the device and reports its hash. **New code sample:** the host run, then `load` as
   one HTTP call with its JSON answer. *Not measured:* the loop's time end to end, and it is labelled so.
7. **Debug, patch and profile over USB.** The table of verbs; every artefact names a source line. The patch demo on
   the C6 (2026-10-06); profiling at 1.3 % cost. **New code sample:** a breakpoint hit, as JSON, with the stack and
   named locals. _§6._
8. **Remote: through the gateway, to a node that is asleep.** D19: the breakpoint is held at porta until the node
   wakes, and a stopped node stays awake while it is stepped. Two kinds of sleep: the VM stays alive, or every wake is
   a cold boot. The host spike's numbers: stop reported in one cadence; 3–4 ms per command while held. The honest
   costs: the debugger polls at 99.9 % CPU, and porta's store grew ~4 MB in a 39 s hold. Program OTA (#233).
   **Labelled designed, not built** until #375–#377 and the C6 demo (#380) run. **New sequence diagram:**
   developer, custos, porta, the sleeping node. _§6's last paragraph and
   [the spike result](../../../../docs/results/2026-10-07-342-sleepy-node-debug-spike.md)._
9. **Trusting code an agent wrote.** The problem is verification, and an agent verifies by running things. What
   bounds its code: authority in the header and a closed primitive set (both owned by L, one paragraph here), and
   there is no FFI. What lets the agent check its own work: every verb is an HTTP call that answers in data, and every
   crash or sample names a line. **New diagram:** the agent loop (write → host run → load → probe → read). What is
   *not* yet true: `platform` is all-powerful (row 32), and #373 has not been run. _§7 items 3, 6, 7 and the
   ¶2 framing._
10. **Costs compared with Zephyr C.** Speed per byte; RAM given to the arena; no compile-time types; a language in
    no model's training data; one kernel; tooling at version 0.1; a single maintainer. Each row says what would lift
    it. _**New**, with a few rows taken from §8._
- **Appendix A: a Thread node in 36 lines** (draft 2's Appendix A, unchanged).
- **Appendix B: the agent's HTTP surface**, one line per verb. _**New**, from the peer design D11–D13._

---

## What this needs before drafting

| Item | Paper | State |
|---|---|---|
| Node stack block diagram | E §3 | to draw (SVG, rendered and read) |
| Reactor event-flow diagram | E §5 | to draw |
| Remote-debug sequence diagram | E §8 | to draw |
| Agent loop diagram | E §9 | to draw |
| JSON samples for `load` and a breakpoint stop | E §6–7 | capture from custos on `c6spare` (not `c6bench`) |
| Retitle draft 2 and cut §6/the appendices | L | after these outlines are agreed |

**Decided (David, 2026-10-08):**
1. Agent first is split by question: L asks whether the language suits an agent, E whether you can trust what one wrote.
2. The Thread node stays an appendix in E; the body quotes an excerpt.
3. L carries the full grammar as an appendix.
