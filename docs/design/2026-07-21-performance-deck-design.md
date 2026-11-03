# Performance deck — design

**Date:** 2026-07-21
**Status:** design agreed, not yet built
**Companion to:** `review/slides.html` (deck 1 — "Script Zephyr. Don't replace it.")
**Artifact:** `review/performance_slides.html` (one file, four audiences)

---

## 1. What this is, and why it exists

Deck 1 makes the architecture argument. This deck answers the question deck 1 provokes
and does not settle: **is it fast enough?** — and the question almost nobody asks,
**fast enough *for an agent*?**

The thesis is **optimize the right thing**. Microbenchmarks say one thing, a real
workload on real hardware says another, and the axes that actually decide whether a
product ships — build time, observability, deployability, failure legibility — are
mostly unbenchmarked by anyone.

This deck is also a **thinking tool**. Its explicit secondary purpose is to expose gaps
as loud TBDs and convert them into honest gates, rather than to decorate work already
done. Four issues fall out of it by construction (§7).

### Audience priority

Build order, per David: **record → technical peers → investors → conference talk.**

The record deck is the *source*: the data is assembled, checked and captioned once, and
the other three are orderings over it. Building a persuasive deck first is how caveats
get quietly dropped — the paced-run asterisk on the HTTP numbers (§5) is exactly the
kind of qualifier that dies under persuasion pressure.

| audience | what changes | length |
|---|---|---|
| `record` | superset. Every caveat, every method note, the wrap-defect story in full | ~30 slides |
| `peer` | leads with the trade, expects the numbers to be checked, keeps the mechanism detail | ~22 |
| `investor` | thesis is *"we can optimize later, cheaply, without a rewrite — here is the proof"*. The STIC table shrinks to one line: we know where we are slow, by name | ~14 |
| `talk` | opens cold on the 50-minute build — the slide nobody has seen — then earns the rest | ~18 |

---

## 2. The spine

A common preamble, then five acts. The other three decks reorder and filter; they do not
re-argue.

### Preamble (identical in all four)

1. Title
2. **What we're building** — a product that measures and controls values in the physical world
3. **What stops you shipping one** — chips, drivers, software, deployment, maintenance
4. **Zephyr removes one roadblock** — widest choice of parts, hundreds of maintained drivers (bridge from deck 1)
5. **Software is the roadblock that's left** — fastest, safest way to write, deploy, maintain?
6. **Two questions, not one** — *is it fast enough?* and *is it fast enough for an agent?* The deck closes where it opened.

### Act I — reality first

7. A real node, a real job — the two shapes `ThermalHttpNode` runs
8. Where the time actually goes — the device table, 7 spans, all n ≥ 200
9. The client's view — 159 ms, of which **119 ms is radio**; the VM is 7.7%
10. And we can see every stage — segments account for **99.6%** of their parent

Reality precedes the microbenchmarks deliberately: it makes "the right thing" the frame
*before* any comparative number appears.

### Act II — what 256 KB costs, and what it buys

11. Now the microbenchmarks — STIC on ESP32, four languages, no editorial
12. The row that hurts — `collection iterate` 532.6 vs Toit 12.7
13. **Why** — `OrderedCollection` is written in nsl, not C. A footprint decision, not a quality gap
14. **The turn** — that weakness *is* present in the real job (`vmCompute` + `publish` are collection work, 12.3 ms of it), and it still decides nothing, because it is 12 ms inside 159 ms

### Act III — the mechanism

15. Suppose it did matter — OEOM: a method is a field in a vtable
16. Hanoi **12.0 s → 0.103 s, ~119×** — one field swapped; callers, `.oimg` and interpreter unchanged
17. The strategy — write it in nsl, measure, drop C in where the measurement points

### Act IV — the unbenchmarked

18. Everyone benchmarks the wrong things — four axes nobody publishes
19. **Build/iteration** — Model A ~50 min / ~4.9 GB RSS (has OOM-killed a desktop) → Model B, seconds
20. **Change without reflash** — loud TBD
21. **Do you know what your code is doing?** — spans, heap, GC, ISR counters, on the device, no host attached
22. **The proof** — a bug that reported a 23 ms job as 4,277,094,806 µs, caught from a web page served by the node
23. **Failure legibility** — supervised jobs, counted sheds vs `k_oops`, crash records — *and* the counterexample, #65, a silent failure we cannot yet explain

### Act V — agent-first (the premise, not the conclusion)

24. **We are not building another scripting language** — we're building a pipeline an agent is comfortable in
25. **An agent's scarcest resource is context** — the surface table (§4)
26. **Determinism is a context budget** — the virtual clock means a test cannot flake, so an agent never burns context distinguishing flake from fault
27. **Never-silent instrumentation, as a design law** — a human can sense something is off; an agent only sees what is surfaced. This *claims* Act IV retroactively: the spans were never a metrics feature
28. **Capabilities are a structural agent sandbox** — a rooted cap over `fleet/lab/**` is enforced by the platform, not by a prompt
29. **What is not built yet** — signing (G5), canary + auto-rollback (G1), letter-log replay, remote eval. Gates, not aspirations
30. Close

