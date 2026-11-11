# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (2026-11-10):** #477 (P1) on `477-compaction` (`27fb6e16`, pushed, not merged). Slide built; host check-in app runs in 20 KB (fails off). First action: David's three answers below, then the c6spare stress run: `rm -rf` the C6 build dir, `OEVM_COMPACT_STRESS=1 tests/target/run.sh bridge-health`, read the `gc: COMPACT_STRESS` lines (slides, µs per slide).
- **Awaiting David (#477):** (1) invarlint new 40: make `gc_collect`/`gc_compact`/`gc.c:visit_roots` implementers of Boot, Interpreter, WellKnownSymbols, then save the baseline? (2) re-bank footprint (+1.2 KB text)? (3) earn compaction at allocation failure (slots in `vm_protected_fire`/`vm_on_do`/`vm_ensure` + the prim), or stop at 20 KB? Record: design 2026-11-09-477, addendum "later".
- **Everything else waits for David** (2026-11-10): the stack size (1536 proposal), #440's OCM.1 flash, the rest below.
- **#440 thread (msi):** R2's last bench leg on `440-r2-bench` (worktree `~/rete-ocm`): David's go to flash `PortaBridgeNodeBle` on OCM.1 (Pixel re-pair); then PR, close R2, then R3 (#458).
- **#432 (opti7040 thread)** on `432-patch-porta`: Task 9 bench, close-out, master merge; re-measure the footprint re-bank, don't pick one.
- **Then (David, 2026-11-02):** #383, #437; then #443, #455.
- **Awaiting David (other):** rebuild gw's `porta-444` with #480's `blob put --node`; PR #476 then the #81 `review/` promotions; `review/` lines naming `startTicking:`; #458/#459 setup; Model A build on z170 (#472); ACC01 reflash; #455; #368's `review/` files; #201/#202; #396; #429; #175; #413/#424; bands #294/#295. Delayed: #427.
- **Bench:** MagTag (`7cdfa11aaaaa`) runs `gcc792554-dirty-20261110T075913` (debug+profile + `OEVM_STACK_HW`), app `faa71dfc`. c6spare runs `g53ca8817-dirty`. OCM.1 runs R2 + ttgm `AccMotionBle`. Production porta is `porta-444`; store writes need David's hand.
- **Decided — do not re-litigate:** compaction safe point = the wait at interpreter depth 1; 0 identity hashes → sticky refusal flag; a before-reaction safe point and bump-tail placement measured no gain (design 2026-11-09-477 addenda); segregation no gain; R2 as built; a full timer table raises (#81); public, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE pair-once (#439); custos = nsl peer; share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #482 (grant hang), #479 (no-debug MagTag halts), #475, #474, #473, #472, #469, #467.
