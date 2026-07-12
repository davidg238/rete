# DRAFT ISSUE (post to davidg238/tuvm — created from a session without gh permission)

Post with:

    gh issue create --repo davidg238/tuvm \
      --title "nRF52840 dongle: attaches to the Thread mesh but NEVER transmits; USB-CDC console dead whenever OpenThread is present" \
      --body-file review/ISSUE-dongle-usb-openthread.md

---

## Status: OPEN, root cause candidate identified — fix built, awaiting a flash.

Discovered while landing rung 8 (`21d4059`, `dc05f49`). The dongle has **never actually
worked** — rung 7 recorded it as "attached, functional, but headless", which was wrong:
it attaches and is **completely silent on the wire**.

### What is proven (by bisect against STOCK Zephyr, not by argument)

| image | USB console |
|---|---|
| stock `hello_world` + `-S cdc-acm-console` | ✅ enumerates (`2fe3:0004`) |
| `hello_world` + snippet + **our OpenThread block only** | ❌ dead |

No VM, no node, no libc change in the failing image. **OpenThread's presence causally
kills USB-CDC enumeration.**

### Findings, in the order they fell

1. **Nothing ever started USB.** `CONFIG_USB_DEVICE_STACK_NEXT` (the new stack) does not
   bring the device up automatically — you must set `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y`
   or call `usbd_enable()`. Rung 7 did neither, so the console could never have
   enumerated, radio or not. **Fixed, necessary, not sufficient.**
2. ~~**`CONFIG_OPENTHREAD_SYS_INIT=n` was missing.**~~ **RETRACTED — the line was a
   silent no-op.** The symbol is `depends on !NET_L2_OPENTHREAD`
   (`zephyr/modules/openthread/Kconfig`) and every image here uses the L2 integration,
   so Kconfig discarded the assignment: it appears in **no built `.config`** (checked:
   dongle, DK). It was never tested, and it cannot explain the working reference image
   either — **st-zephyr's working dongle config carries the exact same dead line.**
   Corollary: "bisect 2" (`hello_world` + OT + `SYS_INIT=n`) is meaningless as designed
   and was **not** flashed.
3. **The settings partition was placed ON TOP of the Open Bootloader.** Without a static
   partition-manager file, NCS PM lays out the dongle as if the app owns the full 1 MB
   and drops `settings_storage` "before: end" → **0xfe000–0x100000** (verified in
   `build-dongle/partitions.yml`). On the dongle, 0xe0000+ is the Nordic Open
   Bootloader; **0xfe000 is its MBR-params page and 0xff000 its bootloader-settings
   page**. OpenThread `select`s `SETTINGS`→NVS, so *every OT-enabled image* — including
   the bisect's `hello_world`+OT — **mounts NVS on the bootloader's own pages at boot**,
   before `main()`. A fresh mount finds non-NVS data there and initializes/erases.
   This is (a) unique to our build — the working image pins settings at 0x6c000 via
   `pm_static.yml`, with an explicit `oob_gap` over the bootloader region, and its
   comments record the same class of failure ("NVS erases on mount, killing USB");
   (b) exactly co-extensive with the bisect result (only OT-enabled images touch it);
   (c) a landmine regardless of whether it is the USB killer.
   **Fixed: `zephyr/pm_static_dongle.yml`** (settings at 0xd8000–0xdc000, everything
   from 0xdc000 up reserved), pass `-DPM_STATIC_YML_FILE=pm_static_dongle.yml`.

### The full remaining delta vs the KNOWN-WORKING image is 49 config lines

Diffed our dongle `.config` against st-zephyr `tests/dongle/test-dongle-06-jast-ftd`
(same board, same NCS install, same devicetree — byte-identical `chosen` console/USB
nodes). After discarding dataset/role/RAM-tuning noise, the only structural suspects:

- **FPU/float ABI: ours is hard-float (`FPU=y`, `FPU_SHARING=y`, `FP_HARDABI=y`,
  `MPSL_LIB_FLOAT_ABI_DIR="hard-float"`); the working image is soft-float, no FPU.**
  A different prebuilt MPSL (radio arbiter) binary. Suspect #2 if the partition fix
  isn't it.
- `OPENTHREAD_THREAD_STACK_SIZE` 6240 vs 3168, `NET_PKT_RX/TX_COUNT` 8 vs 10/16,
  settings partition size — RAM tuning, implausible as USB killers.

### Explanations that are DISPROVEN (do not re-litigate)

- ❌ **"USB-CDC dies under radio current / HFCLK contention"** (a hardware quirk). No:
  stock Zephyr consoles fine on this dongle.
- ❌ **picolibc vs newlib.** Disproven directly: **the DK runs the full VM + Thread node
  on picolibc** and round-trips `BRIDGE OK` every 15 s. picolibc was adopted in rung 7 as
  a guess against a misdiagnosed bug.
- ❌ **`OPENTHREAD_SYS_INIT=n` as a fix** — see finding 2: silently discarded by Kconfig
  under `NET_L2_OPENTHREAD`, present-and-dead in the working config too.

### Why a dead board looked healthy for two rungs

OpenThread runs in its own thread and attaches **independently of the VM**. So a VM frozen
on its first `println` (with `UART_LINE_CTRL=y` the first CDC write blocks forever waiting
for a DTR that never comes) still yields a node that appears in `ot-ctl child table` with a
healthy, climbing age. **Mesh attach proves nothing about VM liveness.** The rung-8
self-gating train is what finally exposed it (`zephyr/README.md` "Rung 8").

Mitigated in `21d4059`: `platform_console_write()` now discards when the board has no
`CONFIG_STDOUT_CONSOLE`, so a console with no reader can never wedge the node.

### Next steps (one flash each, in this order)

1. **Flash the rebuilt full image** (`ThreadTimerNode` + `pm_static_dongle.yml`; DFU zip
   pre-generated). Watch for enumeration (`2fe3:0004`), then the self-gating train on the
   gateway (`udp-echo6` lines every 15 s — N lines = N−1 proven round-trips, no console
   needed).
2. If USB is still dead: rebuild with **soft-float** (`CONFIG_FPU=n`, drop `FP_HARDABI`,
   `MPSL_LIB_FLOAT_ABI_DIR="soft-float"`) to match the working image's MPSL variant, and
   flash that.
3. If still dead after both: the residual delta is ~10 benign-looking lines — flash the
   st-zephyr image as a sanity control, then walk our config toward it line by line.

Requires physical DFU (hold RESET while inserting) for each flash.

Refs: `zephyr/pm_static_dongle.yml`, `zephyr/prj_dongle_thread.conf`,
`zephyr/docs/ZEPHYR_LESSONS.md` §1/§6, `src/backend/zephyr/platform_zephyr.c`,
`src/main.c` `thread_bringup()`,
`st-zephyr/tests/dongle/test-dongle-06-jast-ftd/{prj.conf,pm_static.yml}`.
