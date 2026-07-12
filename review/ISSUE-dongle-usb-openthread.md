# DRAFT ISSUE (post to davidg238/tuvm — I lacked permission to create it)

Post with:

    gh issue create --repo davidg238/tuvm \
      --title "nRF52840 dongle: attaches to the Thread mesh but NEVER transmits; USB-CDC console dead whenever OpenThread is present" \
      --body-file review/ISSUE-dongle-usb-openthread.md

---

## Status: OPEN, partially diagnosed. Two bugs fixed, root cause NOT yet found.

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

### Two bugs found and fixed (necessary, NOT sufficient)

1. **Nothing ever started USB.** `CONFIG_USB_DEVICE_STACK_NEXT` (the new stack) does not
   bring the device up automatically — you must set `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=y`
   or call `usbd_enable()`. Rung 7 did neither, so the console could never have
   enumerated, radio or not.
2. **`CONFIG_OPENTHREAD_SYS_INIT=n` was missing.** `OPENTHREAD_MANUAL_START=y` only defers
   `openthread_run()`; OT's `SYS_INIT` still brings up the radio *before* `main()`.
   (Corollary: the 3 s USB settle in `src/main.c` has been **dead code since it was
   written**.)

**Both applied — and the dongle still does not enumerate.** So `SYS_INIT=n` alone is not
the fix; something else in OT's boot path (L2/net_if init? NVS/settings flash? radio
clock claim?) is also involved.

### Explanations that are DISPROVEN (do not re-litigate)

- ❌ **"USB-CDC dies under radio current / HFCLK contention"** (a hardware quirk). No:
  stock Zephyr consoles fine on this dongle.
- ❌ **picolibc vs newlib.** Disproven directly: **the DK runs the full VM + Thread node
  on picolibc** and round-trips `BRIDGE OK` every 15 s. picolibc was adopted in rung 7 as
  a guess against a misdiagnosed bug.

### Why a dead board looked healthy for two rungs

OpenThread runs in its own thread and attaches **independently of the VM**. So a VM frozen
on its first `println` (with `UART_LINE_CTRL=y` the first CDC write blocks forever waiting
for a DTR that never comes) still yields a node that appears in `ot-ctl child table` with a
healthy, climbing age. **Mesh attach proves nothing about VM liveness.** The rung-8
self-gating train is what finally exposed it (`zephyr/README.md` "Rung 8").

Mitigated in `21d4059`: `platform_console_write()` now discards when the board has no
`CONFIG_STDOUT_CONSOLE`, so a console with no reader can never wedge the node.

### Next steps

1. Flash **bisect 2** (`hello_world` + OT block + `SYS_INIT=n`) — already built. Does
   `SYS_INIT=n` fix the *isolated* case? If yes, something else in the full app also kills
   USB; if no, OT's L2/net_if init starts the radio regardless and `SYS_INIT=n` is a red
   herring.
2. If still dead: **diff our dongle build directly against st-zephyr's working
   `tests/dongle/test-dongle-06-jast-ftd`** (same board, same stack, one works) — the delta
   is finite and enumerable.
3. Separately: even once the console works, **why the node never transmits** is still open
   (bind failure vs wedge). The console should answer it in one boot.

Requires physical DFU (hold RESET while inserting) for each flash.

Refs: `zephyr/prj_dongle_thread.conf`, `zephyr/docs/ZEPHYR_LESSONS.md` §1/§6,
`src/backend/zephyr/platform_zephyr.c`, `src/main.c` `thread_bringup()`.
