# Next steps — overview

Written 2026-07-31 at the end of the #100 session. A living document, not a dated decision
record: re-read it at the start of a session and edit it, don't append to it.

Two audiences in one file. **Part 1 is the bench** — the physical work waiting on the C6
shipsBells rig. **Parts 2-4 are the thinking** — what actually gates a public release, and
which new targets are worth timing a bring-up on.

---

## Part 1 — C6 shipsBells: what to do at the bench

Status: the wall-clock seam (#100) is built, tested and running. The DS3231 answers on
i2c0 and its oscillator is ticking. Everything below is blocked on wire and hands.

### 1.1 Check the trickle-charger BEFORE trusting the coin cell

**Do this first, and do it with the board unpowered.**

The module is the common ZS-042 layout. The photo in
`zephyr/boards/shipsBells/board.png` shows a small red glass diode and a `201` (200 Ω)
resistor beside the SCL pad — that board's well-documented **LIR2032 trickle-charge path
from VCC to VBAT**. The cell fitted is a **CR2032, which is not rechargeable**.

`zephyr/boards/shipsBells/rtc-schematic.png` shows no such circuit and a plain
"Battery1 +3V", so **it cannot be used to rule this out** — it is not a schematic of this
module.

The test, one minute:

1. Remove the coin cell.
2. Power the module at 3.3 V.
3. Measure the holder's **+** terminal against GND.
4. Anything near VCC (~2.6–3.2 V) means the charging path is present.

If present: remove the `201` resistor or the diode, or fit a LIR2032 instead. Do not leave
a CR2032 on a charger.

### 1.2 Wire INT/SQW → GPIO10

The pin is labelled **`SQW`** on the module's 6-pin header (`32K, SQW, SCL, SDA, VCC, GND`).
It is the DS3231's INT/SQW output, and `rtc-schematic.png` confirms **R21 pulls it up 10 kΩ
to +VCC3.3** — open-drain with the pull-up already on the board, so nothing is needed
MCU-side.

Target is **GPIO10**, already declared in the overlay. Checked free on this part: not
strapping (4/5/8/9/15), not USB (12/13), not the UART0 console (16/17), not SPI flash
(24–29; `ngpios` is 30).

Already wired and working, for reference: **SDA → GPIO6, SCL → GPIO7**, module VCC → **3V3**.

Two standing rules for this module:

- **Power it at 3.3 V, never 5 V.** R22/R23 pull SDA/SCL up to whatever VCC is fed, straight
  onto the C6's pins.
- **Do not add pull-ups.** The module already carries 10 kΩ on SDA, SCL and INT/SQW
  (R21/R22/R23).

### 1.3 Then, and only then, turn the alarm on

In `zephyr/prj_esp32c6.conf`, flip:

```
CONFIG_RTC_ALARM=y
```

**Not before the wire is physically in place.** Either `CONFIG_RTC_ALARM` or
`CONFIG_RTC_UPDATE` makes `rtc_ds3231_init()` call `rtc_ds3231_init_isw()`, which configures
the pin and returns `-ENODEV` if it is not ready — taking the *entire RTC device* down with
it. That presents as "the driver is broken", not "the pin is floating", and will cost an
hour if you hit it without expecting it.

(`isw-gpios` is already in the overlay regardless, because the driver's device macro reads
it with `GPIO_DT_SPEC_INST_GET` — not the `_OR` form it uses for `freq-32khz-gpios` — so
omitting the property is a *compile* error, not graceful degradation.)

### 1.4 Build the alarm seam

Design settled this session, not yet written:

**Alarm2, not a 1 Hz square wave.** Alarm2 with a minutes-match mask fires on a boundary you
set, so after each strike you write the next half-hour into it and the C6 does *nothing at
all* until the boundary — no polling, no counting edges, no accumulated error, and the wake
instant is the RTC's own rather than yours. A 1 Hz SQW is a poll loop wearing a hardware
hat, and a missed edge is silent.

Mechanics: set `INTCN=1` so the pin is the alarm output rather than a square wave. Alarm2
supports `HOUR|MINUTE` masks but not `SECOND` (the driver gives `SECOND` to Alarm1 only),
which is exactly right for half-hour boundaries — alternate the minutes field between 0 and
30 after each strike, one I²C write per strike.

**The thing that will bite:** INT stays asserted until you clear `A2F` in the status
register over I²C. The flag clear is what deasserts the line — *not* the read of the time.

Shape it on `sensorArmEventPost:on:threshold:` — the established
`pulse_post_isr(chan_id, handle_idx)` pattern, with both resolved in thread context by the
caller. An ISR must never touch the VM.

### 1.5 Correct the clock

It currently reads **+20 s against host**. That is the stale seed constant in
`WallClockNode.nsl` (baked at authoring, written ~40 s later), not drift. `seedIfUnset:`
deliberately refuses to overwrite a clock that already reads a valid time, so a re-flash
will not fix it — set it through a real path, or bump the constant and clear the RTC first.

### 1.6 Then start measuring drift

The design's §10 wants the part's real ppm measured rather than read off the lid, and this
module earns that: **the IC is marked DS3231M** (MEMS, ±5 ppm) where the run of five
specifies a ±2 ppm TCXO — *and* its package (16-pin) disagrees with its marking (a genuine
DS3231M is 8-pin SO). Relabelled parts are common on these boards. Measure it.

---

## Part 2 — What actually gates a public release

### The reframe: these are two different bars

The strategy doc's falsifiable test for Path B is "one outside deployment surviving M weeks,
with #25, #56, #67, #71, #74 closed behind it." That is the **fleet-service** bar.

**Public release of nsl-tuvm is a much smaller bar.** Nobody cloning the repo to try it on a
board they own exercises OTA, signing or A/B rollback on day one. They exercise: *can I get
this running, and can I tell what it is doing.* Conflating the two costs months.

#79 already names the real blocker, and it is on nobody's feature list:

> "Today there is no entry point that does not start with 'read five repos'."

That is the gate. Everything below is roadmap.

### The four candidates, ranked

**1. porta → channels. First, and not close. — DESIGNED 2026-08-01, PLANS 1-2 DONE, PLAN 3 (HARDWARE) NEXT.**
The only one that *forecloses* the others. Debug-over-porta, OTA-over-porta and
agent-facing-porta all ride on the protocol — demo any of them on the v1 command protocol
and you rebuild the demo after the rewrite. #25 says it outright: "porta development gets a
stable node-side contract early." The v1 read-through conclusion still holds: **design the
node side first**, and read nodus/porta as evidence rather than spec.

Design 2026-08-01; plan 1 (porta side) and plan 2 (node side) are both built and green:

- `~/workspace/porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` — decisions
  D1–D8, three of which *correct* the ratified `PROTOCOL-NSL.md`.
- `~/workspace/porta/docs/superpowers/plans/2026-08-01-porta-space-sync-core.md` — **plan 1,
  DONE** (demolition → `cval` → `space` → `wire` → loopback gate).
- `~/workspace/tuvm/docs/superpowers/plans/2026-08-02-porta-space-sync-node.md` — **plan 2,
  DONE 2026-08-02**: the space's monotonic seq + `changedSince:` (prims 138–140),
  `class PortaBridge` in `kernel.nsl`, `zephyr/PortaBridgeNode.nsl`, and
  `tuvm/tests/porta_sync_host.sh` — the nsl node against a real `porta serve` over UDP
  loopback, green on the first protocol run (`obs/health` reaches porta, `goal/name`
  converges to `obs/name`, `tel/log` exactly once).
- **Plan 3 — the C6 hardware gate — is unwritten, and it is what closes the rung.** The
  same three legs witnessed over a real Thread mesh, including a deliberate datagram
  drop. Everything it needs is host-proven; what it adds is the radio, the C6's node id
  and gateway address, and the drop.

Deferred out of plan 2, each filed rather than assumed: the cursor and `bootId` live in
RAM (NVS), the bridge holds the root space rather than a `goal`-write + `obs`-read pair
(G5), cadence is a constant rather than §4.4's controller, and a stream entry written
while the bridge is down is lost — fine for `tel/**`, **not** for `sys/panic`.

Settled, so it is not relitigated: **no backward compatibility** — v1 is deleted, not
deprecated, and the Toit/`st` kinds go with it. **Transport is plain UDP + CBOR**, because
CoAP rides the same UDP and does not dodge 6LoWPAN fragmentation; it earns its keep on the
bulk/OTA plane (blockwise) and DTLS, not on a control plane capped at 512 B/batch. **The
exchange needs no retransmit layer** — it is idempotent by cursor in both directions, so
loss is absorbed by the next check-in.

New issues from that session: porta#26 (a cold boot at cursor 0 must not re-execute old
`goal/do/*` actions — replaying a cell is idempotent, replaying `reboot` is a brick) and
porta#27 (node identity is a source constant in the first slice).

