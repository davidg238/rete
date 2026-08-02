# GPIO output + level-read seam — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give nsl two new gpio primitives — `gpio set: n to: aBool` and `gpio level: n` — so the MagTag's red LED and NeoPixel power gate become drivable and a button's held state becomes readable.

**Architecture:** Two platform primitives appended to the VM's append-only prim table, dispatched in `src/platform.c` to a pair of C leaves (`gpio_target_set` / `gpio_target_level`). On the host those leaves are a pin→level table; on the ESP32-S2 they are `gpio_pin_set_dt` / `gpio_pin_get_dt` over a devicetree-declared table in `zephyr/src/main.c`, selected by `#ifdef __ZEPHYR__` — so **no new `.c` files are created**. A pin the board never declared answers `nil` from C; the nsl `GpioPulse` wrapper converts that `nil` into a tagged raise.

**Tech Stack:** C99 (the tuvm VM), nsl (the kernel + test nodes), Zephyr devicetree + GPIO API, Python test runner (`nsl-tuvm/tools/run-one.py`).

**Design of record:** `~/workspace/tuvm/docs/design/2026-07-23-gpio-output-level-seam-design.md`
**Issue:** tuvm#82
**Branch:** `feat/magtag-esp32s2-port` (already checked out)

## Global Constraints

- **Two repos are in play.** `~/workspace/tuvm` (the VM) and `~/workspace/nsl-tuvm` (the kernel). Tests live in a third tree, `~/workspace/nsl-tests`. Commit in each repo you touch.
- **`src/prim_ids.h` is append-only and counted-not-computed.** Never derive a new prim id by arithmetic from the previous one. Count `PRIM_NAMES` entries. The count today is **122**; verify with the command given in Task 1 rather than trusting this number.
- **`dispatch.c` has a compile-time parity guard** (`prim_names_enum_parity_check`) that must be advanced to the new last prim id. If you miscount, the build fails with a negative-sized-array error — that is the guard doing its job, not a bug to work around.
- **Run the nsl suite Path B only** (`.oimg`). Path A does a per-test gcc rebuild of the whole VM and is too slow. Every `run-one.py` invocation below passes the explicit `B` argument.
- **Never run two nsl suites at once** — they share `/tmp` paths.
- **Levels are LOGICAL throughout.** `true` means *active*: NeoPixels **powered** for the ACTIVE_LOW gate on GPIO21, button **pressed** for an ACTIVE_LOW button. Always use the `_dt` variants (`gpio_pin_set_dt` / `gpio_pin_get_dt`), never `_raw`.
- **Pin numbers reaching nsl are silkscreen GPIO numbers.** Devicetree alias names (`gpioout`, `gpioout1`, …) never leave `zephyr/src/main.c`.
- **No `toggle:` primitive.** It is `set: n to: (gpio level: n) not` in nsl.
- **Do not touch the release-event chording path** (`OEVM_BUTTON_RELEASE_BASE` in `zephyr/src/main.c`, `zephyr/MagTagChordNode.nsl`). Migrating it is a deliberately deferred follow-up (design D7).

---

### Task 1: The two prims, the host leaves, and the raw kernel forwarders

Delivers `platform gpioSet:to:` and `platform gpioLevel:` working end-to-end on the host. The device path and the nsl-level raise come later; after this task both host and Zephyr builds still link, because `gpio_target_set`/`gpio_target_level` are defined unconditionally in `platform.c` for now.

**Files:**
- Modify: `~/workspace/tuvm/src/prim_ids.h` (append at end, before `#endif`)
- Modify: `~/workspace/tuvm/src/dispatch.c:767` (enum), `:819` (`PRIM_NAMES`), `:831` (parity guard)
- Modify: `~/workspace/tuvm/src/platform.c:57` (host table), `:916` (dispatch cases)
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl:2323` (seam forwarders)
- Test: `~/workspace/nsl-tests/13_pulse/02_gpio_set_level.nsl` (create)

**Interfaces:**
- Produces (C, non-static — Task 3 overrides these on Zephyr):
  - `int gpio_target_set(int pin, int level)` — returns `1` on success, `0` if the pin cannot be driven.
  - `int gpio_target_level(int pin)` — returns `0` or `1` for a declared pin, `-1` for an undeclared one.
- Produces (nsl, on the Kernel instance an app holds as `platform`):
  - `gpioSet: n to: aBool` → `true` on success, `nil` for an undeclared pin.
  - `gpioLevel: n` → `true` / `false`, or `nil` for an undeclared pin.

- [ ] **Step 1: Count `PRIM_NAMES` to derive the two new ids**

Run:
```bash
cd ~/workspace/tuvm && sed -n '771,830p' src/dispatch.c | grep -o '"[^"]*"' | wc -l
```
Expected: `122`

The two new ids are therefore **123** (`gpioSet:to:`) and **124** (`gpioLevel:`). If the command prints anything other than 122, the array bounds have moved — locate `static const char *const PRIM_NAMES[]` and its closing `};` and re-run over the correct line range. Use the number you counted, not 123/124.

- [ ] **Step 2: Write the failing test**

Create `~/workspace/nsl-tests/13_pulse/02_gpio_set_level.nsl`:

```smalltalk
(*
expect:
set true
level true
level false
input false
undeclared nil
finished
*)
(* tuvm#82 — the OUTBOUND half of the gpio seam, host differential form. Drives a
   pin and reads it back; reads a pin declared only as a pulse INPUT (host: a
   gpioPin: registration counts as declared, at level false, which is the host
   analogue of an overlay declaration); and asks about a pin nothing declared,
   which answers nil. This test uses the RAW kernel forwarders on purpose — the
   GpioPulse wrapper that turns that nil into a raise is the next task, and this
   test must keep passing after it lands. *)