**Act V re-reads Acts I–IV; it does not repeat them.** The 50-minute build is an
agent-loop slide. The spans are an agent-legibility slide. The 99.6% accounting is "we
know what our code is doing." Act V claims those. Restating them makes the deck sag.

Source for Act V's argument: `review/WHY-AGENTS.md`, §"Static typing and agents —
stated honestly" and §"Never-silent instrumentation, as a design law".

---

## 3. The comparator posture — a binding rule

**Every comparator row states the constraint that produced it.** A number without its
constraint is an insult; with it, it is an engineering result.

Specifically, and non-negotiably:

- **Toit is not a foil.** Toit demonstrated that OO belongs in embedded, well beyond
  what CircuitPython attempted, and is the reason there is a target worth aiming at.
  Where Toit wins a row, the deck says so plainly and without hedging.
- **The honest statement of our position is a trade, not a deficit:** we bought RAM with
  speed. nRF52840-class parts — 256 KB — were a *key deliverable*, not a stretch goal.
  Toit is sized for ESP32-class RAM and is faster for it. Same problem space, different
  constraint, different optimum.
- **The surface comparison gets the same treatment.** Toit's library is larger *because
  Toit is self-contained*; ours is small because Zephyr's C carries the drivers. That is
  an architecture difference, not a virtue.
- **No dunking on CircuitPython or uLisp either.** Different bets. uLisp will likely
  *win* the surface row, and we report that.

The headline for Act II is therefore **"What 256 KB costs — and what it buys"**, never
"42× slower".

### A claim explicitly rejected

The deck **must not** say "0.103 s vs Toit's 3.0 s — 29× faster than Toit." That
compares *native C Hanoi* against *Toit's compiled Hanoi*: a C-vs-Toit number wearing an
nsl-vs-Toit label. The honest and still-strong claim is that a substituted method runs at
C speed, so it stops being a language comparison at all. The interesting number is the
**119×**, not a rigged head-to-head.

---

## 4. The data

Every figure lives once, in a `DATA` block, with a `src` naming its witness.

### Measured — device, WROOM-32, 2026-07-21 (tuvm#61)

Witness: `review/program-sequence.md` job-latency rung entry; `review/tests/CLAIMS.md`.

| span | n | mean µs | min µs | max µs | max/min | p50 | p95 |
|---|---|---|---|---|---|---|---|
| `httpQueue` | 297 | 61 | 41 | 118 | 2.9× | <100us | <100us |
| `egress` | 3567 | 1319 | 98 | 71288 | 727× | <1ms | <100ms |
| `jobThermal` | 310 | 25229 | 24408 | 28338 | 1.16× | <100ms | <100ms |
| `acquire` | 310 | 12798 | 12491 | 15533 | 1.24× | <100ms | <100ms |
| `vmCompute` | 310 | 4101 | 3961 | 4804 | 1.21× | <10ms | <10ms |
| `publish` | 310 | 8225 | 7848 | 9045 | 1.15× | <10ms | <10ms |
| `render` | 296 | 39837 | 37401 | 98484 | 2.6× | <100ms | <100ms |

Derived, and labelled as derived: `render` = 15894 µs egress + 23942 µs compute;
client RTT p50 159071 µs − node 39898 µs = **119173 µs network, 75%**.

