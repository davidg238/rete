# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with:** #477 (P1), branch `477-frozen-bytes`, PR #483. `frozenBytes` copies once (host check-in app: 22 KB passes, 20 KB fails on fragmentation); stack high-water C6 1077 / MagTag 1053 words (design addendum 2026-11-10). First action: David's call on the stack size (proposal 1536 words, after a debugger-stop and OTA measurement) and on building compaction.
- **#480 (P2), PR #481:** `porta blob put --node <id>` writes goal/apps/main itself. After merge, gw's `porta-444` needs replacing (David's hand).
- **#440 thread (msi):** R2's last bench leg on `440-r2-bench` (worktree `~/rete-ocm`). First action: David's go to flash `PortaBridgeNodeBle` on OCM.1 (needs a Pixel re-pair); then PR, close R2, then R3 (#458).
- **#432 (opti7040 thread)** on `432-patch-porta`: Task 9 bench, close-out, master merge; re-measure the footprint re-bank, don't pick one.
- **Then (David, 2026-11-02):** #383, #437; then #443, #455.
- **Awaiting David:** PR #476 (#81 target probe) then the #81 `review/` promotions; `review/` lines naming `startTicking:`; #432 Task 9; #458/#459 setup; Model A build on z170 (#472); ACC01 reflash; #455; #368's `review/` files; #201/#202; #396; #429; #175; #413/#424; bands #294/#295. Delayed: #427.
- **Bench:** MagTag (`7cdfa11aaaaa`) runs build `gcc792554-dirty-20261110T075913` (the debug+profile recipe plus the first, push-time `OEVM_STACK_HW`), app `faa71dfc`, goal seq 26. The no-debug variant halts it after the first pull (#479). c6spare runs `g53ca8817-dirty` with the probe. OCM.1 runs R2 + ttgm `AccMotionBle`. Production porta is `porta-444`; store writes need David's hand.
- **Carry:** Timer card `owns:` line for `timer_zephyr.c` in the next card PR; rebuild the C6 compile database before the next whole-target review (#435).
- **Decided — do not re-litigate:** arena: segregation no gain, escape analysis buys GC time not arena, compaction safe-point only (design 2026-11-09-477); R2 as built; a full timer table raises (#81); MagTag non-debug runtime: leave as is (David 2026-11-09); public, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE pair-once (#439); custos = nsl peer; share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #482 (grant hang), #479 (no-debug MagTag halts), #475, #474, #473, #472, #469, #467.
