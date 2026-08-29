# Newspeak node — phase roadmap (desktop-first: ns-posix → ns-zephyr)

> **Status:** roadmap (decided 2026-06-25, reshaped same evening by four steers — see below). Each rung
> is its own sub-project (spec → plan → implement). This captures the *decomposition and ordering*.
> Landscape/context: `oevm/docs/archive/ECOSYSTEM.md`; deployment: `oevm/docs/deployment-model.md`.

## Decision

**Build the Newspeak node forward on oevm, in modest steps, to prove the capability node end-to-end —
developed desktop-first (`ns-posix`) then ported to hardware (`ns-zephyr`).** Chosen over
"re-implement st-zephyr on oevm (st-zephyr-o) first": the ns/oevm stack is years ahead (`ns-oevm`
mature; `ns-zephyr` boots M1+M2) while `st-oevm` is spec-only; native-subsystem-as-confined-capability
is the thesis; the in-image debugger already exists; `ns-berry` is dead. Keep **st-zephyr-b as the
behavioral oracle** for each new subsystem.

## Four steers (2026-06-25 evening) that shaped this

1. **Landscape docs root in `oevm/docs`** (oevm is the shared substrate) — `ECOSYSTEM.md` moved there.
2. **The ns node wears a nodus-style keeper interface** — jast was v1 (gateway + MCP debug bridge),
   retired; porta + the `nodus` keeper are v2. *(Resolves the earlier open "keeper relationship" question.)*
3. **Develop desktop-first** — an `ns-posix` nodus port is the fast-iteration vehicle; hardware follows
   as a backend swap.
4. **`.oimg` loading (Model B) is the PRIMARY target, not AOT (Model A).** The resident-VM-loads-`.oimg`
   path is the default; solidify-to-C AOT is demoted to an opt-in appliance/release build. → the `.oimg`
   loader/deserializer is **foundational, early work** (it was reasoned in `deployment-model.md`, not built).

## Goal & north star

A **modern embedded scripting *node***: object-**capability** security + **fibers** + **OTA/in-image
debug**, on a **resident** oevm VM under a nodus-style keeper, joined to the **porta** fleet plane —
developed desktop-first for speed.

## The porta-carries-debug call (and its happy convergence)

Remote debug rides **porta** (which already solves disconnected / sleepy / node-polls-server /
reliable-large-payload-over-UDP), so debug is **async / store-and-forward / poll-paced**, not a live
socket. This *fits* the shipped debugger: C-assisted breakpoints let the node run autonomously to the
breakpoint (~1.28× on the shipped build, [[debugger-run-to-breakpoint-cost]]) and report only on hit —
never streaming per-instruction steps over the air; `dbg:*` is line-oriented request/response (no live
TTY assumed, [[driving-debugger-interactively]]); single-step latency lands where "speed is irrelevant."
Bonus of desktop-first: the entire porta-debug loop can be built and tested between two **localhost**
processes — zero hardware latency.

## The reshaped ladder (desktop-first, Model B, nodus keeper)

