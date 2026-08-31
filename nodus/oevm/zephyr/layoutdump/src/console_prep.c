/* oevm/zephyr/layoutdump/src/console_prep.c — make the dump reach the console.
 *
 * tools/dump_layout.c owns main(), prints the layout JSON once with printf, and
 * returns. It is SHARED with the host gate (the nsl suite rebuilds it and
 * regenerates the host layout every run), so it must not grow a print loop, a
 * flush, or a board-shaped #ifdef. Anything a target needs has to be added
 * BESIDE it — hence a SYS_INIT here rather than an edit there.
 *
 * Two things, and the marker is deliberate:
 *
 *   setvbuf: src/main.c:1801 unbuffers stdout and stderr on every board and says
 *   why — "nothing unbuffers stderr, so without this a failing blob can die
 *   silently". main() returning without exit() never flushes picolibc's buffer,
 *   so a fully-buffered ~3 KB of JSON is emitted into memory and lost. The ESP32
 *   dumps predate this file and worked, so buffering is a HYPOTHESIS about the
 *   dongle, not a known cause.
 *
 *   printk: the discriminator for that hypothesis. printk bypasses stdio
 *   entirely and writes straight to the console driver, so the next capture
 *   separates three outcomes that all look identical as an empty file today —
 *   marker + JSON (stdio buffering was it), marker alone (the stdio path is
 *   broken and buffering was not the cause), neither (the CDC endpoint is
 *   dropping TX, e.g. DTR gating, and no stdio change can help).
 */
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <stdio.h>

static int layoutdump_console_prep(void)
{
    /* A BOUNDED wait for the host, and the bound is the point. On a USB-CDC
       console the driver can drop TX until the host opens the port and asserts
       DTR, which is a third way this capture can come back empty — and it is
       indistinguishable from the other two in the output file. Waiting reports
       what was actually observed instead: dtr=1 means the host was there and the
       gate is not the story; dtr=0 after the full wait names the gate directly.
       Bounded at 10 s (inside BOOT_DELAY's 20 s window) so a board with no
       reader still boots, prints into the void and stops, rather than hanging
       here forever looking like a dead image. */
    const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;
    int waited = 0;

    while (waited < 10000) {
        if (uart_line_ctrl_get(con, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr) {
            break;
        }
        k_sleep(K_MSEC(100));
        waited += 100;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printk("layoutdump: console up, stdout unbuffered, dtr=%u after %dms\n",
           (unsigned)dtr, waited);
    return 0;
}

/* APPLICATION level: after the console and the USB device stack are initialized
   (CDC_ACM_SERIAL_INITIALIZE_AT_BOOT), and before the main thread runs. */
SYS_INIT(layoutdump_console_prep, APPLICATION, 99);
