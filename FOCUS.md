# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session (msi) starts with:** #440 R2's last bench leg. Branch `440-r2-bench` (from master `1001a48a`; carries the `CONFIG_BT_LONG_WQ_PRIO=0` removal, proven: OCM.1 paired fresh without it). First action: J-Link idle samples on OCM.1 (rete node, then the chair app; RTT via `~/workspace/ttgm/bench/439-ble-spike/rttread.py`), comment them on #440, PR `440-r2-bench`, close R2. Then R3 (radio, #458).
- **#432 (opti7040 thread)** on `432-patch-porta`: Task 9 bench, close-out, master merge. Its footprint re-bank (81437) will conflict with master's 80562; re-measure, don't pick one.
- **Then (David, 2026-11-02):** #383, #437; then #443, #455.
- **Awaiting David:** merge PR #476 (#81's target probe, reviewed clean), then the #81 `review/` promotions (`review/program-sequence.md:1196`, `:1273`, `review/tests/CLAIMS.md`); `review/` lines naming `startTicking:` (`review/lib/timers.ns:133`, `review/tests/CLAIMS.md:94`, `review/program-sequence.md:2537`); #432 Task 9; #458/#459 measurement setup; Model A build on z170 (#472); review/ promotions for #432; ACC01 reflash; #455; #368's 14 `review/` files; #201/#202; #396; #429; #175; #413/#424; bands #294/#295. Delayed: #427.
- **Bench:** OCM.1 runs R2 firmware `g1001a48a` + app `54699d65` (goal seq 12, debug detached), on the Pixel relay. MagTag on R2 with the 34,816 arena (#471 closed). c6spare on msi USB with R2 + the #81 probe. Production porta is `porta-444` (bare `porta` on gw is old, no `put`); gateway store writes need David's hand. Worktrees to remove once merged: `~/rete-471`, `~/rete-r2bench`, `~/rete-ocm`.
- **Carry:** Timer card `owns:` line for `timer_zephyr.c` in the next card PR; rebuild the C6 compile database before the next whole-target review (#435).
- **Decided — do not re-litigate:** `invarlint/out/` tracked, refreshed only at baseline saves (PR #466); R2 as built (design 2026-11-04 "The check-in as reactions"); the seam rule, shape B, radio verbs, 1 s accept pause; a full timer table raises (#81); MagTag non-debug runtime: leave as is (David 2026-11-09); public, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE pair-once (#439); custos = nsl peer; share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #475 (shared /tmp/oevm_img), #474 (stdin in link mode), #473 (udp_send result), #472 (stale fixture), #469 (sleepy leg D), #467 (ed25519 card).
