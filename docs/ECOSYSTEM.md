# Ecosystem map — the language/VM/fleet constellation

> **Purpose.** A workspace-wide map of the related repos under `/home/david/workspace/`,
> what each represents, and how the story evolved to the current committed direction:
> **Newspeak on the oevm substrate, under a nodus-style keeper, over the porta fleet plane.**
> This is the single place to orient before touching any one repo. It lives in **`oevm/docs`**
> because oevm is the shared substrate every frontend targets (not owned by any one frontend).
> Maturity markers are point-in-time (2026-06-25) — trust each repo's own README/git log for
> live status. Paths are workspace-rooted (`<repo>/...`) so they read the same from any repo.

## The matrix — language frontend × VM substrate × deployment

```
                    ┌────────────────────────┬────────────────────────┐
                    │   SMALLTALK  frontend   │   NEWSPEAK  frontend    │
        ┌───────────┼────────────────────────┼────────────────────────┤
        │ compiler  │ st-berry               │ ns-berry    ✗ DEADEND   │
        │  → Berry  │  ST → Berry bytecode    │  NS → Berry bytecode    │
        │  bytecode │  working; extracted     │  Berry VM too limited   │
        │           │  from st-zephyr         │  → MOTIVATED  oevm      │
        ├───────────┼────────────────────────┼────────────────────────┤
        │ on BERRY  │ st-zephyr    ★ PRODUCT  │        (none —          │
        │ VM, on    │  = "st-zephyr-b"        │     never shipped)      │
        │ Zephyr    │  gpio/button/timer/     │                         │
        │ (dongle)  │  thread-mesh; → porta   │                         │
        ├───────────┼────────────────────────┼────────────────────────┤
        │ on OEVM   │ st-oevm                 │ ns-oevm     ★ CANONICAL │
        │ VM, host  │  = "st-zephyr-o"        │  compiler + dialect +   │
        │ (compiler │  DESIGN SPEC ONLY       │  capabilities + in-img  │
        │  /frontend)│  (awaits ns path)      │  debugger; suite green  │
        ├───────────┼────────────────────────┼────────────────────────┤
        │ on OEVM   │        (future)         │ ns-posix    ◐ NEXT      │
        │ VM, host  │                         │  desktop NS *node*:     │
        │ (NODE =   │                         │  nodus-style keeper +   │
        │  keeper)  │                         │  resident VM (fast loop)│
        ├───────────┼────────────────────────┼────────────────────────┤
        │ on OEVM   │        (future,         │ ns-zephyr   ◐ THEN      │
        │ VM, on    │     after ns path       │  HW node: backend swap  │
        │ Zephyr    │     proves out)         │  of the proven ns-posix │
        │ (dongle)  │                         │  stack; M1+M2 ✓         │
        └───────────┴────────────────────────┴────────────────────────┘

   SUBSTRATE   oevm   — committed VM (Piumarta OOEM, register-based C). Every "-o" targets it.
   LEGACY VM   berry  — external bytecode VM. Every "-b" targets it. Newspeak will NEVER run here.
   SHARED      ns-tests — black-box conformance suite (both NS image paths).
   FLEET PLANE porta  — canonical wire-protocol + gateway (TFTP/JSON/UDP; sleepy/polled/
                        reliable large-payload). jast was v1; porta + the nodus keeper are v2.
```

Legend: `★` mature/primary · `◐` in progress/next · `✗` abandoned · "spec only" = no code yet.
Note the bottom two Newspeak rows distinguish **frontend** (ns-oevm = the compiler, target-neutral)
from **node** (ns-posix / ns-zephyr = a deployed keeper+VM, a lang×target integration — see below).

## Where each kind of code lives — the 2 languages × 3 platforms decomposition (oevm#10)