class GpioSetLevel packageUsing: manifest = () (
  public main: platform args: args = (
    | space alert gpio |
    space:: platform space.
    alert:: space channel: 'sensor/alert'.

    gpio:: platform gpio.
    gpio pin: 15 edge: #falling post: #down on: alert.   (* declares 15 as an input *)

    ('set ' , (platform gpioSet: 13 to: true) printString) out.
    ('level ' , (platform gpioLevel: 13) printString) out.
    platform gpioSet: 13 to: false.
    ('level ' , (platform gpioLevel: 13) printString) out.
    ('input ' , (platform gpioLevel: 15) printString) out.
    ('undeclared ' , (platform gpioLevel: 99) printString) out.
    'finished' out.
  )
) : (
)
```

- [ ] **Step 3: Run the test to verify it fails**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/02_gpio_set_level.nsl B
```
Expected: `Path B: FAIL` — the selector `gpioSet:to:` does not exist, so the send is a doesNotUnderstand rather than a primitive.

- [ ] **Step 4: Append the two prim ids**

In `~/workspace/tuvm/src/prim_ids.h`, immediately before the closing `#endif /* OEVM_PRIM_IDS_H */`:

```c
/* tuvm#82 (MagTag LEDs): the OUTBOUND half of the gpio seam. gpioSet:to: drives a
   DECLARED pin to a LOGICAL level; gpioLevel: reads one back — and reads a button's
   held state, which is what lets chording cost zero mailbox traffic instead of the
   doubled event rate release events pay. Appended after spanReset; no pre-existing
   id shifts. 123/124 were arrived at by COUNTING PRIM_NAMES (122 entries before
   this append), not by 122+1 arithmetic, per the standing rule above.
   Both answer nil for a pin the board never declared. The raise is written in nsl
   (GpioPulse in kernel.nsl), not here — the same division Vector>>at: uses, and the
   only one available, since build_and_raise is static to dispatch.c. */
#define PRIM_pf_gpioSet   123
#define PRIM_pf_gpioLevel 124
```

- [ ] **Step 5: Append the enum slots, the names, and advance the parity guard**

In `~/workspace/tuvm/src/dispatch.c`, after `PRIM_pf_spanReset,` (line 767) and before the closing `};`:

```c
    /* tuvm#82: the outbound gpio pair — gpioSet:to: / gpioLevel:. Appended after
       spanReset; no pre-existing id shifts. Counted (not computed) to be the
       123rd/124th PRIM_NAMES entries. Platform-dispatched (mirrored in prim_ids.h). */
    PRIM_pf_gpioSet,
    PRIM_pf_gpioLevel,
```

In the same file, after `"spanReset",` (line 819) and before the closing `};`:

```c
    "gpioSet:to:", "gpioLevel:",
```

Then change the parity guard (line 831) from:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_spanReset) ? 1 : -1];
```

to:

```c
typedef char prim_names_enum_parity_check[(N_PRIM_NAMES == PRIM_pf_gpioLevel) ? 1 : -1];
```

- [ ] **Step 6: Add the host level table and the two C leaves**

In `~/workspace/tuvm/src/platform.c`, immediately after the closing `}` of `host_gpio_fire` (line 60) and **before** the `gpio_target_fire` comment block:

```c
/* tuvm#82: the host side of the OUTBOUND gpio seam. A pin counts as DECLARED on
   the host if it has been written by gpioSet:to: or registered by
   gpioPin:edge:post:on: — the host analogue of "declared in the board overlay".
   Deliberately a SEPARATE table from host_gpio above: firing an edge and driving
   a line are different things (the interesting chording test sets a level on one
   pin while firing an edge on another), and keeping them apart leaves the proven
   pulse path untouched.
   KNOWN HOST/DEVICE DIVERGENCE: on the host, gpioSet:to: CREATES a slot, so it
   never reports an undeclared pin; on target it fails for any pin the overlay
   omits. A typo'd output pin therefore passes on the host and raises on the
   board. Accepted: there is no overlay on the host to check against. The level
   read has no such gap and is where the raise gets its host coverage. */
#ifndef HOST_MAX_GPIO_LEVEL
#define HOST_MAX_GPIO_LEVEL 16
#endif
struct HostGpioLevel { int pin; int level; int live; };
static struct HostGpioLevel host_gpio_level[HOST_MAX_GPIO_LEVEL];
static int host_n_gpio_level = 0;

