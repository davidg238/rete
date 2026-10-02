# Node id board proof — #118 / #242, plan Task 10

Result doc of record for the board proof in
[`2026-10-02-node-id-factory-address.md`](../superpowers/plans/2026-10-02-node-id-factory-address.md) Task 10.
Probe: `nodus/oevm/zephyr/NodeIdProbe.nsl`, Model B, branch `118-node-id-factory-address` at `07bbdbd`.

## Table

| family | board | printed | independent reading | sticker | state |
|---|---|---|---|---|---|
| ESP32-C6 | `c6spare` | `board c6spare` / `node 404cca446e60` | esptool `BASE MAC: 40:4c:ca:44:6e:60`; USB-JTAG by-id `40:4C:CA:44:6E:60` | none on the board (David, 2026-10-02) | proven: esptool's own efuse read is the golden |
| RP2350 | RP2350-Zero | `board rp2350zero` / `node 4530454d38330003` | USB serial string `4530454D38330003` (same `hwinfo` source) | none on the board | proven 2026-10-02 ~10:15, [`console log`](./2026-10-02-118-node-id-rp2350-console.log) |
| nRF52840 | `nrf4ec0` (PCA10059 dongle) | `board nrf4ec0` / `node d2666d1af919` | Open DFU Bootloader USB serial `D2666D1AF919` (Nordic's own code) | `D2:66:6D:1A:F9:19` (photo, David) | proven on this board; **formula open for other boards**, see below |
| ESP32 (WROOM), S2 (MagTag) | — | — | — | — | **unproven: not attached** |

The plan's RP2350 row ("the primitive FAILING is the proof") is superseded by `d53dd2e`: an RP board answers its
flash serial, 16 hex.

## C6 — raw console (collector log `console-c6spare-2026-10-02.log`, MST)

First flash, 13:15 (`run.sh --board esp32c6 --node NodeIdProbe.nsl --build-dir build-nodeid-esp32c6 --flash`;
board name resolved by `run.sh` from the collector env, not passed):

```
2026-10-02T13:15:28.475-07:00 E: Failed to init settings.
2026-10-02T13:15:28.475-07:00 *** Booting Zephyr OS build v4.2.0-12-gbc47c6fab9cc ***
2026-10-02T13:15:28.475-07:00 oevm: build-id porta-v1-final-1287-g07bbdbd-20261002T131520
2026-10-02T13:15:28.475-07:00 logudp: not built in (no OEVM_LOG_UDP6) — console is UART only
2026-10-02T13:15:28.476-07:00 oevm: observer armed (slack=8 tick=1000ms backstop=30000ms hw=yes)
2026-10-02T13:15:28.476-07:00 board c6spare
2026-10-02T13:15:28.476-07:00 node 404cca446e60
```

esptool, same flash (its own efuse read, not through `hwinfo`):

```
MAC: 40:4c:ca:ff:fe:44:6e:60
BASE MAC: 40:4c:ca:44:6e:60
MAC_EXT: ff:fe
```

`E: Failed to init settings.` is not from this change — it is rete#263, and the same line is in the 13:03 boot of the
2026-09-17 image this flash replaced.

## Falsifiers

| | check | result |
|---|---|---|
| (a) | compare against a different board's address: `c6bench` JTAG `40:4C:CA:45:47:A4` (`tools/bench-identity.sh`) | `404cca4547a4` ≠ `404cca446e60` — the comparison can fail |
| (b) | full erase, reflash, same id | `esptool erase_flash`: `Chip erase completed successfully in 4.2 seconds.`; console then `invalid header: 0xffffffff` (ROM, no image); reflash at 13:16 prints the same id, below |
| (c) | RP2350 alive under a node that does not read `nodeId` | moot since `d53dd2e` (the primitive no longer fails on RP); the board did boot the #133 gate image, see the RP2350 console log header |

After the erase (new build id, same node id):

```
2026-10-02T13:16:08.462-07:00 oevm: build-id porta-v1-final-1287-g07bbdbd-20261002T131604
2026-10-02T13:16:08.462-07:00 board c6spare
2026-10-02T13:16:08.462-07:00 node 404cca446e60
```

## nRF — raw console (collector log `console-nrf4ec0-2026-10-02.log`, MST)

Flashed by DFU at 13:43 (`CONSOLE_PORT=<the dongle's by-id> run.sh --board nrf52840dongle --node NodeIdProbe.nsl
--build-dir build-nodeid-nrf52840dongle --flash`; RESET held by David). `-dirty` is the uncommitted `docs/overview.md`.

```
2026-10-02T13:43:43.056-07:00 *** Booting nRF Connect SDK v3.2.4-4c3fc0d44534 ***
2026-10-02T13:43:43.057-07:00 oevm: build-id pre-rete-1355-g07bbdbd-dirty-20261002T134300
2026-10-02T13:43:43.858-07:00 thread: attached role=2
2026-10-02T13:43:43.858-07:00 board nrf4ec0
2026-10-02T13:43:43.858-07:00 node d2666d1af919
```

**The plan's STOP check: decided, David 2026-10-02 — force the top two bits.** The probe above ran raw
`DEVICEADDR` with no `|= 0xC0` and equalled the sticker, so on `nrf4ec0` the raw top two bits are already set (`D2`).
The sticker and the bootloader both show the address with those bits forced; a board whose raw bits are clear would
have printed an id differing from its sticker in the first hex digit. `link_addr_read` now ORs `0xC0` into the first
byte. Compiled for the dongle; **not re-run on a board**, and `nrf4ec0` cannot show the difference (`D2 | C0 = D2`).

Falsifier (b), id across a full erase, was not run on the nRF: the dongle has no debugger, so no `recover`. The value is
read from FICR, which a flash does not write.

## Deploy — plan Task 11, 2026-10-02 14:34–15:20 MST

**Gateway.** `gw85224-01`, manifest-only restart (no porta code changed since the running binary). Killed by pid
14:34:23, serving again 14:34:47: `porta: manifest gate ON (MANIFEST.cddl, sha256 d7eefc87…) — persisted to the store`,
and `GET /manifest` hashes to the same value. Rollback beside it: `~/porta-v2/MANIFEST.cddl.pre118`. Nothing on the
gateway keyed a porta space by bench name (soak dashboard: mesh address; collector: console logs only).

**Nodes.** Read from the store after the last flash (`porta get <id> obs/build`, `gw/liveness`); every one pins
`man 0xd7eefc871e538b77` and has zero `gate` rows in its log.

| board | space (id) | image | `obs/build.board` | how flashed | verdict |
|---|---|---|---|---|---|
| `c6spare` | `404cca446e60` | `PortaBridgeNodeC6` | `c6spare` | `tests/target/run.sh bridge-health` | PASS 3/3, 180 s window clean |
| `c6bench` | `404cca4547a4` | `PortaBridgeNodeC6` | `c6bench` | built here, esptool on the gateway, collector stopped | `#live`, name converged |
| `nrf4ec0` | `d2666d1af919` | `PortaBridgeNodeNrf` | `nrf4ec0` | DFU | `#live`, name converged |
| `nrf92c2` | `ec34e57ca8e1` | `PortaBridgeNodeNrf` | `nrf92c2` | DFU | `#live`, name converged |
| the bell | `206ef1195438` | `ShipsBellAlarmNodeC6` | `c6` | `zephyr/flash-bell.sh` | `#live`, clock set, alarm armed |

`c6bench`'s id equals the MAC recorded for its devkit in its collector env (`40:4C:CA:45:47:A4`); both dongles' ids
equal the serial their DFU bootloader enumerates with. `7adf` was not reflashed: it runs the July echo soak, has never
had a porta space, and #118 changes nothing on it.

**The bell did not come up after its flash, and the image was not the cause.** Flash verified at 14:59; then every boot
with the DS3231 powered stopped after `IEEE 802154 radio initialized`, before the Zephyr banner, with the init thread
blocked and no watchdog (the observer is not armed that early). The one boot that got through, during a power glitch,
logged `Couldn't set control register` and `rtc nil` and ran the VM normally. After David intervened at the bench
(15:19; what he changed is not recorded here) the same image, unflashed, booted clean: `rtc 15:19:44`,
`rtcalarm: armed 15:29:55`. So the fault was on the hardware side, connection or supply, and which is not established.
The DS3231 status read `80`, the oscillator-stop flag, so the part had lost power at some point; the ROM strap word
also differed between the hung boots (`boot:0xc`) and the good one (`boot:0x4e`). What blocks forever in init is not
traced (#317).

## Still owed

- An nRF board whose raw top two bits are clear would be the proof that the forced bits match its sticker. `nrf92c2`
  was read and is not that board either (`EC` already has them set); none is identified on this bench.
