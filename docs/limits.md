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
| custos HTTP (port 6971) | any machine whose address is in the allowlist: by default the private ranges `10/8`, `172.16/12` and `192.168/16`, Tailscale's `100.64/10`, and loopback | read the fleet, run the debugger, profile, and **load a program onto a board on its cable** | `protocol/allowlist` `Default`; `--allow-cidr ""` serves any peer |
| porta's operator HTTP | the same allowlist (`--http-allow-cidr`) | read health and the manifest; it writes nothing | `porta/internal/httpsrv`. Writing to nodes (`porta put`, `porta blob put`) is for whoever has a shell on the gateway machine ([porta CLI.md](../porta/docs/CLI.md)) |
| The node ↔ porta wire (UDP, CBOR) | anyone who can send a UDP datagram to a node | write `goal/**` and `dbg/req`, the two paths a node accepts ([records](../protocol/PROTOCOL-NSL-RECORDS.md)) | [PROTOCOL-NSL.md](../protocol/PROTOCOL-NSL.md) defines no message authentication; a source-address check is the most a node could do, and UDP source addresses can be forged |
| The debugger, over the cable or `dbg/req` | whoever holds the cable, or can write `dbg/req` | stop the program, read its variables, and in a debug build replace a method | [tutorial 4](tutorials/04-debugger.md) |
| Program images pushed over the air | porta signs each image with its ed25519 key at push time; the host runner checks the signature before activating | — | [code-push design](design/2026-08-07-code-push-host-slice-design.md) D3. Over the air on a board is not built yet (rete#233) |

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
| Boards | The first hour is built and checked on the ESP32-C6 only. nsl runs on four processors (x86-64, RISC-V, Cortex-M4, Cortex-M33), but `load` needs a program region: the nRF52840 has none yet, and the RP2350 has no Zephyr flash driver, so on both a change is a reflash | [overview](overview.md), the processor table; `nodus/oevm/zephyr/docs/BOARD_PORTS.md` |
| Zephyr | The C6's Thread radio needs four driver fixes carried on a branch; whether a stock Zephyr is enough for the cable-only loop is not checked, and the documented board build is the Thread one | [overview](overview.md), "What to know about the C6" |
| Debugging on a board | Needs a separate debug image: the checks cost 10–16% per bytecode, so the standard image leaves them out | [tutorial 4](tutorials/04-debugger.md) §7 |
| Hot patching | Desktop only, debug builds only, and only a method whose new version uses what the running image already holds (no new strings, symbols or blocks) | [spike result](results/2026-10-06-313-patch-restart-spike.md) |
| Load | The ESP32-C6's program region only (512 KiB) | `custos/internal/load` |
| Memory | About 319 KB of heap on the C6 with the radio out of the build | [overview](overview.md) |
| The suite | The footprint ratchet is red: the code is larger than the banked size (rete#358) | the suite's aux line |