| # | Sub-project | Where | Proves | Depends on |
|---|---|---|---|---|
| **R0** | **oevm Model B `.oimg` loader/deserializer** — resident VM loads a clustered `.oimg` (host). **STATUS: built & shipped** — Path B runs the full suite (70/0/4) at 64- *and* 32-bit; format_version 5. R0-as-rung = *promote to primary + doc refresh + harden*, not build. **Done:** the 32-bit header-padding bug (fixed 2026-06-25, ns-oevm `01b85fa`/oevm `c3f739f`); the 32-bit portability gate (wired into `run_tests_oevm.py`); `deployment-model.md` refresh. **Remaining (follow-ups):** immutability guard (abort vs SIGSEGV on blob write), reachability pruning (orphan-method bloat → porta OTA size). | oevm | the primary deployment path | — (foundational) |
| R1 | **ns-posix node skeleton** — nodus-style keeper (host C) supervising the resident VM; install/run/lifecycle; posix backend. **STATUS: DONE 2026-06-25** — repo `github.com/davidg238/ns-posix` (private), 7 commits `9875648..e863a27`, final whole-branch review (opus) clean. Reused nodus-st's VM-neutral keeper core verbatim (only new C = `posix/oimg_vm.c`, popen `oevm_img`, fault-isolated). End-to-end PROVEN cold: deploy `01_method_call.ns` → real `porta serve` → keeper TFTP-downloads `.oimg` → runs on resident oevm VM → prints `42` → reports observed state. **Real porta was folded into R1** (reused `udp_transport.c`), so R1 also delivered R2's transport+install. | ns-posix (new repo) | the keeper interface on desktop | R0 |
| R2 | **porta client refinements** — transport+install+report already shipped in R1; R2 narrows to **telemetry** (`data?id=` JSONL: surface VM run-failure + captured stdout) and **OTA-delta**/partial-graph messages | ns-posix + porta | richer fleet plane on desktop | R1 |
| R3 | **extend porta for debug + bridge `dbg:*`** over it — remote-debug the ns-posix node from a porta client | porta + ns-posix | remote debug end-to-end (desktop) | R2, debugger (exists) |
| R4 | **GPIO capability** — oevm cap prim + **host-mock backend** + Newspeak `Pin` facade | oevm + ns-oevm | the confined-capability pattern (desktop) | R1 |
| R5 | more capabilities — i2c, networking/Thread (same pattern) | oevm + ns-oevm | breadth of the capability model | R4 |
| R6 | **ns-zephyr port** — swap `posix`→`zephyr` backend (real GPIO/net), flash; keeper/porta/Model-B/debug already proven | oevm/backend + ns-zephyr | the thesis **on hardware** | R1–R5 |
| R7 | **fibers** (stretch) — concurrency for event-driven orchestration | oevm | the concurrency pillar | substantial |