**2. OTA — but only the host-only slice (#25).**
Marked P1 revenue-gate, and the reasoning is the strongest in the tracker: simultaneously
the least-built and the most decision-foreclosing part, and the nRF52840's 1 MB must budget
MCUboot + A/B + NVS *up front*. The point is not to ship OTA before release — it is that the
flash map becomes a transcription of proven host behaviour instead of a guess. Cheap:
localhost UDP, no hardware. Do the slice, defer real OTA.

**3. Debug/profiling/console over porta (#56).**
Highest narrative value of the four — it *is* the agent story, not a supporting feature. And
it just got cheaper: #57 said the C6 unlocks the debug leg of #56, and as of 2026-07-31 the
C6 is proven and running tuvm with 44% of SRAM spare. Third only by sequencing, since it
should ride on the new protocol.

**4. Agent-friendliness.** Fourth because it is mostly not a separate workstream — see below.

### Agent-friendliness, concretely

Three of the things that make this platform agent-friendly are **already revenue gates** and
were never labelled as such:

- **Observability that outlives the node (#56).** "Metrics die with the node"; `/stats` is a
  *pull*. Pull-based introspection is a human affordance — a human is there to click. An
  agent arrives after the corpse is cold.
- **Provenance (#74).** A node cannot name its own build. An agent cannot reason about a
  fleet when it cannot tell which code is on which node. Unglamorous, load-bearing.
- **Iteration cost (#67).** Agents iterate 10–100× more than humans, so per-cycle cost
  multiplies in a way it never did for a human developer. This is the argument that survives
  the "an agent writes C fine" substitution.

Two are genuinely untracked (now noted on #79):

- **porta as a typed tool surface, not a UI.** Agents do not read dashboards. Every
  operation wants to be a callable with structured arguments and structured results — stable
  names, machine-readable errors, no scraping. A human dashboard should be a *client* of
  that surface. This is the likeliest thing to get built the wrong way round, because the UI
  is what demos.
- **Inspectable authority.** Grants are already enforced; the agent-facing half is making a
  grant *legible*, so an agent can ask "what am I allowed to do here?" and get an answer.

Plus the sandbox: #25's fourth item (N VMs + a toy porta in one CI job) is worth more for
agent-friendliness than its issue text suggests — it is what lets an agent try a fleet
operation without hardware.

### The measurement nobody is taking

Every falsifiable criterion in the strategy doc is unmeasured, and the only fleet is on the
bench. The strongest single artifact it names is **time-to-first-working-application on a
board never seen before, honestly timed, failures included**.

2026-07-31 was close to that and nobody was holding a stopwatch: "can you see the C6" to a
DS3231 seam running on hardware with four passing host tests, about two hours, including
finding a real Zephyr driver defect *by reading it before flashing*.

That is now the **third** instance of the best story — Grid-EYE wedge diagnosed from a
timing signature, C6 radio localised to four driver bugs (zephyr#114163), and now the
DS3231's missing `-ENODATA` (#103) predicted from source and confirmed on the first hardware
run. Three is a pattern, not an anecdote.

**Suggested sequencing:** porta→channels, then the #25 host slice, then #56 riding on the
new protocol — and start timing bring-ups now, because that measurement is free and it is
the one number that would make the pitch land.

---

## Part 3 — New targets worth timing a bring-up on

### RAM is not the binding constraint

Current heap settings: ESP32 49 KB, MagTag 64 KB, **C6 128 KB**. The C6 build uses 247 KB of
437 KB SRAM with I2C, RTC, the DS3231 driver and `CONFIG_LOG` all linked in.

The binding constraint is **whether the HAL is already in the west workspace**.
`~/zephyr-esp32/modules/hal/` already carries st, stm32, rpi_pico, silabs, renesas, nxp, ti,
infineon, atmel, microchip, ambiq, gigadevice, nuvoton, wch. Pick outside that set and you
are timing a west setup, not a bring-up.

### Candidates with ≥256 KB on-chip SRAM and a board port in-tree

| Part | SRAM | Board port | Rough street price |
|---|---|---|---|
| **RP2350** (Pico 2) | 520 KB | `rpi_pico2` | ~$5 |
| **RP2040** (Pico) | 264 KB | `rpi_pico` | ~$4 |
| **STM32H743** | 512 KB | `weact/mini_stm32h743` | ~$12–15 |
| **STM32H723** | 320 KB | `nucleo_h723zg` | ~$28 |
| **STM32F412/F413** | 256/320 KB | `nucleo_f412zg` / `f413zh` | ~$20 |
| **EFR32MG24** | 256 KB | `silabs/xg24/*` | ~$30–50 |
| **ESP32-C3** | 384 KB | in-tree | ~$3 |
| **SAME51/SAMD51** | 256 KB | atmel | ~$25 |
| **Renesas RA6M4** | 256 KB | renesas | ~$20–30 |

ST alone has 127 board directories in-tree — the deepest bench available, and untouched.

### The non-obvious one: nsl uses doubles

`src/oevm.h:187` — `struct Float { double value; }`.

Every Cortex-M4F/M33 has a **single**-precision FPU, so nsl floats have been
software-emulated on the nRF52840 all along. Two consequences:

- **RP2040's lack of an FPU costs far less than it looks** — you were already softfloat for
  doubles on every ARM part run so far.
- **STM32H7 (Cortex-M7) has a double-precision FPU** — the only candidate where nsl floats
  would run in *hardware*. Against the existing STIC/Perfstex numbers that is a genuinely
  novel data point, on a ~$12 board.

(Unrelated: `prj_dongle_softfloat.conf` was about matching Nordic's MPSL radio-library ABI,
not about nsl.)

### Picks

For a bring-up *timing* measurement the variable you want is **a vendor never built for**.
ESP32×3 and nRF52840 both carry months of accumulated work; timing a fifth Espressif part
measures nothing.

**First: Raspberry Pi Pico 2.** ~$5, 520 KB, board port present, HAL fetched. The most
widely owned board in the world, so the number needs no explanation to any audience. A third
architecture family (M33, and RP2350 carries RISC-V cores too). Cheap enough to buy five and
brick one.

**Second: WeAct mini_stm32h743.** ~$12–15, 512 KB, double-precision FPU, and ST is the
largest Cortex-M vendor — the "professional standard" leg the bench lacks entirely.

**Skip for this purpose:** ESP32-C3/S3 (same vendor, measures nothing); EFR32MG24 is
interesting as a *second Thread stack* comparison against the C6 work, but dev kits are
pricey and it is a narrower question.

**Pico gotcha, decide before starting the clock:** flashing is the easy part (hold BOOTSEL,
plug in USB, it mounts as mass storage, drag the UF2 — far gentler than the MagTag's
BOOT0+RESET). But Zephyr's default console on `rpi_pico` is **UART0 on GP0/GP1, not USB** —
the same trap that cost time on the MagTag. Either enable USB CDC as the console, or put a
USB-serial adapter on two jumper wires, or use a second Pico as a debugprobe. Decide at
minute zero, not minute forty.

**On timing honestly:** run the clock from a cold start and count the failures. The most
interesting content of the C6 session was a driver defect found by *reading* rather than
flashing, and that texture disappears if only the total is reported.

---

## Part 4 — Loose ends from 2026-07-31

- **#104** — the WROOM-32 does not link at all (`undefined reference to sntp_simple`);
  `prj_esp32.conf` sets `CONFIG_WIFI=y` but never `CONFIG_SNTP=y`. Pre-existing, confirmed by
  stashing. One-line fix, plus a sturdier variant (guard the body on `CONFIG_SNTP`).
- **#103** — the upstream DS3231 `-ENODATA` fix. Same family as #91–#93/#95.
- **The C6 was running Toit/Jaguar** (`test-server`, id `c3a7d184-…`) and that install was
  overwritten. If it is wanted back, it is a re-flash.
- **`ESPTOOL_PORT` must be set explicitly for the C6** — its `run.sh` profile pins
  `*Espressif*` → `/dev/ttyACM0` (native USB-JTAG), which is not cabled. Everything goes over
  the CP2102N. Three CP210x boards share the by-id glob, so identify by `esptool read_mac`.
