# Design: R1 — `ns-posix` node skeleton (nodus-style keeper on the resident oevm VM)

Status: **approved design**, 2026-06-25. Rung **R1** of the Newspeak-node roadmap
(`ns-oevm/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md`).
Builds on R0 (the Model B `.oimg` loader, shipped). Deployment context:
`oevm/docs/deployment-model.md`. Porta protocol: `porta/docs/PROTOCOL.md`.

## Goal & scope

Stand up **`ns-posix`** — a new repo: a nodus-style **keeper** (host C) that
supervises the **resident oevm VM** and is joined to a real **porta** fleet
plane, developed desktop-first. The R1 node is **end-to-end**: it polls a real
`porta serve` on localhost, downloads an OTA'd `.oimg`, runs it on the resident
oevm VM, and reports observed state back to porta.

### In scope
- A new `ns-posix` repo that **owns the strictly-VM-neutral keeper core** (primary).
- Reuse of the proven `nodus-st` keeper core + posix seam impls; **one new file**
  — an oevm `struct vm` seam that runs `.oimg`.
- A **deploy** dev-tool: compile `.ns` → host-ABI `.oimg`, push to porta (HTTP API).
- An **integration gate** proving poll → install → run → report against real porta.

### Out of scope (deferred)
- **`nodus-st` → depends-on-`ns-posix` rewiring** (delete its `src/`, submodule
  `ns-posix/keeper/`). Tracked follow-up; not R1, so the working Berry node is
  untouched while ns-posix stands up.
- `run-loop` lifecycle, triggers (boot/interval), `reboot`-apply — parsed and
  stored by the reused keeper, but R1 runs **run-once** only.
- Surfacing VM run-failure into report `health` (telemetry) — **R2**.
- Telemetry stream (`data?id=`, JSONL), OTA-delta/partial-graph messages — **R2**.
- ESP32/Zephyr backend swap, the device (in-process/thread) vm-seam — **R6**.

## Key decisions (resolved during brainstorming)

