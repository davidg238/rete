# Revenue and positioning — strategy

**Date:** 2026-07-22
**Status:** framing agreed in session; no commitment made to any path
**Companions:** `review/WHY.md` (why the platform), `review/WHY-AGENTS.md` (why agents),
`docs/design/2026-07-21-performance-deck-design.md` (how the argument is presented)

This is the commercial companion to `WHY.md`. That document argues the platform is *worth
building*; this one asks who pays for it, and re-reads the tracker in those terms. It is a
decision record of framing, not a business plan, and it commits to nothing.

---

## 1. The blunt frame: the runtime is not the asset

The comparison table in `WHY.md` is also the evidence: Toit, MicroPython and AtomVM are all
free. Toit is the closest comparable — better funded, a real team, the model this project
chases — and **they open-sourced the language.** Embedded runtimes are commoditised.

Worse, a *proprietary* runtime from a single engineer reads to a buyer as bus-factor risk
they are being asked to pay for. Open-sourcing tuvm/nsl therefore costs nothing that could
have been sold, and removes the main objection to adopting it.

**Corollary:** every hour spent making the VM better is an hour spent on the part that will
never be invoiced directly. That is not an argument against doing it — the VM is what makes
the other paths credible — but it should be a deliberate choice rather than a default.

---

## 2. Three places the money actually is

Ranked for the real situation: one engineer, a platform that mostly works, no product, no
outside deployment.

### Path A — your time, priced up (available now)

Bring up a Zephyr device on Thread, debug the 802.15.4 driver deep enough to land an
upstream fix, and put a scripting layer with remote deploy on top. That combination is rare
and the day rate reflects it. The platform stays a private productivity weapon; the customer
buys an outcome, not a licence.

- **Revenue in weeks, not years.** It funds everything else and needs nobody's permission.
- **It validates.** A paying customer's requirements are the only unbiased priority list.
- **The trap:** it consumes all the time unless each engagement is deliberately harvested
  back into the platform. Budget the harvest, in writing, per engagement.

### Path B — the fleet service (the asset that compounds)

Provisioning, signed OTA, canary + rollback, and **observability**, sold per device. This is
the layer already designed across porta/nodus; it is also a category with real companies in
it — Memfault on device observability specifically, plus Golioth, Balena, Particle on
adjacent ground.

The relevant coincidence: **Memfault's entire business is what the never-silent
instrumentation law produces as a by-product here.** Spans, GC and heap counters, channel
overrun counters, structured crash records on `sys/crash` rather than stack-trace prose —
all readable from the node with nothing attached.

What differentiates it from another device cloud in 2026 is the agent story: *your fleet can
be operated by an agent whose confinement is enforced by the platform, not by a prompt.*
Capabilities are structural, and there are tests that show it. Almost nobody in embedded can
say that sentence honestly.

### Path C — a vertical product (best economics, needs capital)

Pick one physical-world measurement niche and sell the outcome — device and cloud together.
Highest margin, but it needs a domain partner and money, and it is the path most likely to
consume the platform rather than exploit it.

### The sequencing

**A now, to fund and to validate; build toward B as the asset; keep C as an option that a
customer relationship might open.** The next commercial milestone is not a feature — it is a
**reference deployment**: N nodes running unattended for M weeks with the dashboard as the
evidence. That single artifact sells consulting, sells the service, and satisfies an
investor, and producing it closes #67 honestly along the way.

---

## 3. The tracker, re-read: five issues are revenue gates

Labelled `revenue-gate` on 2026-07-22, each with the commercial rationale recorded as an
issue comment. None of these needs a plan change; the label corrects what they are understood
to *block*.

| issue | filed as | actually blocks |
|---|---|---|
| **#25** code-push slice (oimg swap + A/B rollback + signing seat) | strategic sequencing | **all of path B.** Nobody buys unsigned remote code push; nobody buys fleet deploy without bounded blast radius. The first customer fleet bricked by a bad push ends the business |
| **#56** device observability | developer convenience | the **product surface** of path B — in a fleet service, reliability reporting *is* the product |
| **#67** time-to-deployed-change | a deck badge | the **sales number**. "Fix-to-field in seconds, not a flash cycle" is currently an expectation, not a measurement |
| **#71** keep-alive Kconfig guard | config-drift guard | **uptime**, which is the thing being sold. A defence that can be switched off by a config edit and say nothing is an outage waiting to be re-shipped |
| **#74** a node cannot name its own build | small gap | **structural for path B.** A fleet service whose devices cannot say what they are running is not a service — signed OTA can deploy but cannot report, and observability cannot attribute a symptom to a version |

