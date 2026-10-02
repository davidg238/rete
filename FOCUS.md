# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **Next session starts with: #311, the debugger on the C6** (David, 2026-10-02), branch `311-line-in-spike`. First action:
  rebase it onto `master` (#118 merged there at `3f8e82a`). Then the per-bytecode hooks, the enable flag, and a held program that has timers. The input path is proven;
  record `docs/results/2026-10-02-311-line-in-spike.md`.
- **#118/#242 DEPLOYED, MERGED and CLOSED (2026-10-02).** Gateway serves manifest `d7eefc87…` (rollback
  `~/porta-v2/MANIFEST.cddl.pre118`). Five nodes live on factory ids; the table is in
  `docs/results/2026-10-02-118-node-id-board-proof.md` (Deploy). Promoted to `porta/docs/ARCHITECTURE.md` §3.8 and
  `protocol/PROTOCOL-NSL.md` §5.3.
- **Decided 2026-10-02 — do not re-litigate:** bench-named spaces are historical from today (David). `7adf` stays on the July
  echo soak, not reflashed. The nRF id forces the top two bits (design 2026-09-11 addendum). **No SLIP** (#311 record).
  0.x first, 0.1 = one C6 on one cable, freeze is 1.0.0 (`docs/overview.md`, #253).
- **Not proven:** no nRF on the bench has raw top two bits clear, so the forcing has never changed an id (`nrf92c2` reads
  `ec…`). Why an UNPOWERED DS3231 blocks the bell's boot forever is not traced (#317). Why a polled reader hangs the
  C6 is not traced.
- **Bench (2026-10-02 15:25 MST):** the bell is back on its wall supply and chimed 15:30; its clock module now takes 3V3 from the C6; `c6spare` unplugged; `c6bench` on the gateway's USB; both dongles running the bridge, location unverified; RP2350-Zero unplugged.
- **Awaiting David:** `review/DOCMAP.md` line 56 (still cites `protocol/v1/`); the three cabot relay items; where #297 goes; #300.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #296–#300, #303, #304, #306–#309, #315–#318. Liveness next: #272.