static struct HostGpioLevel *host_level_find(int pin) {
    int i;
    for (i = 0; i < host_n_gpio_level; i++)
        if (host_gpio_level[i].live && host_gpio_level[i].pin == pin)
            return &host_gpio_level[i];
    return NULL;
}
static int host_gpio_is_registered(int pin) {
    int i;
    for (i = 0; i < host_n_gpio; i++)
        if (host_gpio[i].live && host_gpio[i].pin == pin) return 1;
    return 0;
}

/* Answers 1 on success, 0 for a pin that cannot be driven (host: table full). */
int gpio_target_set(int pin, int level) {
    struct HostGpioLevel *e = host_level_find(pin);
    if (e) { e->level = level ? 1 : 0; return 1; }
    if (host_n_gpio_level >= HOST_MAX_GPIO_LEVEL) return 0;
    e = &host_gpio_level[host_n_gpio_level++];
    e->pin = pin; e->level = level ? 1 : 0; e->live = 1;
    return 1;
}

/* Answers 0/1 for a declared pin and -1 for an undeclared one. A pin registered
   as a pulse input but never driven reads false — an input that nothing has
   asserted, which is what an idle active-low button looks like. */
int gpio_target_level(int pin) {
    struct HostGpioLevel *e = host_level_find(pin);
    if (e) return e->level;
    if (host_gpio_is_registered(pin)) return 0;
    return -1;
}
```

- [ ] **Step 7: Add the two dispatch cases**

In `~/workspace/tuvm/src/platform.c`, immediately after the `case PRIM_pf_spanReset:` block's `return 1;` (line 916):

```c
    case PRIM_pf_gpioSet: {
        /* tuvm#82: `gpioSet: pin to: aBool` — args[0]=pin (SMI), args[1]=Boolean.
           LOGICAL level: on target this goes through gpio_pin_set_dt, so `true`
           means ACTIVE, and the overlay's GPIO_ACTIVE_LOW flags carry the polarity
           instead of every caller. Answers nil for a pin the board never declared;
           kernel.nsl's GpioPulse turns that nil into a tagged raise. */
        int lvl = (args[1] == true_symbol) ? 1 : 0;
        if (!gpio_target_set((int)UNTAG_SMI(args[0]), lvl)) { *out = NULL; return 1; }
        *out = true_symbol;
        return 1;
    }
    case PRIM_pf_gpioLevel: {
        /* tuvm#82: `gpioLevel: pin` — the LOGICAL level, so `true` reads as
           "button pressed" for an active-low button and "powered" for the
           active-low NeoPixel gate. nil for an undeclared pin (see above). */
        int lvl = gpio_target_level((int)UNTAG_SMI(args[0]));
        if (lvl < 0) { *out = NULL; return 1; }
        *out = lvl ? true_symbol : false_symbol;
        return 1;
    }
```

- [ ] **Step 8: Add the two raw kernel forwarders**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, immediately after the line `public gpioFire: n = ( ^ platform gpioFire: n )` (line 2323):

```smalltalk
  (* tuvm#82: the OUTBOUND half of the gpio seam. Thin forwarders, same shape as
     gpioPin:/gpioFire: above. Both answer nil for a pin the board never declared —
     the GpioPulse wrapper is what turns that nil into a tagged raise, so these stay
     raw and a test can observe the nil directly. *)
  public gpioSet: n to: aBool = ( ^ platform gpioSet: n to: aBool )
  public gpioLevel: n = ( ^ platform gpioLevel: n )
```

- [ ] **Step 9: Run the test to verify it passes**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/02_gpio_set_level.nsl B
```
Expected: `Path B: PASS`

If the build fails with `size of array 'prim_names_enum_parity_check' is negative`, the id count in Step 4 is wrong — recount and fix `prim_ids.h`, not the guard.

- [ ] **Step 10: Confirm the existing pulse test still passes**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/01_pulse_roundtrip.nsl B
```
Expected: `Path B: PASS` — the new level table must not have disturbed the registration/fire path.

- [ ] **Step 11: Commit (three repos)**

```bash
cd ~/workspace/tuvm && git add src/prim_ids.h src/dispatch.c src/platform.c && git commit -m "feat(gpio): the outbound seam — gpioSet:to: and gpioLevel:, host leaves (tuvm#82)

Ids 123/124 arrived at by COUNTING PRIM_NAMES (122 entries before this
append), per the standing rule in prim_ids.h; the parity guard advanced to
PRIM_pf_gpioLevel so a miscount is a build error.

