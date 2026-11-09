# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session (msi) starts with:** #440 (P0), R2 on its own branch from master. #81 is settled and merged (`db29b7c9`): a full timer table raises in the arming job, and the node lives. First action: build a fresh invarlint from `~/invarlint` (the checked-in binary is stale), then the R2 builder (check-in as reactions, plus ttgm's chair apps at 60 s with `misses: 4`) under `~/invarlint/skills/invarlint-designer/SKILL.md` (a file; read it). The reviewer is a fresh Fable agent given only the PR number and the `invarlint-review` skill file. `~/rete-440` is the designer's worktree. R2's board gate should also show #81's full-table raise on target (then close #81).
- **#432 (opti7040 thread)** on `432-patch-porta`: Task 9 bench, close-out, master merge. Its footprint re-bank (81437) will conflict with master's 80562 at merge; re-measure, don't pick one.
- **Then (David, 2026-11-02):** #383, #437; then #443, #455.
- **Awaiting David:** #432 Task 9 ("restart porta" on gw, bell + MagTag at the bench); the current-measurement setup (#458, #459); the Model A build on z170 after R1's kernel change (optional); review/ promotions for #432 and #81; whether invarlint's generated `nodus/oevm/invarlint/terms.md` is tracked; the ACC01 reflash from master; the thermal board (#455); #368's 14 `review/` files; #201/#202; #396; paper E/L (#429); #175; #413/#424; bands #294/#295. Delayed: #427.
- **Bench:** c6spare carries R1's `PortaBridgeNodeC6`. OCM.1 runs `AccMotionBle-tick3`, always on; console at custos gw :6994. Production porta is `porta-444`; gateway store writes need David's hand.
- **Decided — do not re-litigate:** the seam rule, shape B, radio verbs, the 1 s accept pause, and idle evidence at R2; a full timer table raises in the arming job (#81, `timer_backend.h`) (design `docs/design/2026-11-04-440-vm-zephyr-seam-crc-design.md`); public, history kept, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE mainline + pair-once (#439); custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12).
- **Weekly review:** `gh issue list -l P3` (unscheduled), newest #465 (run.sh absolute --build-dir), #464 (one k_timer per earliest deadline).