The full target matrix is **2 languages** (Smalltalk `st-oevm`, Newspeak `ns-oevm`) **× 3 platforms**
(x86-linux, nRF52840-zephyr, **esp32-freertos**), all hosting on the one oevm VM. The ratified split
(Option A, oevm#10) records *where each kind of code lives so it isn't re-derived each time*:

| Code kind | Lives in | Example |
|---|---|---|
| portable · lang-neutral · target-neutral | **oevm core** (`oevm/src/`: message kernel, GC, dispatch) | `vm.c`, `dispatch.c`, `gc.c` |
| capability prims (platform tier = ambient; native tier = confined) — lang-neutral, dispatched through the kernel | **oevm**, capability-tiered | `platform.c` (print/new/hash); `serial.c` / `mirror.c` (confined) |
| per-target · target-specific · lang-neutral | **`oevm/src/backend/<target>/`** behind a link-time seam (exactly one impl links per build; `$OEVM_TARGET`/`$OEVM_BACKEND` selects) | `backend/posix/`; future `backend/zephyr/`, `backend/esp32/`; seams `platform_backend.h`, `serial_backend.h` |
| language-specific · target-neutral | **the frontend repo** | `ns-oevm` (compiler + dialect), `st-oevm` |
| language × target (the deployed **node**: keeper + resident VM) | **a thin integration repo** | `ns-posix` (NS × host), `ns-zephyr` (NS × zephyr), `nodus-st` (ST × zephyr) |

**Deployment posture = Model B, and `.oimg` loading is now the PRIMARY target** (decided 2026-06-25;
`oevm/docs/deployment-model.md`): a **resident** oevm VM supervised by a **keeper** (the `nodus` family
— portable C that polls porta, installs/runs/deep-sleeps/re-polls), loading OTA'd
**clustered-serialization `.oimg`** images delivered by porta. oevm replaces the Berry VM *under* the
keeper. **The Newspeak line adopts this nodus-style keeper interface** (jast — the v1 gateway+MCP debug
bridge — is retired). **AOT solidify-to-C (Model A) is demoted to an opt-in appliance/release build**
(max-speed single-purpose firmware); it is *not* the default. The `.oimg` loader/deserializer
(reasoned in `deployment-model.md`, not yet built) is therefore **foundational, early work**. ns-zephyr's
M1/M2 used Model A for bring-up; from here the path is Model B, prototyped first on **ns-posix** for
fast desktop iteration, then backend-swapped to the dongle.

## The repos

**Substrate & VMs**
- `oevm` — the committed VM. Register-based C, Ian Piumarta open object model, late-bound message
  kernel. Backends under `src/backend/` (`posix/` today; `zephyr/`, `esp32/` are phase work). Target
  for BOTH languages via the `-oevm` frontends. **Home of the cross-cutting docs** (this map,
  `deployment-model.md`, decomposition).
- `berry` — the Berry language/VM (external, C). Legacy backend for the `-berry` frontends.

**Smalltalk frontend line**
- `st-berry` — ST → Berry bytecode compiler, extracted from st-zephyr (2026-06-12). Working.
- `st-zephyr` — **the production node.** ST → Berry on Zephyr/nRF52840 dongle, OpenThread mesh.
  Exposes gpio / button / timer / thread-mesh via Berry-native C modules. Retargeting jast-gw → porta.
  The working reference & **behavioral oracle** for new ns node subsystems.
- `st-oevm` — ST on oevm ("st-zephyr-o"). **Design spec only, no code.** Inherits the shared oevm
  backend + capability prims once the ns path builds them; deferred until there is product pull.

**Newspeak frontend / node line**
- `ns-berry` — **the deadend.** NS → Berry bytecode. Berry's object model / reflection / debuggability
  couldn't carry full Newspeak — the dead branch that **motivated building oevm.** Not revisited.
- `ns-oevm` — **the canonical Newspeak frontend** (compiler: Ohm grammar → oevm bytecode; the dialect
  `ns-oevm/docs/NEWSPEAK_DIALECT.md`; the capability tiers; the in-image debugger). Suite green on both
  Path A (solidified C) and Path B (.oimg).
- `ns-posix` — **the desktop NS node (next).** A `nodus`-style keeper on the host supervising a
  resident oevm VM (posix backend), speaking porta and carrying remote debug — the fast-iteration
  vehicle that proves the keeper/porta/debug stack before hardware. *(repo to be created)*
- `ns-zephyr` — **the hardware NS node.** ns-oevm output on the dongle; M1 (arithmetic) + M2 (100k-alloc
  GC stress) pass. Becomes "the proven ns-posix stack with the backend swapped to `zephyr`."
- `ns-tests` — shared black-box conformance suite driving both NS paths.

**Fleet / supervisor plane**
- `porta` — canonical wire-protocol owner + Go gateway. TFTP/JSON over UDP, built for disconnected /
  sleepy / node-polls-server / reliable-large-payload fleets. `porta/docs/PROTOCOL.md` is the source of
  truth. Command/telemetry/OTA today; **debug is a planned protocol extension** (ns node phase).
- **keeper / supervisor layer** — the on-device porta client that polls, installs the OTA'd image, runs
  its lifecycle, deep-sleeps, re-polls (watchdog/restart/abandon), supervising a resident VM (Model B):
  `nodus` (Toit/ESP32), `nodus-st` (Smalltalk; portable C in `nodus-st/src/`). **The ns line takes a
  nodus-style keeper** — first as `ns-posix` (host), then `ns-zephyr` (device).
- `jast` / `jast-gw` — **v1** Go gateway + MCP debug bridge (the `mcp__jast__*` tools). **Retired** in
  favour of porta + the nodus keeper; still under `st-zephyr/tools/`.

**Reference / upstream** (not part of the build): `SOMns`, `newspeak`, `primordialsoup` (Newspeak
reference implementations; Soup is the Model-B reference), `toit-zephyr` (Berry-on-Zephyr provenance).

## The evolving story (how we got here)

1. **Smalltalk-on-Berry-on-Zephyr shipped first** (`st-zephyr`): a tiny Smalltalk → Berry bytecode on
   an nRF52840 dongle with a Thread mesh, driven by the **jast** gateway (v1). Proved the scriptable
   embedded node on real hardware.
2. **Newspeak was the language we wanted** (capabilities, modules, mirrors). Tried on the same Berry VM
   (`ns-berry`) — hit a wall: Berry couldn't carry full Newspeak semantics or a real debugger. **Deadend.**
3. **That deadend motivated `oevm`** — an open-object-model, stratifiable-reflection VM chosen as the
   substrate so confined capabilities are expressible *and* a real in-image debugger is possible.
4. **`ns-oevm` matured** on that substrate — compiler, documented dialect, three capability tiers, the
   in-image debugger (run-to-breakpoint ≈1.28× on the shipped build). The foundation-first bet (see
   `ns-oevm/docs/NEWSPEAK_DIALECT.md` §"Framing & background").
5. **`porta` was extracted** from st-zephyr as the standalone fleet plane (the hard sleepy/polled/
   reliable-UDP transport), and **`nodus`/`nodus-st`** established the keeper/supervisor model — porta +
   nodus are **v2**, replacing the **v1 jast** gateway+debug-bridge.
6. **Now: the Newspeak *node*** — adopt the nodus-style keeper for the ns line and prove the whole stack
   **on the desktop first** (`ns-posix`): keeper + resident VM + porta + remote-debug-over-porta, fast
   iteration. Then **`ns-zephyr`** is largely a backend swap to hardware. `st-oevm` inherits the shared
   oevm backend + capability prims afterward, if/when product pull justifies a second frontend.

## The committed direction (one line)

**oevm is the substrate; Newspeak (ns-oevm frontend → ns-posix node → ns-zephyr node) is the lead;
the node wears a nodus-style keeper; porta is the fleet plane.** Berry/`-berry` is legacy; `ns-berry`
is dead; jast is v1/retired; `st-oevm` waits behind the ns path. Through-line: *capabilities + fibers +
OTA/in-image debug on memory/CPU-constrained targets, developed desktop-first for speed.*

## Related docs
- `oevm/docs/deployment-model.md` — Model B (resident VM + OTA `.oimg` + keeper); image-format decisions.
- `ns-oevm/docs/NEWSPEAK_DIALECT.md` — the dialect + the foundation-first framing.
- `ns-oevm/docs/superpowers/plans/2026-06-25-ns-zephyr-capability-node-roadmap.md` — the current phase ladder.
- `porta/docs/PROTOCOL.md` — the fleet wire protocol (the debug extension lands here).
- oevm#10 — the 2-languages × 3-platforms decomposition issue.
