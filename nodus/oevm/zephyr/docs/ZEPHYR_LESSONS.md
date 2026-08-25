# Zephyr / NCS + OpenThread + USB — hard-won lessons (nRF52840)

Consolidated bring-up lessons for running the tuvm/oevm VM on ZephyrOS + nRF
Connect SDK on Nordic nRF52840 hardware, so tuvm is **self-contained** and does
not depend on reading the sibling repos. Harvested (2026-07-11, rung-7) from two
prior generations of the same VM-on-Zephyr work — cite them for the primary source:

- **`~/workspace/st-zephyr`** — Berry/Smalltalk "jast" agent. **The proven original recipe** (works on the dongle under radio).
- **`~/workspace/ns-zephyr`** — Newspeak/OEVM "ns" node. The port that **re-derived** the recipe; its dated docs record the actual root causes.

Toolchain baseline (both, and tuvm rung-7): **NCS v3.2.4, Zephyr v4.2.99, Zephyr SDK 0.17.0.**

> **Framing that saves weeks:** the "node boots silent, no console" symptom has
> *many* distinct causes (USB-CDC-under-radio, MPU stack fault, 30 s NET_CONFIG
> block, blocking flash erase, OT auto-start on the boot path) that all look
> identical because there is no console up yet to report the fault. **Get SWD/RTT
> observability first** (J-Link, or a DK wired to the dongle's SWD pads) before
> blind-flashing.

---

## 0. tuvm rung-7 delta (what we hit, 2026-07-11)

The nRF52840-**DK** node (`ThreadEchoNodeDK.nsl` + `prj_dk_thread.conf` + `src/main.c`
`thread_bringup()`) rounds-trips a CBOR Record over real OpenThread → `BRIDGE OK`.
The one root cause we re-derived on NCS v3.2.4:

- **`openthread_run()` enables OT but never `net_if_up()`s the Zephyr `net_if`.**
  With `MANUAL_START=y` (or even auto-start) and `NET_CONFIG_SETTINGS=n`, OT
  attaches (so `ot-ctl ping` + child table work — those are OT-internal ICMP) but
  `zsock_sendto` fails **`ENETDOWN` (errno 115)** because nothing admin-ups the
  Zephyr socket iface. Fix in `main.c thread_bringup()`: `openthread_run()` **+**
  `net_if_up(openthread_get_default_context()->iface)` + poll `otThreadGetDeviceRole`
  until CHILD. (ns's fix-plan draft listed `net_if_up` but the shipped ns `main_b.c`
  relied on the L2 to do it — on v3.2.4 it does not; call it explicitly.)
- The DK attached with the **Kconfig dataset alone** (via the gitignored
  `prj_dk_thread_local.conf`), *not* needing the full active-dataset TLV blob that
  ns required (see §2). If a future mesh/build fails to attach, that is the first
  thing to escalate to (`otDatasetSetActiveTlvs`).

---

## 1. USB-CDC console (the dongle's problem; the DK sidesteps it)

- **The ~3 s settle at the top of `main()` is mandatory on USB-CDC boards.** USB
  CDC-ACM must enumerate before any output or the run-once program's early prints
  are lost. jast: `k_msleep(3000)` after a boot LED blink; ns Model B:
  `k_sleep(K_SECONDS(3))` as the **first** statement, before anything touches
  OT/settings/PSA; ns Model A (non-OT) uses 2 s. **Use 3 s once OT is present.**
  The **DK does not need it** (console is uart0/VCOM, not SoC USB-CDC).
- **`CONFIG_USBD_CDC_ACM_WORKQUEUE=y` — required on the dongle.** Without it,
  OpenThread starves the system workqueue and the CDC console goes silent /
  never enumerates. Offloading CDC to its own workqueue keeps it serviced while
  OT is busy at boot.
- **`uart_irq_rx_enable()` before `uart_poll_in()`** on the USBD-next stack — it
  won't queue OUT transfers until RX is enabled (else CDC enumerates but you can't
  read input).
