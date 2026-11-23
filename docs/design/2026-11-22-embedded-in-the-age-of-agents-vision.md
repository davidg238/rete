# Embedded programming in an age of agents — a vision, and where rete stands against it

**Status: DRAFT, 2026-11-22. A brainstorm written up at David's request; nothing here is decided, built or
scheduled. No issue tracks it yet.** It proposes no mechanism. Where it names work, that work is either an
existing issue (cited) or labelled **not designed**.

**Sources, and their limits.** Read on 2026-11-22: the Newspeak home page (its "Vision" section), the paper and
talk listings, and the Ampleforth paper (Bracha, Live 22). **Not read**: "Modules as Objects in Newspeak"
(ECOOP 2010), "The Newspeak Programming Platform" (2008) and the Croquet synchronization draft — the PDFs did
not parse. What §2 says about modules, mirrors and serviced objects comes from recall of those papers, not from
a reading on that day; check it against the papers before quoting it outside this repository. What §4 says
about rete comes from [limits.md](../limits.md), [overview.md](../overview.md) and
[vm.md](../../nodus/oevm/docs/vm.md).

## 1. The constraint

An embedded device differs from the systems Newspeak was designed for in four ways (David, 2026-11-22):

1. it lives a long time — ten years or more;
2. it runs without a person watching it;
3. it must be secure;
4. it is **not** a dynamic, interactive environment: it must be robust and understandable.

And three things are wanted of it: it is monitored and updated remotely; it gathers enough information to catch
problems, and early in a design, to measure performance; and in development it can be debugged remotely.

## 2. What of Newspeak carries over

| Newspeak idea | Read for an embedded device | In rete today |
|---|---|---|
| No global namespace; a module is a class handed its platform as an argument | What a program can touch is written in its header, so a reviewer checks authority rather than every line | Built: a capability is a rooted path |
| Mirrors: reflection is a separate object, so it can be withheld | Debug, patch and profile are capabilities offered over the wire, to whoever is entitled | The verbs are built; they are gated by which image was flashed, not by who is asking |
| Serviced objects: a program kept in step with a server, updated while in use, working offline | A node nobody can reach for ten years | Built in part: `goal/**` and `obs/**` converge, program images are signed and pushed, the outbox stores until acknowledged (#443) |
| Liveness: the IDE lives inside the running image | **Does not carry over.** The node stays frozen and small; what is live is the fleet as the gateway sees it | By decision: run-to-completion, no fibers ([vm.md](../../nodus/oevm/docs/vm.md)) |
| Pluggable types: optional checking that does not change what runs | Checked contracts at the seams between a program, its platform and the wire | Only the wire has one (`protocol/MANIFEST.cddl`) |

The third row is the one to notice. The Newspeak home page says of it "We're not there yet", and it describes a
device in the field more closely than the desktop application it was written for.

## 3. What agents change

When an agent writes the code, code is cheap. What stays expensive is the evidence that the code is right, and
the ability to change it safely, years later, by someone who did not write it. That moves the goal:

> A device you cannot reach can still be **seen, explained and changed**, for ten years, by a person or an agent
> who did not write its program.

Three consequences.

- **Small and whole beats fast.** A language an agent can hold entire (the README's figure for nsl is about
  45,000 tokens) can be re-understood in 2034 by a reader with no history. That is a longevity argument, not a
  convenience.
- **Every verb is a machine-readable answer first** (peer design D11–D12, already the rule here), and every
  change has a host twin to run on before it reaches a board.
- **Performance enters last, through evidence.** Run the interpreter, measure in the fleet, then replace the one
  method the measurements name with C (the native-substitution spike, 2026-07-17). Hand-tuning every line of a
  128 MHz part up front optimises the cheap thing.

The cost, stated first: an interpreter is slower and larger than C, the debug checks cost 10–16% per bytecode
([limits.md](../limits.md)), and none of the above has run unattended for even one year.

## 4. Where rete stands

**What is shown:** the mechanism fits. The language, the reactor, the supervisor, load, debug, patch, profile
and convergence run in the C6's arena (`OEVM_HEAP_SIZE`; [vm.md](../../nodus/oevm/docs/vm.md)).

**What is not shown:** the claim itself, which is about unattended years.

| Constraint (§1) | Gap | Record |
|---|---|---|
| Unsupervised | No watchdog on the nRF52840, ESP32-S2 or WROOM-32; one node sat wedged for about ten days | #526 |
| Unsupervised | Firmware replacement over the air with a fallback slot is decided, not built. Nothing confirms a new version healthy or reverts it | #449; **confirm-or-revert not designed** |
| Secure | Nothing is authenticated except a signed program image. Whoever can send a UDP datagram to a node can write `goal/**` and `dbg/req`. Nothing is encrypted; no key is ever replaced | [limits.md](../limits.md); [security levels](2026-10-02-security-levels-design.md) (draft) |
| Long-lived | "No compatibility is kept" is right while both ends are reflashed together, and it is the opposite of ten years. One small thing has to freeze — at the least the updater and its first exchange — so that everything above it stays replaceable | [frozen surface](2026-10-02-frozen-1.0-surface.md) (draft), #253 |
| Long-lived | The C6's Thread radio depends on four driver fixes carried on a branch | [overview.md](../overview.md) |
| Evidence | Profiling and debugging need a requested window or a separate image. Nothing records, always, the reason for a reboot, the arena's high-water mark, how long reactions take, or the last events before a wedge | **not designed** |
| Understandable | The run-time model is simple. The seams between a program and its platform are unchecked | **not designed** (§2, last row) |
| Proof | No measured rows against CircuitPython | #383 |

Security is the largest gap, and it bears on the mirror row of §2: a debugger that anyone on the network can
open is an authority, so "which image was flashed" cannot stay the gate.

## 5. What more RAM would buy

Most of §4 is not limited by memory. More RAM (the question was 512 KB) buys margin, and the proposal is to
spend it on evidence and isolation, not on features. **None of this is designed or measured.**

| Use | What it gives | What would have to be measured first |
|---|---|---|
| One image, not two | The debug checks stay in the production image, opened by an authenticated request rather than by a build | Whether the 10–16% and the image growth are acceptable on each board |
| A flight recorder | A ring of recent events and counters that survives a reboot | Its size per event, and where it lives across a reset |
| Shadow running | The old and new program resident together; the new one's outputs compared before it takes over | Whether two programs' live data fit, per app |
| An arena per job | One job's leak cannot starve another | #22 (attribution comes first) |
| An authenticated transport | Room for a session's state | Its RAM cost on the weakest board ([security levels](2026-10-02-security-levels-design.md) §9) |

**Kept off the node on purpose:** the compiler and the agent. Both belong at the gateway, with a host twin of
the node to try a change on.

## 6. Questions this leaves open

1. Is §3's sentence the goal? If so, which of §4's rows is the first to earn a design?
2. Does the mirror reading (§2, row 2) change the security-levels draft — one question, "who may hold the
   debugger", instead of a debug build?
3. What is the smallest thing that freezes for ten years (§4, row 4), and is it smaller than the 1.0 surface?
