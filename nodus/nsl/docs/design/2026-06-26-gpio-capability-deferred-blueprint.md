# GPIO capability — deferred blueprint (do on Zephyr, not host-mock)

**Status:** DEFERRED by David, 2026-06-26. Not building a host-mock GPIO.
GPIO will be implemented **directly on real Zephyr hardware** at the R6 rung
(hardware bring-up), reusing the confined-capability pattern proven by `Serial`.
This file preserves the recon/blueprint so the later build is a fill-in, not a
rediscovery.

**Why deferred:** a host GPIO mock can only fake pin state — unlike networking
(POSIX gives the desktop *real* sockets), GPIO has no faithful desktop backend,
so the host-mock would prove the cap-machinery (already proven twice by `Serial`
+ platform extraction) while adding no fidelity. David redirected the active rung
to **networking (UDP)**, where the host backend is genuine. GPIO waits until there
are real pins (R6).

## The pattern GPIO follows (identical to `Serial`, the first native cap tier)

GPIO is a **native, confined capability**, NOT ambient like `platform`. It rides
the exact spine shipped for `Serial` (std-lib step 3c-3b) and refined by the
`platform.c` extraction — one capability = one C module + one receiver hook +
one thin `.ns` wrapper, granted by boot injection.

### 1. C primitive module — `oevm/src/gpio.c` (mirror `serial.c`)
- `oop gpio_object(void)` — static singleton factory; lazily `gpio_open()`s the
  backend and sets `GpioVtable.base.vtable = VtableVtable`.
- `oop gpio_dispatch(oop selector, int argc, oop *args)` — **receiver-identity
  dispatch**, NOT a numbered prim id. Compares `symbol_name((struct Symbol*)selector)`
  by string; reads `oop *args`; returns `oop` (`TAG_SMI(n)` / `NULL`=nil /
  `&true_sym_obj`/`&false_sym_obj` / `&GpioObj`). Adds nothing to `PRIM_NAMES`.

### 2. Link-time backend seam — `oevm/src/gpio_backend.h` (mirror `serial_backend.h`)
Exactly one impl linked per build from `src/backend/$OEVM_TARGET/`. Suggested seam
(digital-first, what `Serial`'s shape implies for GPIO):
```c
void gpio_open(void);                 /* idempotent init at boot */
void gpio_set_mode(int pin, int mode);/* 0=input,1=output,2=input-pullup,3=input-pulldown */
void gpio_write(int pin, int level);  /* level 0/1 */
int  gpio_read(int pin);              /* 0/1 */
```
- Host: `oevm/src/backend/posix/gpio_posix.c` — in-memory pin-state array (would
  only ever be a mock; the reason this is deferred).
- Device (the real target): `oevm/src/backend/zephyr/gpio_zephyr.c` — wraps Zephyr
  `gpio_pin_configure_dt` / `gpio_pin_set` / `gpio_pin_get` over devicetree specs.
  **This is the file the R6 build actually wants.**

### 3. Dispatch hook — `oevm/src/dispatch.c` (mirror serial's line ~619-621)
```c
if (receiver == gpio_object()) return gpio_dispatch(selector, argc, args);
```
Early receiver-identity gate (after BoundClass, before selector interning).
`mark()` skips static BSS (`gc.c` `is_valid_heap_object`) → no new GC root for
`&GpioObj`.

### 4. Newspeak facade — `ns-oevm/lib/kernel.ns` (mirror the `Serial` class)
- Decide facade shape at build time (was the open GPIO design question):
  likely a `Gpio` controller that **vends confined `Pin` objects**
  (`gpio pin: 13` → a `Pin` with `mode:`, `high`, `low`, `read`, `value:`), so the
  roadmap's "Newspeak `Pin` facade" holds. Pin slot holds the pin number; the
  `Gpio` wrapper's slot holds `gpio_object()`; `Pin` reaches the native singleton
  through its `Gpio`.
- Minted by a **non-public** `Kernel>>gpioOn:` (no public accessor → confined).

### 5. Grant = boot-injected sibling capability (mirror serial threading)
- Entry signature `class Main usingPlatform: platform gpio: gpio = (...)`.
- **Path A** (`ns-oevm/tools/ns_compile_oevm.py`, the `~1940-1970` boot region):
  thread `gpio` as Nth cap by entry signature, guarded by `"gpio" in params`;
  1-cap / serial-only entries untouched.
- **Path B** (`.oimg`): serializer (`oevm/tools/oimg_serialize.py`) emits a
  `gpio_on_sel_ref` header word for gpio-declaring entries; bump
  **format_version** (was 4 after serial; whatever is current +1); loader
  (`oevm/src/oimg_loader.c`) reads it and mints the SAME wrapper via
  `send(kernel_inst, gpioOn:, {gpio_object()})`. Non-gpio images stay byte-identical
  at the lower version.
- **Confinement proof = runtime DNU:** an entry without `gpio:` that names `gpio`
  compiles (bare unary self-send) but aborts at runtime ("Message 'gpio' not
  understood"). Pin with an `expect-runtime-fail:` test.

### 6. Tests (mirror `38_serial.ns` / `39_serial_confined.ns`)
- `ns-tests/04_somns_basic/4N_gpio.ns` — capability round-trip (on host: write a
  pin, read it back; on device: blink/loopback against a real pin pair).
- `ns-tests/04_somns_basic/4N_gpio_confined.ns` — `expect-runtime-fail:` DNU when
  `gpio` undeclared.
- `oevm/tests/test_gpio.c` — backend-only C smoke test (self-documenting build cmd).

### 7. Verify-first probes that paid off for serial (do the same)
- NS parser rejects inline `"..."` comments — use `(* ... *)`.
- `platform print:` dispatches only on the native C platform object, not the
  Kernel-wrapper instance the entry gets as `platform` — use `println`.
- Trace serializer/loader header offsets independently and confirm equal before
  trusting Path-A/Path-B parity.

## Open design question carried forward (decide at build time)
Facade shape: `Gpio`-vends-`Pin` (recommended, matches roadmap "Pin facade") vs.
flat `gpio pinMode:/digitalWrite:/digitalRead:`. Plus the digital-only vs.
analog/PWM scope question that was never answered (was the question David
redirected away from). Decide both when GPIO is actually built on Zephyr, against
real hardware constraints.

## Source decomposition (per oevm#10, Option A)
`gpio.c` + `gpio_backend.h` are **language-neutral oevm core** (shared by a future
`st-oevm`); `gpio_zephyr.c` lives in `oevm/src/backend/zephyr/` (language-neutral
target backend); only the `.ns` `Pin`/`Gpio` facade + boot wiring is ns-specific.
