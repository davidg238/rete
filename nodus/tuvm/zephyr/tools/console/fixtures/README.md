# Golden fixtures

Real captures from the bench C6 on 2026-08-03, in the collector's own on-disk
format — stamps and `@@rete-console@@` markers included. Not hand-written, not
trimmed beyond run boundaries.

- `c6-boot.log` — a clean boot of a default build.
  Key `porta-v1-final-779-gc76a6d5-dirty-20260803T201928`, 37 lines, no `#CD:`.
- `c6-fault.log` — a boot of a `prj_esp32c6_faultgate.conf` build, faulting on
  purpose at `src/main.c:1688`, with the 270 `#CD:` coredump lines that follow.
  Key `porta-v1-final-779-gc76a6d5-dirty-20260803T202011`, 329 lines.

Each begins at its **boot banner** (`ESP-ROM:esp32c6-...`), not at the
`tuvm: build-id` line, because that banner is where `logread.split_runs()`
starts a run — a fixture beginning at the build-id line parses as zero runs.

Note there are two `ESP-ROM` lines per flash on this board: esptool's reset into
download mode, then the real boot. Each fixture contains exactly one, the second.

They exist because the reader's parsers rot when upstream Zephyr changes a
banner or a fault format, and a fixture is the only thing that notices
(design §8). Regenerate them from hardware — never edit them to make a test
pass. A test that only passes against an edited fixture is measuring the edit.

The matching ELF for `c6-fault.log` is in the archive under the build id on its
`tuvm: build-id` line. If it has been pruned, the fault fixture can still be
parsed; only `symbolize` needs the ELF.

## Regenerating

    systemctl --user is-active rete-console@c6
    ./run.sh --board esp32c6 --pristine --flash
    ./run.sh --board esp32c6 --conf-extra prj_esp32c6_faultgate.conf --pristine --flash
    grep -n 'ESP-ROM:esp32c6' ~/.local/state/rete/console/console-c6-$(date +%F).log

then slice each run from its second `ESP-ROM` line to just before the next one.