- **`CONFIG_UART_LINE_CTRL=y` + DTR gating.** First console write blocks until the
  host opens the port (run-once output preserved). **Consumer must assert DTR to
  flush — plain `cat` does NOT.** Read the dongle console with pyserial `dtr=True`,
  not `cat`. jast uses `uart_line_ctrl_get(...DTR...)` to detect a terminal and
  switch transports.
- **Console split — the central hardware fact:**
  - **DK (nrf52840dk):** console on **uart0/VCOM via onboard J-Link** (`1366:1024`).
    Independent of the radio, immune to Issue A. **Use the DK for Thread work.**
  - **Dongle (PCA10059):** no debugger; console on the **SoC's own USB-CDC**,
    flashed via DFU. USBD-next CDC enumerates as VID **`2fe3`** (app) — DFU
    bootloader is `1915:521f`, a provisioned Nordic OT device is `1915:cafe`.
- **✅ "Issue A" — SOLVED 2026-07-11. It was TWO ordinary config bugs in our tree,
  and BOTH previously-recorded explanations were WRONG.** It was never a hardware
  quirk, never radio TX current / HFCLK contention, and never the libc. Proven by
  bisect against **stock Zephyr**, not by argument:

  | image | USB console |
  |---|---|
  | stock `hello_world` + `-S cdc-acm-console` | ✅ enumerates (`2fe3:0004`) |
  | `hello_world` + snippet + **our OpenThread block only** | ❌ dead |

  **Bug 1 — nothing ever started USB.** `CONFIG_USB_DEVICE_STACK_NEXT` is Zephyr's
  *new* USB stack: the device is NOT brought up automatically. You must either set
  **`CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y`** or call `usbd_enable()` yourself.
  Rung 7 set `USB_DEVICE_STACK_NEXT=y` + `USBD_CDC_ACM_CLASS=y` and did *neither*, so
  the console could never have enumerated — under radio or not, at any time.

  **~~Bug 2 — `CONFIG_OPENTHREAD_SYS_INIT=n`~~ RETRACTED (2026-07-11, same day):
  the line was a SILENT NO-OP.** The symbol is `depends on !NET_L2_OPENTHREAD`
  (`zephyr/modules/openthread/Kconfig`) and every build here uses the L2, so
  Kconfig **discarded the assignment** — it appears in no built `.config`
  (dongle or DK; only a warning buried in the west output). The "OT's SYS_INIT
  starts the radio before main()" story above was never actually tested, and it
  cannot be the "jast technique" either: **st-zephyr's working dongle prj.conf
  carries the exact same dead line.** Under `NET_L2_OPENTHREAD`, OT init is
  driven by the L2/net_if path regardless; `MANUAL_START` is the only deferral
  knob that applies. Meta-lesson: **a Kconfig line you add is not a fix until
  you see it in the built `.config`** — assignment to a dependency-unavailable
  symbol is silent.

  **Bug 3 — the partition manager placed `settings_storage` ON the Open
  Bootloader's flash pages.** With no static PM file, NCS lays the dongle out as
  if the app owns the full 1 MB and drops `settings_storage` "before: end" →
  **0xfe000–0x100000 = the bootloader's MBR-params (0xfe000) and settings
  (0xff000) pages**. OpenThread `select`s SETTINGS→NVS, so every OT-enabled
  image mounted NVS on the bootloader's own pages at boot — the failing bisect
  image included, and only OT-enabled images do it (matches the bisect
  perfectly). st-zephyr hit the same landmine (its `pm_static.yml` pins settings
  at 0x6c000 with an explicit `oob_gap` over the bootloader). Fix:
  **`pm_static_dongle.yml`** (settings at 0xd8000, 0xdc000+ reserved), passed
  via `-DPM_STATIC_YML_FILE=pm_static_dongle.yml` — dongle builds ONLY (the DK
  has no MBR/bootloader and keeps dynamic PM). Whether this is the USB killer is
  pending a flash; it is a real defect regardless. Remaining suspect #2 = float
  ABI (we're hard-float, the working image is soft-float ⇒ different prebuilt
  MPSL blob); `prj_dongle_softfloat.conf` is the ready-made experiment.

  **picolibc is EXONERATED** (disproven directly): the DK runs the full VM + node on
  **picolibc** and round-trips `BRIDGE OK` every 15 s. The libc was never involved;
  it was adopted in rung 7 as a guess against a misdiagnosed bug. Same-SoC boards
  should share a libc unless there is a concrete reason.

