# nsl: a Newspeak dialect for Zephyr — what it keeps, what it drops, and why

**and the case for an agent-first OO language**

_DRAFT, 2026-10-06, for iteration with David. The record each claim stands on is cited inline; where a number lives
in a file, the file is cited rather than the number restated. Render to PDF later with
`pandoc nsl-design-paper.md -o nsl-design-paper.pdf` if a PDF is wanted; Markdown is the source._

Companion pieces: the row-by-row ledger [`NEWSPEAK_DIALECT.md`](NEWSPEAK_DIALECT.md) (every divergence, its reason
and its revisit trigger), the user's guide [`programming_with_nsl.md`](programming_with_nsl.md), and the VM's
one-page description [`nodus/oevm/docs/vm.md`](../../oevm/docs/vm.md).

## Abstract

Embedded VMs used to live or die by porting: many RTOSes, each with its own drivers, network stack and security
story, and a VM had to follow them all. Zephyr's reach changes that: a VM ported once can reach more than 900
boards, with connectivity, security and drivers built in — Thread among them, more work than a small project could
build alone. Zephyr's cost is the learning curve, which its own community names its most persistent challenge
[ZT10].

nsl is a Newspeak dialect that puts an approachable object-capability language over Zephyr. Zephyr carries the
freight, nsl directs traffic: the radio, the crypto and the drivers stay in C, and the interpreter pays only for
decisions. A host compiler produces a read-only image that a small C VM runs in place from flash, in the 48 KB arena
left on a 256 KB nRF52840 after Zephyr, OpenThread and mbedTLS. nsl keeps Newspeak's structure — modules as
objects, no globals, capabilities as the unit of authority — and drops most of its dynamism, each drop with its
reason and its cost.

Last, the case for an agent-first OO language. Agents can't smell trouble, but they can find patterns in data, so
every tool answers in data: the whole language fits in a context window, every development verb is a
machine-readable HTTP call, every profile sample or crash names a source line, and the loop is short — the host VM
first, then a device load, not a reflash. Zephyr's contributors don't trust AI-generated code without heavy review
[ZT10]; nsl bets that a fast, repeatable loop supplements that review.

## 1. The constraints, measured

| Constraint | Value | Record |
|---|---|---|
| Smallest board the system targets | nRF52840: 256 KB RAM, 1 MB flash, shared with OpenThread and mbedTLS | `docs/design/2026-07-03-nsl-tuvm-first-step-design.md` §1 ("Fit the 256 KB-RAM class (nRF52840), which Toit cannot") |
| VM arena on the nRF52840 | 49 152 B, holding objects, the VM stack (4096 words) and floats | `nodus/oevm/zephyr/docs/BOARD_PORTS.md` (config-D) |
| VM arena on the ESP32-C6 (the bench board) | 128 KB | `nodus/oevm/docs/vm.md` §Memory |
| VM code, Cortex-M4 | `text` in `nodus/oevm/tools/footprint-mcu-baseline.txt` (banked 2026-08-26); 42 178 B measured on `master` 09b61e1 with the #313 hot-patch hooks, over the bank (rete#358) | `tools/footprint.sh`, an aux gate of the suite |
| VM static RAM, Cortex-M4 | `data_bss` in the same file | the same |
| Execution | an interpreter, no JIT; 11.83 µs per activation on the C6, 11.13 µs on the RP2350 | `docs/overview.md`; `docs/results/2026-10-01-rp2350-first-numbers.md` |
| Where the program lives | a read-only `.oimg` in its own flash partition, executed in place; firmware flashed once | `protocol/IMAGE-ARTIFACTS.md` §2a; rete#310 |
| Who reads the other end | porta, a Go gateway, mirrors each node's state over UDP + CBOR | `protocol/`; `nodus/oevm/docs/vm.md` §The reactor |

