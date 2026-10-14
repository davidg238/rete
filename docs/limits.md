# What 0.1 is not

rete 0.1 is a development tool for one developer, one board and a trusted network. This page lists what it does not
protect against, and what will change under you. Each line cites where it is decided or where the code shows it.

## Nothing is authenticated

To authenticate is to check who sent a request before acting on it. rete 0.1 does not do this anywhere except for
signed program images (last row). Its only protection is the network it runs on: the custos peer design (D3) names
Tailscale, a private network between your own machines, as the security perimeter, and leaves authentication for
later.

| Surface | Who can use it | What they can do | Source |
|---|---|---|---|
| custos HTTP (port 6971) | any machine whose address is in the allowlist: by default the private ranges `10/8`, `172.16/12` and `192.168/16`, Tailscale's `100.64/10`, and loopback | read the fleet, run the debugger, profile, **patch a running method**, **load a program onto a board on its cable**, and **push one over the air through porta** (`--porta-node`) | `protocol/allowlist` `Default`; `--allow-cidr ""` serves any peer |
| porta's operator HTTP | the same allowlist (`--http-allow-cidr`) | read health and the manifest, store and sign a blob (`POST /blob`), and **write any `goal/` cell of any node** (`PUT /node/{id}/{path...}`): a program push, a breakpoint set | `porta/internal/portacli/ops.go` (rete#398; CIDR-only admission is a known gap for 0.1). Whoever has a shell on the gateway machine can do the same with `porta put` and `porta blob put` ([porta CLI.md](../porta/docs/CLI.md)) |
| The node ↔ porta wire (UDP, CBOR) | anyone who can send a UDP datagram to a node | write `goal/**` and `dbg/req`, the two paths a node accepts ([records](../protocol/PROTOCOL-NSL-RECORDS.md)) | [PROTOCOL-NSL.md](../protocol/PROTOCOL-NSL.md) defines no message authentication; a source-address check is the most a node could do, and UDP source addresses can be forged |
| The debugger, over the cable or `dbg/req` | whoever holds the cable, or can write `dbg/req` | stop the program, read its variables, and in a debug build replace a method | [tutorial 4](tutorials/04-debugger.md) |
| Program images pushed over the air | porta signs each image with its ed25519 key at push time; the runner checks the signature before activating, on the host and on a board | — | [code-push design](design/2026-08-07-code-push-host-slice-design.md) D3 and §10; the board's check is `nodus/oevm/zephyr/src/runner.c` (rete#233). A firmware built without `OEVM_OTA_PUBKEY` cannot receive a program over the air (`nodus/oevm/zephyr/CMakeLists.txt`) |

Nothing is encrypted either: HTTP is plain HTTP, and a Thread mesh is only as private as its network key (the
committed configs carry the OpenThread default key; the real one is kept out of the repository).

## Everything will change

- **No compatibility is kept.** Both ends of the wire are reflashed together, so a shape change is an edit and a
  rebuild, never a versioned field or a deprecation period. The wire, the `.oimg` format, the image map, custos's
  HTTP answers and the nsl language can all change in any commit. That is a rule of this repository
  ([CLAUDE.md](../CLAUDE.md), design posture).
- **The `.oimg` must match the firmware.** The loader refuses an image built against another builtins registry
  (`oimg_load_blob: registry_version …`), so after pulling, rebuild both.
- **custos's HTTP is the contract** ([peer design](design/2026-10-04-custos-peer-design.md) D12), but it is 0.1's
  contract: its JSON shapes are not frozen. Freezing a 1.0 surface is rete#253.

## What works only part of the way

| Area | The limit | Record |
|---|---|---|
| Boards | The cable hour is built and checked on the ESP32-C6. Through porta, load and debug are proven on the C6 over Thread and on the MagTag (ESP32-S2) over WiFi (rete#233, rete#379, rete#380). nsl runs on four processors (x86-64, RISC-V, Cortex-M4, Cortex-M33), but `load` needs a program region: the nRF52840 has none yet, and the RP2350 has no Zephyr flash driver, so on both a change is a reflash; and master no longer links on the nRF52840 dongle (rete#427) | [overview](overview.md), the processor table; `nodus/oevm/zephyr/docs/BOARD_PORTS.md` |
| Zephyr | The C6's Thread radio needs four driver fixes carried on a branch; whether a stock Zephyr is enough for the cable-only loop is not checked, and the documented board build is the Thread one | [overview](overview.md), "What to know about the C6" |
| Debugging on a board | Needs a separate debug image: the checks cost 10–16% per bytecode, so the standard image leaves them out | [tutorial 4](tutorials/04-debugger.md) §7 |
| Hot patching | Debug builds only (desktop, and a board over its cable), and only a method whose new version uses what the running image already holds (no new strings, symbols or blocks) | [spike result](results/2026-10-06-313-patch-restart-spike.md); rete#313 |
| Load | Over the cable, the ESP32-C6's program region only (768 KiB, `program_a`). Through porta, any node whose firmware has program slots and the key; proven from custos on the C6 bell, and by `porta put` on the MagTag | `custos/internal/load/load.go`; rete#398, rete#233 |
| Memory | About 319 KB of heap on the C6 with the radio out of the build | [overview](overview.md) |
| Profiling | Over the cable only on master: the rows leave over the console. Through porta is rete#415, in flight | rete#415 |
| The Go gate | Red on master since rete#375: custos's `TestEveryLiveRecordHasReadingRules` fails, because `obs/debug`'s descriptor carries no validity rule (rete#430) | `cd custos && go test ./...` |