R7 fibers **shipped 2026-06-29** (both paths, dialect #18), but the Zephyr hijack
underneath them was architecturally broken for multi-fiber interleave (`ABBBAA`) —
this forced the **de-recursify rewrite** (Toit-model flat interpreter), tracked as
its own sub-ladder: L0 (frozen ABI) → L1 (first-class `Stack` + precise GC) →
core-de-recursify **Phase 1** (finish lowering) → **Phase 2** (the flip: recursive
→ flat loop + in-loop unwinder) → **Phase 3** (swap-`Stack` fibers + demolish
ucontext/hijack/conservative-scan). **De-recursify Phase 3 (fibers + demolition) —
STATUS: DONE 2026-07-01** — one universal single-C-stack fiber-switch mechanism on
all targets (the `#ifdef __ZEPHYR__` hijack + its POSIX ucontext twin both deleted,
not merely unified); conservative C-stack GC scan deleted in favor of fully precise
rooting; `ABBBAA→ABABAB` proven architecturally impossible to regress (host
`-D__ZEPHYR__` repro). Suite 82/0/6 both paths + all 5 gates + structural
demolition greps clean. Design: `oevm/docs/archive/fiber_derecursify_core_design.md` §5.

Classlib grows **as required** by the active rung — not its own step. Per the decomposition (oevm#10),
R0/R4/R5 (loader, capability prims, backend) are **language-neutral** — a future `st-oevm` inherits them.

## Frontends — language targets on the oevm substrate

The oevm core (loader, capability prims, backends, GC, debugger, fibers) is **language-neutral**; a
frontend is a compiler emitting oevm register bytecode + the matching object/class graph. The substrate
pays off most for a language that has the object model but **lacks** an embedded/capability/OTA runtime.

- **`ns-oevm` (Newspeak)** — the current, mature frontend; drives the rungs above.
- **`st-oevm` (Smalltalk)** — the planned 2nd frontend (spec-only today). The *natural* fit: Smalltalk
  has the object model but no embedded story, so oevm supplies exactly what it's missing. Inherits the
  language-neutral rungs.
- **Toit-on-oevm — assessed 2026-06-27 (David's question), verdict: plausible-as-experiment, NOT
  practical-as-product.** Toit's cooperative **tasks/`monitor`** model is the gating dependency → needs
  **R7 fibers** first; the VM fit is genuinely fine (fibers map 1:1 onto oevm's per-call register/frame
  file — a fiber is its own register-stack region + saved `ctx`; and fiber-aware capability I/O is the
  same mechanism that retires the `42_udp.ns` busy-wait). Object models are close (class-based, single
  inheritance, blocks). **But the cost is dominated by the runtime surface, not the VM:** (1) a new Toit
  compiler backend targeting oevm's ISA/layout (Toit's C++ compiler isn't built for pluggable backends);
  (2) reimplementing Toit's large core lib + hundreds of system primitives + the driver/service
  ecosystem; (3) a real **impedance mismatch** — Toit reaches privilege via a **service/RPC** model
  (`system.services`, containers), oevm via **confined capability objects**. And the strategic catch:
  Toit **already ships** a mature VM + tasks + OTA + debugger + containers, so retargeting it largely
  *reimplements Toit's own strengths on a younger VM* (weak ROI) — and it loses Toit's compact
  tree-shaken snapshot (oevm's object-graph image is heavier). **Higher-value moves instead:** (a) let
  Toit and oevm nodes **coexist under porta** (already the ecosystem shape — `toit-zephyr` runs
  alongside); (b) **borrow Toit's task/monitor/service design as the reference** when designing oevm's
  R7 fibers + capability concurrency, rather than hosting Toit itself.

## Confirmations (2026-06-25, David) — the GPIO-first revision is accepted

The earlier plan led with GPIO-on-hardware (Model A appliance). The four steers moved the **infra ahead
of hardware-GPIO**, and David confirmed:
- ✅ **Lead is R0** (the `.oimg` loader), the foundational piece — not a GPIO slice.
- ✅ **`ns-posix` is a new repo** (the NS×host integration; name matches the `posix` backend target).
- ✅ **Keeper: decide by elegance/efficiency, not minor reuse.** David: "whatever is cleaner, more
  interested in elegance/efficiency than minor reuse." So reuse-vs-own is a **design call made at R1**,
  after reading `nodus`/`nodus-st`'s actual C — not a reuse mandate. *Preliminary lean (to validate at
  R1):* the reusable asset is the **porta-protocol contract + lifecycle shape**, not the Berry/ST-coupled
  keeper code; likely grow a clean, oevm-native nodus-style keeper for `ns-posix` and lift only genuinely
  VM-neutral C where it's clean. Decide on evidence at R1.

**Next action (revised 2026-06-25):** R0's load+run path was found **already built & shipped** (it became
the substrate the std-lib + in-image-debugger work sits on). Recon closed the one correctness gap — a
32-bit `.oimg` header-padding bug (fixed + gated + pushed) — and refreshed `deployment-model.md` to mark
Model B primary. Remaining R0 items (immutability guard, reachability pruning) are filed follow-ups, not
blockers. R1 (the ns-posix nodus-style keeper) is now **DONE** — built subagent-driven, reviewed clean,
end-to-end proven against real porta (`github.com/davidg238/ns-posix`, `9875648..e863a27`); reusing
nodus-st's VM-neutral keeper core was the decisive elegance/efficiency call (only new C = `oimg_vm.c`).
Because real porta folded into R1, R2's transport+install are already shipped. **The live front is now
R2-narrowed (telemetry + OTA-delta) or R4 (GPIO capability, the confined-capability pattern on a host
mock).** Spec questions for David: prioritize R2-telemetry vs jump to R4-GPIO; and schedule the nodus-st→
depends-on-ns-posix rewiring (nodus-st#1).
