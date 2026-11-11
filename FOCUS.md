# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (2026-11-11):** master `2bca8002`; no focus issue — #477 closed 2026-11-10 (safe-point compaction merged, PR #484; we run with it). First action: David picks the next focus from the list below.
- **Candidates, David's pick:** the VM stack size (1536-word proposal); #440's OCM.1 flash; #432; then (David, 2026-11-02) #383, #437; then #443, #455.
- **#440 thread (msi):** R2's last bench leg on `440-r2-bench` (worktree `~/rete-ocm`): David's go to flash `PortaBridgeNodeBle` on OCM.1 (Pixel re-pair); then PR, close R2, then R3 (#458).
- **#432 (opti7040 thread)** on `432-patch-porta`: Task 9 bench, close-out, master merge; re-measure the footprint re-bank (re-banked again 2026-11-10 for #477), don't pick one.
- **Awaiting David (other):** rebuild gw's `porta-444` with #480's `blob put --node`; PR #476 then the #81 `review/` promotions; `review/` lines naming `startTicking:`; #458/#459 setup; Model A build on z170 (#472); ACC01 reflash; #455; #368's `review/` files; #201/#202; #396; #429; #175; #413/#424; bands #294/#295. Delayed: #427.
- **Bench:** c6spare runs the regular build `g7040e732-20261110T172709` (stress run done). MagTag (`7cdfa11aaaaa`) runs `gcc792554-dirty-20261110T075913`, app `faa71dfc`. OCM.1 runs R2 + ttgm `AccMotionBle`. Production porta is `porta-444`; store writes need David's hand.
- **Decided — do not re-litigate:** stop at 20 KB, compaction at allocation failure waits for its earning condition; the collector implements Boot/Interpreter/WellKnownSymbols (design 2026-11-09-477, addendum "later"); safe point = the wait at depth 1; segregation and bump-tail no gain; R2 as built; a full timer table raises (#81); public, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE pair-once (#439); custos = nsl peer; share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #482 (grant hang), #479 (no-debug MagTag halts), #475, #474, #473, #472, #469, #467.