Two things follow from the table. First, the VM's *code* is not the tight axis: about 40 KB of flash on a 1 MB part.
RAM is (the footprint gate's own header calls it "the scarce axis, #53"): the arena is under a fifth of the
nRF52840's RAM, and the radio stack takes most of the rest. Second, the binding constraints are as much about *kind*
as size: no JIT, a program that cannot be written to at run time, and a peer that is not nsl.

**On the 256 KB premise.** "The divergences exist to fit 256 kB" is half right. 256 KB is the nRF52840's **RAM**, and
fitting that class is the first of the two strategic bets in the 2026-07-03 pivot. But the pivot's own account of
*why* it removed NLR, fibers and growable stacks is complexity — "a cascade of interacting complexity … with the VM
ballooning far past its simple intent" (§1 of the same design) — and the measured saving was 3 921 B of code
(−10 %) and 676 B of RAM (`nodus/oevm/docs/MILESTONE_A.md` §8). Size was the target; simplicity was the mechanism.
Most other rows were decided for reasons that would hold on a large board too (Figure 1).

## 2. The VM in one page

oevm is a register VM in C, built on Piumarta's Open Extensible Object Model: object, symbol and vtable are the only
primitive kinds, and everything else — arithmetic included — is a message a vtable answers. 31 opcodes in 32-bit
fixed-length instructions (`oevm.h` `enum Opcode`); a small-integer fast path that falls back to a send;
`OP_SEND_IMPLICIT` for Newspeak's implicit-receiver send, which walks the lexical enclosure chain before the
inheritance chain (`nodus/oevm/docs/op_send_implicit.md`). One flat, non-recursive interpreter loop over one
fixed-capacity stack; a heap `Context` so that a block can outlive its frame, which is the whole reactive model. A
precise mark-sweep collector over a fixed arena, no `malloc` at steady state. Selectors are Symbols that carry their
primitive id (rete#226), and the 160 primitive selectors are one list (`nodus/oevm/src/prim_names.def`).

The kernel is nsl, not C: `nodus/nsl/lib/kernel.nsl` holds collections, the value algebra, the CBOR codec, channels,
the space, timers, the supervisor and the gateway bridge. C carries data, nsl carries policy.

## 3. The dialect against Newspeak

![What drives each divergence](img/nsl-divergence-drivers.svg)

_Figure 1. Each group of ledger rows against the reason its record gives. Rendered from
`img/nsl-divergence-drivers.svg`._

### 3.1 Kept

- **No global namespace.** Every name is reached through an object handed in: `platform List`, `platform space`. A
  class that was not given `platform` cannot reach the library at all (ledger rows 8, 9).
- **Modules are objects.** `class App packageUsing: manifest = (…) ( public main: platform args: args = (…) )`; the
  module is an instance parameterised by what it was given.
- **Nested classes and implicit-receiver sends**, with Newspeak's lookup order (lexical first), so an upgraded
  superclass cannot capture a name the enclosing module defines.
- **Slots with `::=` (mutable) and `=` (immutable), `::` assignment, keyword selectors, blocks, class-side bodies.**

### 3.2 Changed or removed

| Area | Newspeak | nsl | Primary driver |
|---|---|---|---|
| Class references | virtual (late-bound slots) | static, resolved at compile time to `OP_NEW` (row 1) | no JIT: production Newspeak makes virtual lookup free on Truffle/Graal |
| Composition | mixin application | single inheritance, one `delegate` (row 2) | as above; mixins are how virtual classes compose |
| Non-local return | `^` from any block unwinds to its home | compile error from an escaping closure (row 21) | simplicity: NLR forced the reify/unwind/cross-fiber cascade |
| Concurrency | actors / processes | a run-to-completion reactor over a tuple space (row 22) | the same pivot; determinism, predictable RAM |
| Exceptions | class hierarchy, resumable | `Record {#tag,#text}` values, predicate `on:do:`, `ensure:`, terminate-only (row 24) | errors cross a channel and the wire, so they must be values |
| Reflection | ambient | a `mirror` capability held only by the Supervisor (rows 15, 16); no `class`, `perform:`, `thisContext` (row 26) | capability security; a closed-world image |
| Integers | unbounded | one type, ±(2^64−1), overflow traps (row 29) | one range for host, board and CBOR |
| Strings | collections of characters | immutable UTF-8 bytes, no `Character` (row 10) | one allocation, a GC leaf |
| Symbols | interned at run time | interned at compile time only (row 30) | the image is read-only |
| Methods | recompiled in place | read-only image; a debug build can override a method from RAM and restart its frame (row 31) | the image lives in flash |
| Syntax | cascades, literal arrays, `$a`, radix numbers | 109-line grammar without them (row 27) | simplicity |
| Access modifiers | enforced | parsed, not enforced (row 25) | **undecided** — a finding of the 2026-10-06 audit |

### 3.3 Why most of this is reversible

The ledger's framing still holds: the *structural* half (no globals, modules as objects, confined privilege) was paid
for first because it cannot be retrofitted, and most subtractions are additive to restore — static binding is the
special case of late binding in which no class slot is overridden, so nothing written for nsl becomes wrong if virtual
classes come back. The exceptions are rows 21–24: restoring NLR, processes or resumption would bring back the
machinery the pivot deleted, and they are not on any plan.

## 4. Against Smalltalk-80 and Pharo

nsl's ancestry runs through Smalltalk (the lineage is `st/berry` → `ns/berry` → `ns/oevm` → `nsl/tuvm` → rete), and a
Smalltalk programmer reads it easily. The differences are mostly about *where authority and state live*, not about
expression.

| | Smalltalk-80 / Pharo | nsl |
|---|---|---|
| Everything is an object; computation is message sends | yes | yes — including arithmetic, which falls back to a send off the integer fast path |
| Keyword selectors, blocks as closures | yes | yes |
| Control flow | `ifTrue:` etc. are messages, inlined by the compiler | the same, and the inlining is now the only implementation (row 19) |
| Namespace | one global `Smalltalk` dictionary; any method names any class | none; everything through `platform` (rows 8, 9) |
| The image | a live, mutable heap you develop in and ship | a frozen `.oimg` built from source; the VM runs it from flash |
| `thisContext`, `become:`, `perform:`, `instVarAt:` | yes, ambient | no; frame access only through the confined mirror (rows 15, 26) |
| Processes and semaphores | yes | no; reactions that run to completion (row 22) |
| Non-local return | yes | only from inlined blocks (row 21) |
| Exceptions | class-based, resumable | value-based, predicate-filtered, terminate-only (row 24) |
| Class extension (adding methods to `Object`) | yes, from any package | no; the kernel is one module, and an app cannot reach its classes to extend them |
| Debugging | in-image, by anyone | over a line protocol to the confined debugger; breakpoints by image offset (row 16) |

What nsl takes from Newspeak rather than Smalltalk is the point of it: a module cannot reach anything it was not
given, and that is what makes a capability *mean* something. What it gives up relative to Smalltalk is liveness — the
system is developed on a host and deployed as an image, and hot patching is a debugging act (row 31), not the way
programs are built.

## 5. Why an object language, for code written by agents

The platform's current focus is agent first: every custos verb has a machine-readable HTTP path before any human
front (peer design D11–D13, `docs/design/2026-10-04-custos-peer-design.md`). The language should be chosen on the
same terms. The argument, with what is measured and what is not:

1. **Keyword selectors name every argument at the call site.** `aSocket sendTo: host port: p data: bytes` cannot be
   called with the port and the host swapped without the code saying so; `sendto(fd, buf, len, 0, addr, alen)` can.
   The selector is the documentation, and it is the part of the code an agent pattern-matches on. *Not measured*: we
   have no count of argument-order errors by agents in nsl versus C or Python.
2. **The whole language fits in a context window.** Measured 2026-10-06 (bytes; tokens estimated at 3.5 bytes per
   token, not counted by a tokenizer):

   | Piece | Bytes | ≈ tokens |
   |---|---:|---:|
   | Grammar, `nodus/nsl/grammar/ns.ohm` (109 lines) | 3 416 | 1 000 |
   | User's guide, `programming_with_nsl.md` | 27 385 | 7 800 |
   | Dialect ledger, `NEWSPEAK_DIALECT.md` (before this audit's additions) | 36 374 | 10 400 |
   | Kernel's public selectors (438 signatures) | 6 397 | 1 800 |
   | Kernel source without comments | 91 815 | 26 200 |
   | Kernel source as written | 247 689 | 70 800 |

   Grammar, guide, ledger and the comment-free kernel come to about 160 KB, roughly 45 000 tokens: the complete
   language, standard library included, fits a 200 k window with room for the program, and the grammar plus guide plus
   public selectors fit easily in the 64 k window of a local model. The kernel *with* its comments does not fit
   64 k. A Python or C agent works from memory of a library it cannot see; an nsl agent can be given all of it.
3. **Authority is visible in signatures.** A module that can use the network says so in its header
   (`usingPlatform: platform net: net`); a channel capability can only narrow. An agent — or a reviewer of an agent's
   work — can read what a piece of code is able to touch without reading its body. (Weakened today by row 32:
   `platform` hands out every device.)
4. **A small, closed primitive set.** 160 primitive selectors in one file, all others are nsl. There is no FFI to
   wander into.
5. **Errors written for the reader that is an agent.** A message not understood names the receiver's class and the
   nearest selector it does understand (rete#247, ~1 KB of flash spent on the abort path on purpose: "an operator that
   is an agent is exactly as good as the error line it is fed", `nodus/oevm/tools/footprint-baseline.txt`).
6. **Every artefact is addressable.** The image map names every method by offset, file and line
   (`protocol/IMAGE-ARTIFACTS.md`), so a breakpoint, a profile sample or a crash names source an agent can open.

**Against it, honestly.** Dynamic typing gives an agent nothing at compile time: a misspelt selector is found when it
runs, which is why item 5 exists. nsl is not in any model's training data, so an agent writes it from the context it
is given, not from habit — which is the bet item 2 makes and has not yet tested. What would test it: the same small
board tasks given to an agent in nsl (with the language in context) and in Zephyr C, scored on first-run correctness.

## 6. Costs, and what would lift each

| Cost | Felt as | Would lift it | Ledger row |
|---|---|---|---|
| No virtual classes or mixins | a library cannot be specialised per target except by choosing a module at boot | restore late-bound class slots on the same vtable substrate | 1, 2, 6 |
| No NLR | loops that would `^` out of a `do:` are rewritten as `detect:ifNone:` or a flag | not planned | 21 |
| No processes | a long computation starves timers | split it across reactions; the liveness observer shows the stall | 22 |
| No resumable exceptions | a handler cannot supply a value and continue at the raise | `on:do:` answering the default | 24 |
| `platform` is all-powerful | any app holds every device | narrowed platforms, or `serial:`/`net:`-style header grants for devices | 32 |
| Access modifiers unenforced | `private` documents intent and nothing more | emit them and check, or delete the check | 25 |
| Hot patch limited to known constants | a fix that names a new selector or string cannot be patched | RAM interning; map-carried offsets | 30, 31 |
| No `Character`, byte strings | text is bytes | a rune-aware protocol (tracked as issue #1 in the old tracker) | 10, 12 |

## 7. Open questions

- Row 25: enforce access modifiers or delete the mechanism?
- Row 32: is `platform` meant to hand out every device, or should devices become header grants as `serial:` and
  `net:` are?
- Section 5: is the agent-in-nsl versus agent-in-C comparison worth running before 0.1 is public, so that the claim is
  measured rather than argued?

## References

- `nodus/nsl/docs/NEWSPEAK_DIALECT.md` — the divergence ledger (rows 1–32, audited 2026-10-06).
- `docs/design/2026-07-03-nsl-tuvm-first-step-design.md` — the pivot: no NLR, no fibers, the reactor.
- `nodus/oevm/docs/MILESTONE_A.md`, `MILESTONE_E.md` — what the pivot deleted, measured; exceptions.
- `nodus/nsl/docs/design/2026-07-04-L7-integer-semantics-decision.md` — integers.
- `nodus/oevm/docs/vm.md`, `op_send_implicit.md` — the VM.
- `docs/design/2026-10-04-custos-peer-design.md` D11–D13 — agent first.
- `docs/results/2026-10-06-313-patch-restart-spike.md` — hot patching.
- G. Bracha et al., *Modules as Objects in Newspeak* (ECOOP 2010); G. Bracha and D. Ungar, *Mirrors: Design
  Principles for Meta-level Facilities of Object-Oriented Programming Languages* (OOPSLA 2004); I. Piumarta and A.
  Warth, *Open, Extensible Object Models* (2007); A. Goldberg and D. Robson, *Smalltalk-80: The Language and its
  Implementation* (1983).
- [ZT10] B. Trinkenreich, A. Lawson and H. Carter, *Zephyr Turns 10: A Decade of Adoption, Maturity, and Ecosystem
  Evolution*, The Linux Foundation, March 2026 (CC BY-ND 4.0), `docs/ZephyrTurns10_Report_Revised_051826.pdf` —
  900+ boards; learning curve; Figure 35, GenAI trust.