| # | Decision | Rationale |
|---|----------|-----------|
| Reuse vs own | **Reuse the keeper core**; ns-posix owns it as a shared, **strictly VM-neutral** component (primary), `nodus-st` will depend on it | The Berry/ST coupling is confined to ONE ~60-line file (`berryhost_vm.c`); `keeper.c`/`cmd.c`/`tftp.c`/`report.c`/`crc32.c`/`seam.h` are already VM-neutral and porta-correct. Rewriting reinvents a battle-tested wire codec for no gain. ("Elegance/efficiency over minor reuse" → this *is* major, clean reuse.) |
| Ownership | **`ns-posix` is primary**; `nodus-st` depends on it (follow-up) | The VM-neutral keeper becomes a first-class shared asset; the seam discipline ("strictly VM-neutral is a win") keeps both nodes honest. |
| R1 porta scope | **Real porta in R1** (reuse `udp_transport.c` + `porta serve`) | The reused keeper core *is* the porta poll loop; a mock transport would be throwaway. R1 delivers a genuinely end-to-end node; R2 narrows to telemetry/OTA-delta. |
| VM/keeper seam | **Host = subprocess** (`popen("oevm_img <tmp.oimg>")`, run-once) | Fault isolation: a VM panic/abort cannot take down the keeper (matches the keeper's watchdog/restart/abandon + isolate-style model). Zero link coupling to oevm's build closure. Matches the `berryhost_vm.c` precedent. Device = in-process thread is an R6 backend concern the seam already hides. |

## Architecture: reuse map

The keeper depends only on the 3-interface seam (`seam.h`): `struct transport`
(datagram send/recv), `struct store` (kv + CRC-keyed image cache), `struct vm`
(`run(name,bytes,len,out)` / `stop(name)`), plus `keeper_config`
(`device_id`/`chip`/`loop_sleep_s`). R1 reuses everything except the vm-seam:

```
  ┌──────────────── ns-posix (NEW repo; owns the VM-neutral keeper core) ────────────────┐
  │  keeper/   keeper.c cmd.c tftp.c report.c crc32.c seam.h   ── reused, VM-NEUTRAL      │
  │  posix/    udp_transport.c  file_store.c                   ── reused verbatim         │
  │            oimg_vm.c   ◄── THE ONE NEW FILE (struct vm → popen oevm_img, run-once)    │
  │            keeperd.c                                       ── adapted harness/main    │
  │  tool/     deploy.{sh,py}  ── compile .ns→.oimg + push to porta (HTTP /api)           │
  │  tests/    unit suite (reused) + porta_integration.sh (new R1 gate)                   │
  └──────────────────────────────────────────────────────────────────────────────────────┘
        │ TFTP-over-UDP (commands / payload / report)        │ popen, run-once, capture stdout
        ▼                                                     ▼
   porta serve (localhost :6969)                       oevm_img <image.oimg>  (Model B resident VM)
```

`keeper/` is a bounded unit so `nodus-st` can later submodule `ns-posix` and
compile `ns-posix/keeper/*.c`. The core is **seeded from `nodus-st/src/`**; after
R1, `ns-posix` is canonical.

## Repo layout

```
ns-posix/
  keeper/      keeper.{c,h} cmd.{c,h} tftp.{c,h} report.{c,h} crc32.{c,h} seam.h
  posix/       udp_transport.c  file_store.c  oimg_vm.c  keeperd.c
  tool/        deploy.py            # ns-posix deploy app.ns --to <id>
  tests/       <reused unit tests>  porta_integration.sh
  Makefile     README.md
```

External deps (not vendored): the `oevm_img` binary (oevm Model B build) on
`$OEVM_IMG`/PATH; `ns_compile_oevm.py` + `dump_layout` (host-ABI `.oimg`); a
`porta` build for `porta serve`.

## Components

### `posix/oimg_vm.c` — the new vm-seam (the only genuinely new code)
A near-mechanical port of `nodus-st/host/berryhost_vm.c`:
- `oimg_run(ctx, name, bytes, len, out, out_size)`: write `bytes` to a temp
  `.oimg`, `popen("$OEVM_IMG <tmp>")`, stream stdout into `out` (NUL-terminated),
  `pclose`, `unlink`. Return 0 iff `WIFEXITED && WEXITSTATUS==0`, else <0.
- `oimg_stop`: no-op (run-once).
- `host_vm()`: resolves `oevm_img` from `$OEVM_IMG` else a default path; returns
  a `struct vm`.

### `tool/deploy.py` — the deploy path
`ns-posix deploy app.ns --to <id>`:
1. `dump_layout` → `layout.json` (host ABI); `ns_compile_oevm.py app.ns
   /tmp/app.oimg --format=oimg --layout=layout.json`.
2. `size = len(oimg)`; `crc = crc32_ieee(oimg)` (poly `0xEDB88320`, init/xor
   `0xFFFFFFFF`; test vector `"123456789" → 0xCBF43926`).
3. Push to porta — upload the `.oimg` payload and enqueue
   `{verb:"run", name, crc, size, lifecycle:"run-once"}` — via porta's HTTP `/api`
   directly (plain HTTP from Python) or by shelling out to the `porta` CLI,
   whichever the devsdk contract (`porta/docs/DEVSDK.md`) makes cleaner; the plan
   phase pins the exact endpoint/command.

### Reused
- **Verbatim:** `cmd.c`, `report.c`, `crc32.c`, `seam.h`, `udp_transport.c`,
  `file_store.c`.
- **Reused with one VM-neutral change:** `keeper.c` + `tftp.c` — raise the image
  buffer/transfer cap from the Berry-era 8 KB static (`static uint8_t img[8192]`)
  to real `.oimg` sizes (~34–47 KB for `01_method_call`; see Risks). This is a
  size constant, not a behavior change, and stays strictly VM-neutral.
- **Adapted harness:** `keeperd.c` wires `host_vm()` and a host `keeper_config`
  (`device_id` = fixed synthetic 16-hex EUI-64, `chip="host"`, small
  `loop_sleep_s`).

## Data flow (one cycle)

Deploy seeds porta; the keeper converges:

1. **deploy** (operator): compile `.ns`→`.oimg`, upload payload + enqueue `run` (HTTP).
2. `keeper_poll_once`: RRQ `commands?id=<id>` repeatedly → drain → `cmd_parse` → dispatch.
3. on `run`: `ensure_image` → `img_has(crc)?` no → RRQ
   `payload?id=<id>&name=&crc=` → **verify `size` + `crc32`** → `img_put`.
4. `vm->run` → `oimg_vm`: temp `.oimg` → `popen(oevm_img)` → capture stdout (run-once).
5. record app in observed `apps[]`.
6. `report_build` → WRQ `report?id=<id>` (apps + health; `node_config` on cold boot).
7. daemon sleeps `loop_sleep_s`; repeat.

Steps 2–3 and 6 are entirely reused keeper-core code; only step 4 is new.

## Error handling & fault isolation

- **Run-once** (R1): maps exactly to `oevm_img <image>` (runs `main:args:` to
  completion, exits); `stop` is a no-op.
- **Bad image** (size/CRC mismatch): `ensure_image` <0 → app skipped, never run
  (existing `keeper.c` behavior).
- **VM run failure** (nonzero `oevm_img` exit / panic): `popen` nonzero →
  `oimg_vm` <0; **the keeper survives** (the subprocess seam's isolation). R1
  keeps reused-`do_run` parity (records the app regardless of `vm->run` return);
  surfacing failures into report `health` is filed for **R2**.
- **Transport hiccups** (porta down/timeout): `keeper_poll_once` treats TFTP
  errors as non-fatal — drains, uploads a report, returns 0; the daemon loops on.

## Testing & verification

- **Unit:** reuse `nodus-st`'s VM-neutral suite (crc / tftp / cmd / report /
  keeper) verbatim — `make test`. Passing it is the signal the core stayed
  VM-neutral.
- **Integration (`tests/porta_integration.sh`) — the R1 acceptance gate:** build
  `oevm_img` (Model B) + `keeperd`; start `porta serve` on localhost; `deploy`
  `ns-tests/04_somns_basic/01_method_call.ns` (expected output `42`); run
  `keeperd` through a poll cycle; assert **both** (a) porta received a report
  listing the app installed with matching `crc`, and (b) the captured VM stdout
  `== "42"`. Mirrors `nodus-st/tests/porta_integration.sh`.
- **Oracle:** the program's known output (the same `42` Path A/B produce) — no
  new oracle.

## Risks & open questions

- **oevm-target decomposition:** `oimg_vm.c` + `keeperd.c` are host-specific
  integration; the keeper core is language- *and* VM-neutral. Confirm placement
  matches oevm#10 (R0/R4/R5/backend language-neutral) — the keeper core is a new
  neutral asset that a future `st-oevm` node could also reuse.
- **Identity on host:** a fixed synthetic EUI-64 is fine for a single dev node;
  multi-node desktop testing (R3+) needs distinct ids — out of R1 scope.
- **Image size cap:** the reused keeper buffers images in an 8 KB static
  (`keeper.c`); a `42`-printing `.oimg` is ~34–47 KB (R0 measurements). **R1 must
  raise this cap** (and the TFTP transfer size) to the real `.oimg` size — a
  concrete change to the reused core, kept VM-neutral.
- **Per-target `.oimg`:** deploy builds host-ABI; porta delivers per-target. R1 is
  host-only; the target-tagging story is R6.
```