**The paced-run asterisk is mandatory on every slide showing HTTP figures.** They come
from a run paced at 4 req/s, because the node cannot survive an unpaced burst (#65).
Dropping this qualifier when the investor deck gets tight is the single most likely
integrity failure in the whole project.

### Measured — STIC microbenchmarks, ESP32

Witness: `~/workspaceToit/stic/README.md`. Four columns: Toit, uLisp, CircuitPython,
tuvm/nsl. Shown whole, no row omitted.

### Measured — OEOM substitution

Witness: `~/workspaceToit/stic/README.md` Hanoi A/B table.
Hanoi 16: 0.7 s → 0.0067 s (~116×). Hanoi 20: 12.0 s → 0.103 s (~119×).

### Indicative — surface in tokens (NOT a benchmark yet)

Two systems only, ~4 chars/token, marked as indicative on the slide itself:

| | files | chars | ≈ tokens |
|---|---|---|---|
| `kernel.nsl` — the entire nsl stdlib | 1 | 123,308 | ~31k |
| Toit SDK `core/` | 21 | 323,032 | ~81k |
| Toit SDK `lib/` (full) | 146 | 1,380,028 | ~345k |

Second tier — **whole system**, language + VM: `kernel.nsl` ~31k + hand-written C
substrate ~105k (`src/*.{c,h}` excluding the generated `solidified_image.c`) ≈ **136k
tokens**. An agent can hold the entire system in one context window.

The demonstration, not the claim: the 2026-07-21 session that produced tuvm#61 edited
`span.c`, `kernel.nsl`, `ThermalHttpNode.nsl`, an upstream Zephyr driver, four test
files and five documents — one agent, one context, no handoff.

Upgrading this from indicative to a benchmark is gate 1 (§7).

---

## 5. Architecture

One file: `review/performance_slides.html`. Self-contained, no build step, no external
assets — same constraints as `review/slides.html`, whose CSS variables and slide styling
it inherits so the two decks read as one family.

**Slide pool + playlists.** Each slide is authored once with an `id`. Four playlists name
ids in order:

```js
const PLAYLIST = {
  record:   ["title", "product", "roadblocks", ...],
  peer:     [...],
  investor: [...],
  talk:     [...],
};
```

The four decks genuinely are four *orderings over one set*, so that is the data
structure.

**Per-element audience filters.** Framing varies; data does not.

```html
<h2 data-aud="peer record">What 256 KB costs — and what it buys</h2>
<h2 data-aud="investor">We know exactly where we are slow</h2>
<table><!-- one table, authored once, no data-aud --></table>
```

Elements without `data-aud` appear for everyone. The renderer hides non-matching
elements on slide entry.

**Variants where filtering is not enough.** An investor deck is not the peer deck minus
slides; it is a different arc. Filters handle wording, not pacing. Expect ~3 slides to
need genuine variants (same data, different slide). Allowed, and kept rare.

**`DATA` block with provenance.** Every measured figure once:

```js
const DATA = {
  jobThermal_mean_us: { v: 25229, src: "review/program-sequence.md#job-latency" },
  ota_cycle_s:        { v: null, tbd: "OTA flash cycle — not yet measured" },
};
```

A TBD is a first-class value that renders as a loud badge. **An unmeasured claim cannot
quietly look measured** — the failure `PROTOCOL-NSL-RECORDS.md` §4.4 committed against
itself with `~185–230 B`, and which tuvm#61 had to go and fix.

**URL carries the audience.** `#peer/7` deep-links to a deck and a slide, so a link can
be handed out with the audience pre-selected and the recipient never sees the chooser.
Slide 1 is the chooser and belongs to no playlist.

**Chosen over four separate files** because one number would otherwise live in four
places — and today's table was re-measured twice in a single session.

---

## 6. Non-goals

- Not a rewrite of deck 1. It bridges from deck 1 and stays consistent with it.
- No new measurement *on the device* to build the deck itself. The #61 run stands; §7
  gates are separate work with their own issues.
- No animation, no framework, no external assets.
- Not a marketing asset. The record deck is the source of truth and the other three are
  filtered views of it — if a claim cannot survive in the record deck, it does not go in
  the investor deck either.

---

## 7. Gates this deck creates

Filed as tracker issues so the deck plans honest gates rather than collecting holes.
All four are open as of 2026-07-21.

1. **Surface-in-tokens benchmark (#66)** — full peer set (nsl, Toit, uLisp, CircuitPython),
   both tiers, published methodology: what counts as surface, per-system source of truth,
   how uLisp's C builtin table and CircuitPython's C-modules-plus-bundle are handled.
   Until then the deck shows the two-system indicative card.
2. **OTA / change-without-reflash timing (#67)** — time-to-deployed-change on hardware versus a
   flash cycle. Currently unmeasured and load-bearing for the agent-loop claim.
3. **Build/iteration timing, properly measured (#68)** — Model A vs Model B, timed rather than
   remembered. ~50 min / ~4.9 GB is from notes, not from a recorded measurement.
4. **Deck-vs-record consistency check (#69)** — a script asserting every `DATA` figure still
   matches its cited witness under `review/`.

---

## 8. Open question

Whether the deck should render the STIC table with a **normalised** column (e.g. relative
to the fastest per row). It would make the shape legible at a glance, but normalisation
is a rhetorical act and this deck's whole ethos is that raw numbers plus their constraint
beat processed ones. Defer to build time; default to raw.

---

## 9. Addendum 2026-11-02 — figures revised from the records to hand

The deck (`review/performance_slides.html`, `d166bedc`) now cites: the 2026-10-12 ESP32-C6 three-VM STIC
table (Toit, MicroPython, rete; Richards 4.3× and DeltaBlue 4.4× behind Toit are the rows that hurt; collection
iterate, July's 42× row, is now ahead of Toit); the design paper's token table (language ≈ 45k; whole system
≈ 155k comment-free, ≈ 310k as written — the Act V "one window" claim holds only comment-free); one measured OTA
cycle (#449: 164,000 B in 143 s over BLE through porta; the flash-cycle comparison, #67, still unmeasured); and a
new Act I slide on the C6 check-in decomposition (2026-08-05/09). Act I's thermal figures are unchanged and marked
"re-measure pending": the board must be located first (#455). Gate 3 (#68, build timing) still unmeasured.