- **A console you cannot read must never be able to BLOCK the node.** With
  `UART_LINE_CTRL=y` the first CDC write blocks until a host asserts DTR. On an
  unattended board that blocks *forever* — and because OT runs in its own thread, the
  node still ATTACHES to the mesh while the VM is frozen on its first `println`,
  before it binds a socket. **That looks identical to a healthy node from the
  outside**, which is how a dead dongle passed for "working but headless" for two
  rungs. `platform_zephyr.c` now discards console writes when the board has no
  `CONFIG_STDOUT_CONSOLE`. A headless node's evidence belongs **on the wire**
  (the self-gating train — see `zephyr/README.md` "Rung 8"), not on a console.

---

## 2. OpenThread bring-up

- **`CONFIG_OPENTHREAD_MANUAL_START=y` is mandatory — and only HALF the recipe.**
  The NCS OT L2 runs `openthread_init()` unconditionally at boot (builds the
  instance, applies the Kconfig dataset). `MANUAL_START` gates the **enable**:
  `=n` → L2 calls `openthread_run()` on the **boot path** (starves USB on the
  dongle); `=y` → the app must enable Thread **after USB settles**. The config +
  the deferred app-side enable are a **pair** — one without the other does nothing
  (or double-enables).
- **App-side enable sequence (v3.2.4, in `thread_bringup()`):**
  1. (USB-CDC boards) `k_sleep(K_SECONDS(3))` first — settle, before touching OT.
  2. `openthread_init()` (idempotent no-op; L2 already made the instance).
  3. `openthread_run()` → does `otIp6SetEnabled(true)` + `otThreadSetEnabled(true)`
     internally (the modern equivalent of ns's manual toggle).
  4. **`net_if_up(openthread_get_default_context()->iface)`** ← the tuvm §0 fix.
  5. Poll `otThreadGetDeviceRole()` until `CHILD`(2)/`ROUTER`(3), ~30 s budget.
  - v3.2.4 API lives in `<openthread.h>` (module wrapper): `openthread_init/run/
    get_default_instance/mutex_lock/unlock`; `<zephyr/net/openthread.h>` for
    `openthread_get_default_context()` (`->iface`, `->instance`). Hold the OT mutex
    only around individual OT API calls, **never across the attach wait**.
  - ns's manual variant (older API): `otThreadSetEnabled(false)`→`otIp6SetEnabled(false)`
    →`otDatasetSetActiveTlvs`→`otIp6SetEnabled(true)`→`otThreadSetEnabled(true)`.
- **Kconfig dataset params may NOT be enough to attach — have the full TLV blob
  ready.** `CONFIG_OPENTHREAD_NETWORKKEY/PANID/CHANNEL/XPANID/NETWORK_NAME` set
  link params but are **not a commissioned dataset** (no Active Timestamp,
  Mesh-Local Prefix, PSKc, Security Policy). ns needed the full ~111-byte active
  TLV blob applied at runtime via `otDatasetSetActiveTlvs()`.
  **(tuvm's DK attached with Kconfig-only — but keep this as the escalation path.)**
  Get the blob: `ssh <gw> 'docker exec otbr ot-ctl dataset active -x'`.
- **OT thread stack — MPU-fault value.** NCS defaults `OPENTHREAD_THREAD_STACK_SIZE`
  to ~3168 (JOINER off); OT init runs PSA/AES-CCM link crypto on it and overflows →
  **silent boot**. Use **`CONFIG_OPENTHREAD_THREAD_STACK_SIZE=6240`**.
- **`CONFIG_NET_CONFIG_SETTINGS=n` — the 30 s boot deadlock.** `=y` pulls in
  net_config's `SYS_INIT(APPLICATION)` which **BLOCKS up to 30 s** waiting for an
  IPv6 address that only appears *after* the app enables Thread → deadlock, silent
  boot. `=n`; the node binds its own sockets. (Contrast: the loopback build *uses*
  NET_CONFIG because `::1` is available immediately.)
- **`CONFIG_OPENTHREAD_SYS_INIT` is a red herring** — it `depends on
  !NET_L2_OPENTHREAD`, already forced off when the L2 is present.

---

## 3. MTD vs FTD

- **MTD only for leaf nodes.** FTD **faulted at boot on the dongle** (heavier on
  MPSL/radio/RAM). `CONFIG_OPENTHREAD_MTD=y` / `CONFIG_OPENTHREAD_FTD=n`. An MTD
  leaf attaches as a child of the OTBR and sends through it; never routes.
- FTD is only for a **repeater** build (router-eligible, child/neighbor tables for
  topology observation) — disable Joiner/PING_SENDER to reclaim flash, and it must
  be pre-provisioned.
- **SED / rx-on-when-idle:** no sleepy-end-device tuning exists in either repo —
  nodes run rx-on MTD children. (Gap if a future board needs power savings.)

---

## 4. Flash / settings / NVS

- **Partial (time-sliced) erase is the USB-survival fix.** A blocking flash erase
  at boot (~100 ms, e.g. NVS mounting) **drops USB CDC IRQs** → CDC never
  enumerates → silent boot ("cost a whole bench session"). Fix:
  `CONFIG_SOC_FLASH_NRF_PARTIAL_ERASE=y` + `CONFIG_SOC_FLASH_NRF_PARTIAL_ERASE_MS=2`
  (chunks erases into ~2 ms slices so USB IRQs keep being serviced).
- **NVS: jast avoids it (raw flash), ns embraces it — both need partial-erase.**
  ns *initially* blamed NVS-at-boot for the silent boot, then a `.config` diff
  proved jast and ns are **byte-identical** on `NVS=y SETTINGS_NVS=y
  TRUSTED_STORAGE=y OPENTHREAD_CRYPTO_PSA=y` — jast keeps NVS on and still
  enumerates. `CONFIG_NVS=n` **fails at link** (trusted_storage needs NVS-backed
  PSA storage for OT's mandatory 802.15.4 link crypto). Takeaway: **keep NVS +
  Settings on** (needed for PSA), just (a) partial-erase and (b) never touch the
  boot path with OT/settings/flash until USB settles.
- **Settings partition placement (dongle): NEVER trust dynamic PM.** Measured
  2026-07-11: with no static file, NCS partition manager placed our
  `settings_storage` at **0xfe000–0x100000 — the Open Bootloader's MBR-params and
  bootloader-settings pages** (`build-dongle/partitions.yml` is the evidence; the
  board DTS's `storage_partition@dc000` is ignored by PM). Every OT-enabled image
  therefore mounted NVS on the bootloader's pages at boot (§1 Bug 3). Fix =
  **`pm_static_dongle.yml`**: settings at 0xd8000–0xdc000, `open_bootloader_gap`
  reserving 0xdc000–0x100000. (jast pins settings at `0x6c000`, which would cap
  the app at ~428 KB — our VM image is ~442 KB, hence the higher address.)
  **After any dongle build, verify `partitions.yml` before flashing.**

---

## 5. Heap / stack / RAM budget (nRF52840, 256 KB RAM)

| Kconfig | Value | Why |
|---|---|---|
| `HEAP_MEM_POOL_SIZE` | **32768** | OT+mbedTLS boot allocs; 16384 → boot NULL-alloc fault |
| `MAIN_STACK_SIZE` | **8192** | VM + script + OT init |
| `SYSTEM_WORKQUEUE_STACK_SIZE` | **2048** | networking + CDC-ACM workqueue |
| `OPENTHREAD_THREAD_STACK_SIZE` | **6240** | see §2 (MPU fault below ~3168) |
| `MBEDTLS_HEAP_SIZE` | 10240 (jast) | DTLS/crypto; ns relies on defaults |

- **GC C-stack root scan:** ns's `main.c`/`main_b.c` call `gc_set_stack_base(&local)`
  (conservative roots base). **tuvm's engine does NOT do a C-stack scan** (no
  `gc_set_stack_base`; roots come from `vm_stack`/`current_frame` + hooks) — so
  tuvm's `main.c` correctly omits it. Do not add it back.
- **OT build RAM is tight.** ns's `prj_thread` newlib build hit **93.9 % RAM**
  (246 KB/256 KB); OT stack alone ≈54 KB RAM / ≈188 KB flash. tuvm rung-7 DK build:
  **RAM ≈187 KB / 71 %, FLASH ≈437 KB / 42 %** at `OEVM_HEAP_SIZE=49152`. VM RAM
  knobs (env → `-D`, forwarded by `CMakeLists.txt`): `OEVM_HEAP_SIZE` (largest
  single knob), `METHOD_CACHE_SIZE`, `PRIM_CACHE_SIZE`, `VM_STACK_INIT_WORDS`,
  `OEVM_MARK_STACK_SIZE`. Shrink `OEVM_HEAP_SIZE` first if a board doesn't fit.

---

## 6. libc / FP

- **picolibc is fine, and saves ~12 KB flash** at ~0 RAM cost and zero functional cost
  (the VM has no newlib-isms: only `malloc/realloc/free`, `setjmp`, `snprintf`,
  `printf %g`, `mem*/str*`). **Verified on hardware 2026-07-11:** the DK runs the full
  VM + Thread node on picolibc and round-trips `BRIDGE OK` every 15 s.
- **⚠️ CORRECTION: picolibc has NOTHING to do with "Issue A".** This section previously
  called it "the prime suspect for the dongle Issue A delta". That was wrong, and it
  cost two rungs. The dongle console failure traces to config/layout bugs
  (`CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` + the settings-partition-on-bootloader
  placement — see §1 Bugs 1/3; the once-blamed `OPENTHREAD_SYS_INIT=n` is retracted
  there); the libc is
  not load-bearing either way. Choose a libc on its merits (flash size), and prefer the
  **same libc on same-SoC boards** so a working board stays a valid control for a
  broken one.
- **The swap:** newlib = `NEWLIB_LIBC=y` + `NEWLIB_LIBC_FLOAT_PRINTF=y` +
  `CBPRINTF_FP_SUPPORT=y`; picolibc = `PICOLIBC=y` + `PICOLIBC_IO_FLOAT=y` +
  `CBPRINTF_FP_SUPPORT=y` (drop the two newlib lines). **FP printf is required**
  (the VM prints floats via `%g`). `CONFIG_FPU=y` on both (Cortex-M4F).

---

## 7. Flashing / hardware bring-up

- **DK = J-Link** (SWD + RTT + independent uart0/VCOM). `west flash --runner jlink`.
  Capture VCOM: `stty -F /dev/ttyACMx 115200 raw -echo` first (else reads empty).
  Clean J-Link = `1366:1024`; a flaky/underpowered port flips PID (`0105↔0101`) with
  "Cannot connect / no SWD". **Preferred for all Thread/debug work.**
- **Dongle = DFU only** (no debugger). Enter DFU: **hold RESET while inserting USB**;
  LED fades red; `lsusb | grep 1915` → `1915:521f`. Flash: `nrfutil pkg generate
  --hw-version 52 --sd-req 0x00 --application zephyr.hex ...` then `nrfutil
  nrf5sdk-tools dfu usb-serial -pkg ... -p /dev/ttyACMx` (or `nrfutil device
  program --traits nordicDfu`, or `west flash -r nrfutil`). `nrfutil 8.1.1` is in
  the NCS toolchain.
- **POWER/SEATING is the #1 gremlin.** "Every USB gremlin traced to power/seating."
  Use a **self-powered USB hub** + firm edge-connector insertion; pressing RESET can
  unseat data pins. DFU can refuse to enumerate (zero radio) until firmly reseated.
- **RCP dongle firmware (`st-zephyr/firmware/usb840f-ot-rcp`):** turns an nRF52840
  dongle into an OpenThread **Radio Co-Processor** for the Docker OTBR gateway.
  **Do NOT build OT RCP on NCS v3.x** (USBD-next CDC-ACM IRQ UART path is broken for
  Spinel) — use the prebuilt ArthFink `ot-rcp-USB.hex` (old nRF5-SDK stack). Fanstel
  USB840F ships a Secure-DFU bootloader with a mismatched key → **flash via J-Link
  over SWD** (USB-powered; J-Link EDU doesn't supply target power). OTBR Spinel URL
  **must include `uart-flow-control`** (else RTS unasserted, dongle won't TX),
  baud 460800. Provisioned RCP/device enumerates `1915:cafe`.

---

## 8. Event loop / button (transferable architecture)

- **Event-loop-problem (st-zephyr spec):** a single-threaded `main()` that mixes
  blocking I/O (e.g. TFTP `recv()` with a 3 s timeout), net polling, and VM callback
  dispatch causes **unreliable event delivery + multi-second latency** (button
  release delayed 2–4 s, first-press-after-idle lost). **Fix = event-queue
  architecture:** ISRs/threads/workqueues `k_msgq_put()` typed events; `main()` is a
  pure consumer `k_msgq_get(K_FOREVER)` that never blocks on I/O; blocking I/O moves
  to its own thread; **the VM runs only on the main thread** (it is not thread-safe —
  you cannot call it from a timer ISR). *The single most transferable lesson for
  porting the VM to a new board.* (tuvm's pulse seam is a small instance of this:
  the GPIO ISR posts to a msgq the VM drains — see `pulse_zephyr.c` /
  [[pumpevents-blocks-forever]].)
- **Button debounce (st-zephyr, Golioth "Approach B"):** GPIO ISR on **both edges**
  (re)schedules `debounce_work` at `DEBOUNCE_MS=25`; on expiry read the *stable* pin;
  press schedules `long_press_work` at `LONG_PRESS_MS=500`; release cancels it. Pin =
  GPIO input + **pull-up** + edge IRQ, active-low. Dongle button = **P1.06** (distinct
  from the P0.06 green LED). The work-handler posts a typed event to the msgq → the
  loop dispatches the VM callback.

---

## 9. Other genuinely hard-won items

- **`CONFIG_POSIX_API` must stay OFF** — it exports a bare `send()` that **collides
  with the VM's `send()` dispatch** (`dispatch.c`). Use `zsock_*`-prefixed sockets
  (they work without POSIX_API). tuvm's `udp_zephyr.c` already uses `zsock_*`.
- **`CONFIG_RESET_ON_FATAL_ERROR=n`** (jast) — catch hard faults instead of silent
  reboot. **Set this early on any new-board bring-up.**
- **Unbuffered stdout is mandatory** for run-once programs: `setvbuf(stdout, NULL,
  _IONBF, 0)` — else a slow tail or fault hides all prior output. tuvm's `main.c`
  already does this.
- **native_sim caveat:** uses **NSOS host-offloaded sockets** — proves the socket
  *seam*, not the in-kernel net stack. On-silicon (dongle loopback / the DK Thread
  gate) is the only real net-stack proof.
- **Secrets discipline:** the real Thread dataset (network key, PANID, channel,
  XPANID, name; and the full TLV blob) lives ONLY in gitignored files
  (`prj_*_thread_local.conf` via `-DEXTRA_CONF_FILE`; `prov_dataset.h`). Committed
  files carry OpenThread-default placeholders (build but won't join). Never write the
  real key into any committed file, commit message, or log paste.
- **Attach-diagnostics overlay (`ns-zephyr/app/diag_ot_log.conf`):** `OPENTHREAD_DEBUG`
  + `LOG_MODE_DEFERRED` + `NET_LOG` shows the MLE handshake (Parent Request/Response,
  Child ID Request) — distinguishes **RF unreachability** (no Parent Response → node
  cycles channels) from **auth failure** (response but key reject). `OPENTHREAD_DEFAULT_TX_POWER=8`
  for marginal links.

---

### Quick config-delta table (jast ✅ proven-on-dongle vs ns port; tuvm follows the proven column)

| Symbol | jast | ns | tuvm DK (rung-7) |
|---|---|---|---|
| USBD_CDC_ACM_WORKQUEUE | y | y | n/a (uart0 console) |
| SOC_FLASH_NRF_PARTIAL_ERASE(_MS) | y / 2 | y / 2 | y / 2 |
| OPENTHREAD_MANUAL_START | y | y | **y** (+ `thread_bringup()`) |
| **OPENTHREAD_SYS_INIT** | ~~n~~ (dead line) | (default y) | *unavailable under `NET_L2_OPENTHREAD` — assignments silently discarded (§1 retraction, §2)* |
| **CDC_ACM_SERIAL_INITIALIZE_AT_BOOT** | (old stack) | — | **y on the dongle** — *else USB never starts (§1)* |
| OPENTHREAD_MTD / FTD | y / n | y / n | y / n |
| OPENTHREAD_THREAD_STACK_SIZE | 6240 | 6240 | 6240 |
| HEAP_MEM_POOL_SIZE | 32768 | 32768 | 32768 |
| NET_CONFIG_SETTINGS | (absent) | n | n |
| libc | picolibc | newlib | **either — NOT load-bearing** (DK proven on *both*) |
| dataset | full TLV blob | full TLV blob | Kconfig-only (attached) |
| net_if_up() | (L2 implicit) | (L2 implicit) | **explicit** (v3.2.4 needs it) |
| RESET_ON_FATAL_ERROR | n | (default) | **n** (a fault must leave evidence, not reboot) |

The two bolded OpenThread/USB rows are what the jast column was *actually* teaching us.
The `libc` row is the one we wrongly copied for two rungs (see §1, §6).

## 10. Node identity + fleet forensics (2026-07-12, the dongle endgame)

- **ext-MAC AND ML-EID are chip-stable on this platform.** Both survived reflashes and
  even the settings-partition move (0xfe000 → 0xd8000) on the dongle — observed, not
  theorized. So a source ML-EID in the gateway's `udp-echo6` log is a reliable
  per-board fingerprint; RLOC16s remain parent-assigned and unstable (rung-7 lesson).
- **The OTBR container log is the mesh flight recorder.** `docker logs --tail N otbr`
  (NOT `--since`, which matches nothing against this container's stamps) pairs every
  child's ML-EID with its ext-MAC-derived link-local in the MLE `Child ID Request` /
  `Child Update` lines — enough to attribute hours-old traffic to a physical board.
- **A silent boot OOM looks exactly like "attached but dead".** The VM's OOM message
  went to stderr, which is not wired to the Zephyr console; OpenThread (own thread)
  kept the node attached. Fixed twice over: config-D is baked into CMakeLists for all
  52840 builds, and `default_oom()` now prints via `platform_console_write` (see §5,
  BOARD_PORTS.md "Build", and review/ISSUE-dongle-usb-openthread.md for the full
  post-mortem).
- **Gate vs pinger loads:** `ThreadTimerNode` (:54360) halts forever on one lost
  round-trip — a GATE, loud by design, ends a soak in minutes at kitchen RF range.
  `SoakNode` (:54361) sends regardless and counts misses — the endurance load.
  Fleet status page: `http://gw85224-01:8085` (`soak-web` systemd unit on the gw,
  read-only tail of the udp-echo6 journal).
