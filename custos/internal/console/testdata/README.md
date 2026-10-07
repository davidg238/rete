# Golden fixtures

Real captures from the bench C6 on 2026-08-03, in the collector's own on-disk
format — stamps and `@@rete-console@@` markers included. Not hand-written, not
trimmed beyond run boundaries.

- `c6-boot.log` — a clean boot of a default build.
  Key `porta-v1-final-779-gc76a6d5-dirty-20260803T201928`, 37 lines, no `#CD:`.
  It carries `E: Couldn't set control register.` / `E: Failed to init settings.`
  because it was captured on a C6 with no DS3231 on the bus. Those two lines are
  the RTC driver correctly reporting an absent part, not a fault in the capture
  (#146, and the reasoning is at `prj_esp32c6.conf`'s RTC block). **Do not
  regenerate the fixture to make them go away** — a fixture edited to look
  cleaner is measuring the edit.
- `c6-fault.log` — a boot of a `prj_esp32c6_faultgate.conf` build, faulting on
  purpose at `src/main.c:1688`, with the 270 `#CD:` coredump lines that follow.
  Key `porta-v1-final-779-gc76a6d5-dirty-20260803T202011`, 329 lines.

Each begins at the C6's ROM banner (`ESP-ROM:esp32c6-...`), the start of one
physical boot. The reader (`read.go`, `Runs`) starts a run at the kernel banner
(`*** Booting Zephyr OS`), which every board prints, so the ROM lines before it
are no run's here; in a live log they end the run before.

Note there are two `ESP-ROM` lines per flash on this board: esptool's reset into
download mode, then the real boot. Each fixture contains exactly one, the second.

They exist because the reader's parsers rot when upstream Zephyr changes a
banner or a fault format, and a fixture is the only thing that notices
(design §8). Regenerate them from hardware — never edit them to make a test
pass. A test that only passes against an edited fixture is measuring the edit.

The matching ELF for `c6-fault.log` is in the archive under the build id on its
`tuvm: build-id` line. If it has been pruned, the fault fixture can still be
parsed; only the backtrace (`/panics/<n>/bt`) needs the ELF.

## Regenerating

    curl -s http://localhost:6971/board/c6/health
    ./run.sh --board esp32c6 --pristine --flash
    ./run.sh --board esp32c6 --conf-extra prj_esp32c6_faultgate.conf --pristine --flash
    grep -n 'ESP-ROM:esp32c6' ~/.local/state/rete/console/console-c6-$(date +%F).log

then slice each run from its second `ESP-ROM` line to just before the next one.