Both answer nil for a pin the board never declared. The host table is
separate from host_gpio on purpose: firing an edge and driving a line are
different things, and the chording test needs both on different pins.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(kernel): raw gpioSet:to:/gpioLevel: seam forwarders (tuvm#82)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd ~/workspace/nsl-tests && git add 13_pulse/02_gpio_set_level.nsl && git commit -m "test(gpio): drive a pin, read it back, and observe nil for an undeclared one (tuvm#82)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: The `GpioPulse` wrapper and the tagged raise

Delivers the API nsl nodes actually use — `gpio set: n to: aBool` and `gpio level: n` — with an undeclared pin raising `#gpioUndeclared` instead of answering a plausible-but-wrong level.

**Files:**
- Modify: `~/workspace/nsl-tuvm/lib/kernel.nsl:2092` (inside `class GpioPulse`, after `fire:`)
- Test: `~/workspace/nsl-tests/13_pulse/03_gpio_undeclared_raises.nsl` (create)

**Interfaces:**
- Consumes: `gpioSet: n to: aBool` and `gpioLevel: n` on the Kernel instance (Task 1), both `nil`-answering.
- Produces (on the object an app gets from `platform gpio`):
  - `set: n to: aBool` → the `GpioPulse` (for chaining); raises `#gpioUndeclared` on an undeclared pin.
  - `level: n` → `true` / `false`; raises `#gpioUndeclared` on an undeclared pin.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/13_pulse/03_gpio_undeclared_raises.nsl`:

```smalltalk
(*
expect:
led true
caught gpioUndeclared
text gpio pin not declared
finished
*)
(* tuvm#82 — an undeclared pin is a WRONG LITERAL, not a load condition, so it
   raises rather than answering false and letting the node carry on with a value
   that is simply incorrect. The check lives in nsl over a nil-answering C leaf,
   exactly as Vector>>at: bounds-checks over a raw basicAt:. The tag is asserted,
   not merely the fact that something failed — an untagged catch would pass even
   if the raise came from somewhere else entirely. *)
class GpioUndeclaredRaises packageUsing: manifest = () (
  public main: platform args: args = (
    | gpio |
    gpio:: platform gpio.
    gpio set: 13 to: true.
    ('led ' , (gpio level: 13) printString) out.

    [ gpio level: 99 ]
      on: [ :e | e tag = #gpioUndeclared ]
      do: [ :e | 'caught gpioUndeclared' out.
                 ('text ' , (e at: #text ifAbsent: [ '?' ])) out ].
    'finished' out.
  )
) : (
)
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/03_gpio_undeclared_raises.nsl B
```
Expected: `Path B: FAIL` — `GpioPulse` understands neither `set:to:` nor `level:`.

- [ ] **Step 3: Add the two wrapper methods**

In `~/workspace/nsl-tuvm/lib/kernel.nsl`, inside `class GpioPulse`, immediately after the line `public fire: n = ( seam gpioFire: n. ^ self )   (* host/test only *)` (line 2092):

```smalltalk
    (* tuvm#82: the outbound direction. A pin the board never declared RAISES
       rather than answering a plausible-but-false level — it is a wrong literal in
       the source, not something traffic can produce, and a silent false would show
       up as an LED that merely looks broken. The C leaf stays raw and answers nil;
       the check lives here, the same division Vector>>at: uses over basicAt:. The
       tag is what makes `on: [ :e | e tag = #gpioUndeclared ]` possible for code
       that is deliberately probing an unknown board. *)
    public set: n to: aBool = (
      (seam gpioSet: n to: aBool) == nil
        ifTrue: [ ^ self error: 'gpio pin not declared' tag: #gpioUndeclared ].
      ^ self )
    public level: n = (
      | v |
      v:: seam gpioLevel: n.
      v == nil ifTrue: [ ^ self error: 'gpio pin not declared' tag: #gpioUndeclared ].
      ^ v )
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/03_gpio_undeclared_raises.nsl B
```
Expected: `Path B: PASS`

- [ ] **Step 5: Confirm Task 1's test still passes**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/02_gpio_set_level.nsl B
```
Expected: `Path B: PASS` — the raw forwarders must still answer nil, not raise.

- [ ] **Step 6: Run the full suite**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run_tests_oevm.py
```
Expected: 0 failed. **Read the aux gate line as well as the failure count** — the suite prints "0 failed" while aux gates can be red (tuvm#63).

- [ ] **Step 7: Commit (two repos)**

```bash
cd ~/workspace/nsl-tuvm && git add lib/kernel.nsl && git commit -m "feat(kernel): gpio set:to:/level: raise #gpioUndeclared on an undeclared pin (tuvm#82)

The raise is written in nsl over a nil-answering C leaf — build_and_raise is
static to dispatch.c and unreachable from platform.c, and this is the division
Vector>>at: already uses. An undeclared pin is a wrong literal, not a load
condition, so answering false would hand the caller a value that is simply
wrong and let the node carry on.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"

cd ~/workspace/nsl-tests && git add 13_pulse/03_gpio_undeclared_raises.nsl && git commit -m "test(gpio): assert the #gpioUndeclared tag, not merely that something failed (tuvm#82)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: The device path — devicetree aliases, the outputs table, and the `__ZEPHYR__` split

Delivers the real hardware implementation. At the end of this task both the host suite and the ESP32-S2 firmware build; hardware verification is Task 4.

**Files:**
- Modify: `~/workspace/tuvm/src/platform.c` (wrap the host leaves added in Task 1 in `#ifndef __ZEPHYR__`, add the extern declarations)
- Modify: `~/workspace/tuvm/zephyr/src/main.c:205` (hoist the gpio include), `:401` (outputs table + leaves), `:481` (call the wiring)
- Modify: `~/workspace/tuvm/zephyr/boards/esp32s2_saola_esp32s2.overlay:113` (two aliases)

**Interfaces:**
- Consumes: `int gpio_target_set(int pin, int level)` and `int gpio_target_level(int pin)` — the exact signatures and return conventions from Task 1. This task supplies the Zephyr definitions.
- Produces: nothing new at the nsl level. `gpio set:to:` / `gpio level:` gain a real hardware implementation with identical semantics.

- [ ] **Step 1: Add the two devicetree aliases**

In `~/workspace/tuvm/zephyr/boards/esp32s2_saola_esp32s2.overlay`, inside the `aliases { ... }` block, after the `neopixelpower = &neopixel_power;` line:

```
		/* tuvm#82 — the OUTBOUND seam. gpioout* is to outputs what pulsegpio* is
		 * to inputs: ../src/main.c compiles the table away entirely on boards
		 * that declare none, so the nRF52840-DK and dongle are untouched. The
		 * ALIAS NAMES NEVER REACH nsl — a node names the silkscreen GPIO number
		 * (13 for the red LED, 21 for the NeoPixel power gate), which main.c
		 * matches against each spec's own .pin. */
		gpioout  = &red_led;
		gpioout1 = &neopixel_power;
```

Note both aliases point at nodes that already exist in this file, and `magtagled` / `neopixelpower` stay — they cost nothing and document the pins' identity.

- [ ] **Step 2: Hoist the gpio include so it is unconditional**

In `~/workspace/tuvm/zephyr/src/main.c`, `#include <zephyr/drivers/gpio.h>` currently sits at line 205, *inside* `#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))`. The outputs table needs it on a board that declares outputs but no buttons. Move that `#include` line to sit with the other top-of-file includes, above the `#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))` block. Header guards make this safe; do not leave a duplicate behind.

- [ ] **Step 3: Add the outputs table, the wiring, and the two device leaves**

In `~/workspace/tuvm/zephyr/src/main.c`, immediately after the `#endif` that closes the `pulsegpio` block (the one following `static inline void wire_pulse_gpio(void) { }   /* native_sim: no physical button */`, around line 401):

```c
/* tuvm#82: the OUTBOUND half of the gpio seam. Outputs are declared as gpioout /
   gpioout1..3 aliases, exactly as inputs are declared as pulsegpio / pulsegpio1..3,
   and the whole table compiles away on a board that declares none. An nsl node
   names the SILKSCREEN gpio number; the alias names never leave this file, which is
   why the lookup below matches on spec.pin. */
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
static const struct gpio_dt_spec gpio_outputs[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout), gpios),
#if DT_NODE_EXISTS(DT_ALIAS(gpioout1))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout1), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(gpioout2))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout2), gpios),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(gpioout3))
    GPIO_DT_SPEC_GET(DT_ALIAS(gpioout3), gpios),
#endif
};
#define GPIO_N_OUTPUTS ARRAY_SIZE(gpio_outputs)

static void wire_gpio_outputs(void) {
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++) {
        if (!gpio_is_ready_dt(&gpio_outputs[i])) {
            printk("gpioout[%u] device not ready\n", (unsigned)i);
            continue;   /* one dead pin must not cost the others */
        }
        /* INACTIVE, not ACTIVE. On the MagTag gpioout1 is the NeoPixel POWER GATE,
           declared GPIO_ACTIVE_LOW, and coming up powered is exactly the quiescent
           draw the vendor guide says to avoid. INACTIVE therefore drives that line
           physically HIGH — the opposite of what the flag's name suggests, and
           correct. Do not "fix" this to GPIO_OUTPUT_ACTIVE. */
        gpio_pin_configure_dt(&gpio_outputs[i], GPIO_OUTPUT_INACTIVE);
    }
}
#else
static inline void wire_gpio_outputs(void) { }
#endif

/* The two leaves src/platform.c calls under #ifdef __ZEPHYR__. Return conventions
   match the host definitions they replace: set answers 1/0, level answers 0/1/-1,
   where -1 means "no board declared this pin" and becomes nil in nsl.
   LOGICAL levels throughout (gpio_pin_set_dt / _get_dt), so the overlay's
   GPIO_ACTIVE_LOW flags carry the polarity: true == NeoPixels POWERED, and
   true == button PRESSED. */
int gpio_target_set(int pin, int level) {
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++)
        if ((int)gpio_outputs[i].pin == pin)
            return gpio_pin_set_dt(&gpio_outputs[i], level) == 0;
#endif
    ARG_UNUSED(pin); ARG_UNUSED(level);
    return 0;   /* not declared as an output on this board */
}

int gpio_target_level(int pin) {
#if DT_NODE_EXISTS(DT_ALIAS(gpioout))
    for (size_t i = 0; i < GPIO_N_OUTPUTS; i++)
        if ((int)gpio_outputs[i].pin == pin) {
            int v = gpio_pin_get_dt(&gpio_outputs[i]);
            return (v < 0) ? -1 : (v != 0);
        }
#endif
#if DT_NODE_EXISTS(DT_ALIAS(pulsegpio))
    /* Searching the INPUT table too is the whole chording payoff: a node can ask
       "is BUTTON_A held right now" for zero mailbox traffic, instead of tracking
       state from the doubled press/release event stream. The table already exists
       and already holds the button specs, so this costs nothing. */
    for (size_t i = 0; i < PULSE_N_INPUTS; i++)
        if ((int)pulse_inputs[i].spec.pin == pin) {
            int v = gpio_pin_get_dt(&pulse_inputs[i].spec);
            return (v < 0) ? -1 : (v != 0);
        }
#endif
    ARG_UNUSED(pin);
    return -1;
}
```

- [ ] **Step 4: Call the wiring at boot**

In `~/workspace/tuvm/zephyr/src/main.c`, immediately after the existing line in `main()`:

```c
    wire_pulse_gpio();   /* Task 8 (DK): arm BUTTON1 as the pulse edge; no-op on native_sim */
```

add:

```c
    wire_gpio_outputs();   /* tuvm#82: configure declared outputs INACTIVE; no-op where none */
```

- [ ] **Step 5: Guard the host leaves so the Zephyr build uses the device ones**

In `~/workspace/tuvm/src/platform.c`, wrap the entire block added in Task 1 Step 6 — from the `#ifndef HOST_MAX_GPIO_LEVEL` line through the closing `}` of `gpio_target_level` — in:

```c
#ifndef __ZEPHYR__
/* ... the whole Task 1 Step 6 block, unchanged ... */
#else
/* tuvm#82: on target these live in zephyr/src/main.c, beside the devicetree tables
   they read. Conditional compilation in a shared src/*.c rather than a new backend
   .c pair is deliberate: __ZEPHYR__ is already used this way in src/gc.c and
   src/vm.c, and adding a .c file costs edits to nine enumerated source lists
   across two repos (tuvm#63) for two functions.
   Note the direction is the MIRROR of gpio_target_fire below, which platform.c
   defines and main.c calls inbound. */
extern int gpio_target_set(int pin, int level);
extern int gpio_target_level(int pin);
#endif
```

The whole host block must go inside the `#ifndef`, helpers included — leaving `host_level_find` or `host_gpio_is_registered` outside would produce unused-function warnings under `-Wall` on the device build.

- [ ] **Step 6: Verify the host build and suite are unaffected**

Run:
```bash
cd ~/workspace/nsl-tuvm && python3 tools/run-one.py ../nsl-tests/13_pulse/02_gpio_set_level.nsl B && python3 tools/run-one.py ../nsl-tests/13_pulse/03_gpio_undeclared_raises.nsl B
```
Expected: `Path B: PASS` for both — `__ZEPHYR__` is not defined on the host, so nothing changed there.

- [ ] **Step 7: Verify the ESP32-S2 firmware builds**

Run:
```bash
cd ~/workspace/tuvm/zephyr && bash run.sh --board esp32s2 --model B --node MagTagChordNode.nsl
```
Expected: the build completes and prints the "Found devicetree overlay: .../boards/esp32s2_saola_esp32s2.overlay" line. Do **not** pass `--flash` yet.

If the build reports `gpio_outputs` undeclared, the overlay aliases were not picked up — check that overlay line in the build log first, because a missing overlay is silent otherwise.

- [ ] **Step 8: Commit**

```bash
cd ~/workspace/tuvm && git add src/platform.c zephyr/src/main.c zephyr/boards/esp32s2_saola_esp32s2.overlay && git commit -m "feat(magtag): drive real pins — gpioout aliases, DT outputs table, logical levels (tuvm#82)

gpioout/gpioout1..3 is to outputs what pulsegpio/pulsegpio1..3 is to inputs,
so boards declaring none compile the table away. gpio_pin_set_dt/get_dt keep
polarity in the overlay: set: 21 to: true means NeoPixels POWERED though the
gate is ACTIVE_LOW, and level: 15 true means BUTTON_A PRESSED.

gpio_target_level searches the INPUT table too — that is the chording payoff,
and it costs nothing because pulse_inputs already holds the button specs.

Host/device selection rides #ifdef __ZEPHYR__, already precedent in gc.c and
vm.c, so no new .c file and no nine-source-list edit (tuvm#63).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Hardware gates and the close-out promotions

Delivers the two on-hardware proofs the design names, plus the documentation promotions that close the issue.

**Files:**
- Create: `~/workspace/tuvm/zephyr/MagTagLedNode.nsl`
- Modify: `~/workspace/tuvm/review/program-sequence.md`
- Modify: `~/workspace/tuvm/review/tests/CLAIMS.md`
- Modify: `~/workspace/tuvm/zephyr/docs/BOARD_PORTS.md` (if the MagTag row is still absent — see the companion design's close-out)

**Interfaces:**
- Consumes: `gpio set: n to: aBool` and `gpio level: n` from Task 2, with the device implementation from Task 3.
- Produces: no code interface. Documentation of record.

- [ ] **Step 1: Write the device node**

Create `~/workspace/tuvm/zephyr/MagTagLedNode.nsl`:

```smalltalk
(*
MagTagLedNode — the two hardware gates for tuvm#82.

GATE 1 (output): the red LED on GPIO13 follows BUTTON_A. Press A, the LED lights;
press A again, it goes out.

  The intended state is held in `ledOn` and is what drives the toggle. The node ALSO
  prints what `gpio level: 13` reads back, but never steers on it — reading back an
  output returns the driven state via the input register, and nothing guarantees the
  esp32 driver leaves the input path enabled on a GPIO_OUTPUT pin (design Risk 1).
  Steering on the readback would make a driver quirk look like a broken LED. Printing
  it answers the readback question on real hardware at zero risk to the gate.

GATE 2 (level read): on every BUTTON_B press the node prints the held state of all
four buttons, read via `gpio level:` rather than tracked from events. Hold A and C
down, tap B, and the line reads held=AC. That is chording with ZERO mailbox traffic —
MagTagChordNode.nsl gets the same answer by paying a doubled event rate.

The NeoPixel power gate on GPIO21 is driven once at startup, purely to prove it is
reachable. NOTHING LIGHTS — the NeoPixels are WS2812 devices needing a ws2812/RMT
driver, which is a separate issue. A dark strip here is the expected result.

  BUTTON_A 15   BUTTON_B 14   BUTTON_C 12   BUTTON_D 11   red LED 13   npx power 21

expect — press A, press A, hold A+C and tap B:
led on   readback true
led off  readback false
held=AC
*)
class MagTagLedNode packageUsing: manifest = (
  | ledOn ::= false. |
) (
  heldOf: gpio = (
    | s |
    s:: ''.
    (gpio level: 15) ifTrue: [ s:: s , 'A' ].
    (gpio level: 14) ifTrue: [ s:: s , 'B' ].
    (gpio level: 12) ifTrue: [ s:: s , 'C' ].
    (gpio level: 11) ifTrue: [ s:: s , 'D' ].
    ^ s
  )

  public main: platform args: args = (
    | space gpio aDn bDn |
    space:: platform space.
    gpio:: platform gpio.

    (* Prove the power gate is reachable. Nothing lights; see the header. *)
    gpio set: 21 to: true.

    aDn:: space channel: 'btn/a/down'.
    bDn:: space channel: 'btn/b/down'.

    aDn whenever: '' do: [ :m |
      ledOn:: ledOn not.
      gpio set: 13 to: ledOn.
      ((ledOn ifTrue: [ 'led on ' ] ifFalse: [ 'led off' ])
        , '  readback ' , (gpio level: 13) printString) println ].

    bDn whenever: '' do: [ :m | ('held=' , (self heldOf: gpio)) println ].

    gpio pin: 15 edge: #falling post: 'A' on: aDn.
    gpio pin: 14 edge: #falling post: 'B' on: bDn.

    [ true ] whileTrue: [ platform pumpEvents. space drain ]
  )
) : (
)
```

- [ ] **Step 2: Build and flash**

Run:
```bash
cd ~/workspace/tuvm/zephyr && bash run.sh --board esp32s2 --model B --node MagTagLedNode.nsl --flash
```

Flashing the MagTag is **manual**: hold BOOT0, tap RESET, release BOOT0 to enter the bootloader before the flash step runs. **Never trust a `/dev/ttyUSB` number** — identify the board with `esptool read_mac` first. Zephyr takes over the native USB, so the **soldered UART header is the only console**.

- [ ] **Step 3: Verify gate 1 — the LED follows BUTTON_A**

Open the console (a pyserial reader holding one handle open; `cat` reads zero bytes, and *any* open of the port reboots the board, so open it once and leave it). Press BUTTON_A twice.

Expected console output:
```
led on   readback true
led off  readback false
```
Expected physically: the red LED beside the buttons lights on the first press and goes out on the second. **The physical LED is the gate.**

The `readback` field is an observation, not a pass condition. If it disagrees with the `led on`/`led off` word, the esp32 driver is not leaving the input path enabled on an output pin — record that in the CLAIMS row (Step 6) and in the design's Risk 1, because it makes `level:` output-readback unusable and the follow-up chording work must not rely on it. The gate still passes.

If the LED never lights but the console prints `led on`, the overlay aliases are not in the image — check the build log's devicetree-overlay line.

- [ ] **Step 4: Verify gate 2 — held state read without events**

Hold BUTTON_A and BUTTON_C down together, and while holding both, tap BUTTON_B.

Expected console output:
```
held=AC
```

Then release everything and tap BUTTON_B alone. Expected `held=B` or `held=`, depending on whether B is still physically down when the reaction runs — either is a pass. What this gate proves is that `level:` reports *other* buttons' state with **no release events registered anywhere in the node**: `MagTagLedNode` registers only pins 15 and 14, and never 115 or 114.

- [ ] **Step 5: Confirm the deferred path is untouched**

Run:
```bash
cd ~/workspace/tuvm/zephyr && bash run.sh --board esp32s2 --model B --node MagTagChordNode.nsl --flash
```
Press A, hold, press and release B, release A. Expected the unchanged output from a6656d3:
```
A down   held=A
B down   held=AB
B up     held=A
A up     held=
```
This proves the release-event chording path still works, which is what makes deferring its migration safe.

- [ ] **Step 6: Promote to the mainline documents**

Per the project's promotion rule, the issue is not closed until the durable knowledge lands in a document of record. Make these edits:

1. `~/workspace/tuvm/review/program-sequence.md` — record that the gpio seam gained an outbound direction and a level read, and that the MagTag's red LED and NeoPixel power gate are drivable from nsl. Read the two most recent rung entries first and match their format; the file's shape is the authority, not this plan.

2. `~/workspace/tuvm/review/tests/CLAIMS.md` — one row, appended to the existing `| # | Spec §7 criterion | Proven by | Status |` table. Use the `| — |` first column that every recent row uses, and follow the house style: bold the headline claim, name the enforcing source files, and cite the design doc. Draft:

```markdown
| — | **A pin can be driven and read from nsl, and a pin the board never declared raises instead of lying** (tuvm#82) | `nsl-tests/13_pulse/02_gpio_set_level.nsl` (drive + read back; a pin declared only as a pulse input reads false; an undeclared pin answers nil at the raw seam) and `03_gpio_undeclared_raises.nsl` — **THE HOST GATE** (`gpio level:` on an undeclared pin raises with tag `#gpioUndeclared`, asserted BY TAG so a raise from anywhere else fails the test); on hardware `tuvm/zephyr/MagTagLedNode.nsl` — gate 1: the red LED on GPIO13 follows BUTTON_A; gate 2: held state for all four buttons read via `gpio level:` on ONE press with no release pins registered (chording for zero mailbox traffic, where `MagTagChordNode.nsl` pays a doubled event rate). Enforced by `nsl-tuvm/lib/kernel.ns` `GpioPulse>>set:to:`/`level:` (the declared-ness check is nsl policy, as `checkPath:` is) atop `tuvm/src/platform.c` prims 123/124 + `tuvm/zephyr/src/main.c`'s `gpioout*` devicetree table, driven through `gpio_pin_set_dt`/`_get_dt` so the overlay's `GPIO_ACTIVE_LOW` flags carry polarity rather than every caller. Design: `tuvm/docs/design/2026-07-23-gpio-output-level-seam-design.md` | DONE |
```

Correct any detail this row asserts that hardware contradicted — in particular the output-readback observation from Step 3.

3. `~/workspace/tuvm/zephyr/docs/BOARD_PORTS.md` — if the MagTag row is still missing, add it, including that the Zephyr SDK needs `xtensa-espressif_esp32s2_zephyr-elf` installed (outside the repo, invisible to git).

Do **not** edit anything else under `review/` — that tree is David's review-agent tree.

- [ ] **Step 7: Open the deferred-chording follow-up issue**

```bash
gh issue create --title "Migrate chording from release events to gpio level: (follows tuvm#82)" --body "$(cat <<'EOF'
`gpio level:` landed in tuvm#82 and is proven on the MagTag (`zephyr/MagTagLedNode.nsl`,
gate 2: held state read with no release events registered).

That makes the release-event path in a6656d3 redundant. It works — `MagTagChordNode.nsl`
prints `held=AB` correctly — but it fires a second logical pin per button
(`GPIO + OEVM_BUTTON_RELEASE_BASE`, 100), doubling the event rate for information the
node can now simply ask for when it cares.

**Scope**
- Rewrite `zephyr/MagTagChordNode.nsl` to read held state via `gpio level:` on a press,
  instead of registering eight logical pins and tracking flags.
- Remove `OEVM_BUTTON_RELEASE_BASE` and the release-fire branch from `zephyr/src/main.c`
  once nothing registers a release pin.
- Confirm on hardware that the chord output is unchanged before deleting anything.

Deliberately deferred out of tuvm#82: replacing a proven seam in the same rung that first
introduces its replacement risks losing both. See
`docs/design/2026-07-23-gpio-output-level-seam-design.md` D7.
EOF
)"
```

- [ ] **Step 8: Commit and close**

```bash
cd ~/workspace/tuvm && git add zephyr/MagTagLedNode.nsl review/program-sequence.md review/tests/CLAIMS.md zephyr/docs/BOARD_PORTS.md && git commit -m "feat(magtag): the LED rung proven on hardware — output and level read (tuvm#82)

Gate 1: the red LED on GPIO13 follows BUTTON_A, toggled by reading back the
pin just written — which is why no toggle: prim was added.
Gate 2: held state for all four buttons read via gpio level: on a single
press, with no release events registered. Chording for zero mailbox traffic.

The NeoPixel power gate is driven once to prove it is reachable. Nothing
lights: WS2812 needs a ws2812/RMT driver, tracked separately.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

Then comment the outcome on tuvm#82 (the tracker is the status of record) and close it, linking the follow-up issue from Step 7.

---

## Notes for the implementer

- **The MagTag console is the only window.** Zephyr claims the native USB, so if the soldered UART header is not connected you will see nothing at all and it will look like a boot failure. A silent port proves nothing on its own — esptool's post-flash reset eats the boot banner.
- **Any open of the serial port reboots the board.** Open one pyserial handle and hold it; do not stack `stty` calls or repeated opens while debugging.
- **If a button seems dead**, check the build log for the devicetree-overlay line before suspecting the code. A missing overlay is completely silent and compiles out three of the four buttons.
- **`gpio level:` on an output reads back the driven state, not the electrical reality of the pin.** It is not a fault detector. Fine for the LED; worth knowing before anyone reads more into it.