---

## 4. The Zephyr question — who actually benefits

The session raised a stronger version of the argument: as signing, OTA and canary/rollback
land, does this become an advantage *for the Zephyr platform itself* — a decent engineer,
possibly a hardware engineer, plus Zephyr plus an agent, reaching working software within
**hours** of a prototype landing on the bench?

### Why the mechanism is real

Zephyr already removed the driver problem: devicetree describes the board, and hundreds of
maintained drivers sit behind uniform APIs. What remains between "board powers on" and
"application works" is the application layer — and that layer is C, with a flash cycle per
change, no memory safety, and a high skill floor. Precisely the four things this platform
removes:

- **no flash cycle** — `.oimg` push, seconds (unmeasured: #67);
- **a mistake is a supervised crash with a structured record**, not a silent corruption on a
  device you cannot reach;
- **capabilities bound the blast radius** structurally;
- **the node reports on itself**, so the loop closes without a scope or a JTAG pod.

The loop becomes: power the board → devicetree describes it → push a script → read the
node's own telemetry → iterate. That is a different activity from firmware development, and
it is one a hardware engineer can plausibly do.

### The sharpest part: debugging the *hardware* through known-good drivers

If the driver is upstream and proven on other boards, a failure localises to *this board* —
solder, power, pull-ups, crystal, antenna, bus wiring. The scripting layer makes that
bisection interactive rather than a reflash per hypothesis.

This is not speculative; it has happened twice in this project's own history, in both
directions:

- **The Grid-EYE wedge.** `acquire` collapsing from ~12,500 µs to ~130 µs with every read
  failing was the *timing signature* of a sensor returning nil. A board reset did not clear
  it — only a power cycle, because the reset line does not reach the sensor. The
  instrumentation said "your hardware is broken, and here is which part", with no
  oscilloscope involved.
- **The C6 "dead radio".** The same approach ran the other way: with a reference
  implementation to compare against, the fault was located precisely — four Zephyr
  802.15.4 driver bugs, one of which became zephyr#114163. Known-good drivers make the
  *driver* falsifiable too.

**So the claim generalises:** instrumentation plus a known-good driver catalog turns bring-up
into a bisection between board, driver and application, instead of a single undifferentiated
"it doesn't work".

### The correction: Zephyr the organisation is not the customer

Zephyr is a Linux Foundation project governed largely by silicon vendors, and it does not
"adopt" things in the way the framing implies. Its charter is the OS; safety certification
pulls firmly *away* from dynamic scripting layers; and no upstream decision would put this in
anyone's hands.

**The party with the incentive is the silicon vendor.** Nordic, Espressif and NXP sell parts,
and *time-to-first-working-prototype* is a sales metric for them.

Two assets already exist for that conversation: an upstream Zephyr contribution
(zephyr#114163) as credibility, and a working multi-target story across nRF52840, ESP32 and
ESP32-C6.

> **CORRECTION, same day.** The sentence originally here — "they fund tooling constantly for
> exactly this reason" — overstated the case, and David challenged it. Splitting the claim:
>
> - **Solid:** vendors invest enormously in their *own first-party* tooling. Nordic is all-in
>   on Zephyr and nRF Connect SDK *is* Zephyr-based; Espressif maintains ESP-IDF and staffs
>   the Zephyr ESP32 port; NXP funds MCUXpresso and Zephyr work. Time-to-first-prototype
>   plainly is a thing they spend on.
> - **Not supported:** that they fund *third-party* tooling from an outside developer. That
>   is rare. The available counter-evidence is close to home — Toit was a funded startup with
>   a strong product, and no vendor adopted its runtime. Vendor money flowing outward goes
>   mostly to partner/design-house programs, eval hardware, co-marketing, occasional NRE tied
>   to a specific customer design win, and acquisitions — not to independent runtimes.
>
> **What this changes:** do not pitch a vendor cold; there is no door there for a tool
> without users. The door opens from the *customer* side — a design win on their part makes
> their own FAE the internal advocate, and that is the channel that reaches a vendor's
> ecosystem budget. Sequence: (1) a paying customer shipping on a vendor's part, (2) visible
> public traction — upstream contributions, one undeniable demo, (3) *then* the partner /
> co-marketing conversation. This makes path A a prerequisite for the vendor route, not an
> alternative to it. Rollout, eyeballs and vendor access are their own problem and are filed
> as a brainstorming issue rather than answered here.

### Honest bounds

- **Certification.** Anything going into a safety-certified product cannot ship a dynamic
  layer without substantial work. That bounds the market — still large, but say it.
- **Catalog coverage.** The "hours" claim holds only where the drivers exist. #64 (Arducam,
  driver on `main` but not 4.2) is the counterexample already in the tracker: outside the
  catalog you are back to C.
- **"Hours" is unmeasured.** It needs exactly one witness: take a genuinely new board and
  time it, honestly, including the parts that go wrong. That is both a falsifiable claim and
  the best demo this project could produce.

---

## 5. The sharpening the agent question forces

The comparison is no longer *nsl versus C for a human*. It is **nsl + agent versus C +
agent** — and an agent writing Zephyr C works, and improves monthly. Which arguments survive
that substitution is worth being precise about, because the deck currently leans on some that
do not:

**Weakened.** "C is tedious and unsafe to write." An agent does not mind tedium and produces
plausible C quickly. Every ergonomics argument loses force against an agent baseline.

**Strengthened.** Everything operational:

- **Iteration cost.** An agent writing C still pays a build-plus-DFU cycle per iteration.
  Since agents iterate far more than humans do, the per-iteration cost multiplies in a way it
  never did for a human developer. This is #67 and #68, and it is now the load-bearing
  argument rather than a nice one.
- **Blast radius.** An agent's bad pointer in C is silent corruption on an unreachable
  device; in nsl it is a supervised crash with a structured record. That is the difference
  between "can run unattended" and "cannot".
- **Authority.** An agent editing C has total authority over the device. A capability grant
  is a bound the platform enforces.
- **Context.** A small closed surface (~138k tokens, whole system) is held at once.

This belongs back in the deck's Act V, which currently argues against a human baseline in
places where the agent baseline is the honest comparison.

---

## 6. What would have to be true — falsifiable, per path

Following `WHY-AGENTS.md`'s discipline: if these numbers do not materialise, the
corresponding path is wrong and this document should say so.

- **Path A:** one engagement closed at a rate that reflects the rarity, with the harvest back
  into the platform actually banked rather than intended.
- **Path B:** one **outside** deployment — nodes someone else owns — surviving M weeks, with
  #25, #56, #67, #71, #74 closed behind it.
- **The bring-up claim:** time-to-first-working-application on a board never seen before,
  measured honestly, including the failures.
- **The agent claim:** agent MTTR on a fleet fault, and fix-to-field latency (#67), against a
  Zephyr-C baseline.

**The standing risk, stated plainly:** every path above depends on someone other than David
running these nodes. Today the only fleet is on the bench, and every falsifiable criterion in
`WHY-AGENTS.md` is unmeasured. Until one outside deployment exists, path B is a story rather
than a product.

---

## 7. Not doing

- Not selling the runtime, and not keeping it closed to protect a revenue line that does not
  exist.
- Not pursuing "Zephyr adopts this" as a strategy — the vendor conversation is the real one.
- Not building any of path B's commercial layer ahead of a customer who needs it. The gates
  in §3 are labelled so the priority is *visible*, not so they jump the queue.

---

## 8. Open questions

1. Does `review/WHY.md` want a pointer to this document? It is David's tree, so left alone
   pending his call.
2. Which vertical, if path C is ever taken? Thermal/occupancy is the only one with a working
   sensor rung today, which is weak evidence for a market and strong evidence for a demo.
3. Open-source timing: open-sourcing the runtime helps adoption and removes the bus-factor
   objection, but it is easier to do once than to undo. No deadline pressure — but it should
   be a decision, not a drift.
