# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session (msi) starts with:** #440 (P0). R0 and R1 are merged (`fd081298`). First action: settle #81 (a full timer table `k_oops()`es, and a bridged node holds 3 of 8 slots), then put the R2 builder on its own branch (check-in as reactions, plus ttgm's chair apps at 60 s with `misses: 4`) under `~/invarlint/skills/invarlint-designer/SKILL.md` (a file; read it). The reviewer is a fresh Fable agent given only the PR number and the `invarlint-review` skill file. Build a fresh invarlint from `~/invarlint`; the checked-in binary is stale. `~/rete-440` is the designer's worktree; it needs `node_modules` and the Thread `.local.conf` (both done).
- **#432 (opti7040 thread)** on `432-patch-porta`: Task 9 bench, close-out, master merge. Its footprint re-bank (81437) will conflict with master's 80562 at merge; re-measure, don't pick one.
- **Then (David, 2026-11-02):** #383, #437; then #443, #455.
- **Awaiting David:** #432 Task 9 ("restart porta" on gw, bell + MagTag at the bench); the current-measurement setup (#458, #459); the Model A build on z170 after R1's kernel change (optional); review/ promotions for #432; the ACC01 reflash from master; the thermal board (#455); #368's 14 `review/` files; #201/#202; #396; paper E/L (#429); #175; #413/#424; bands #294/#295. Delayed: #427.
- **Bench:** c6spare carries R1's `PortaBridgeNodeC6`. OCM.1 runs `AccMotionBle-tick3`, always on; console at custos gw :6994. Production porta is `porta-444`; gateway store writes need David's hand.
- **Decided — do not re-litigate:** the seam rule, shape B, radio verbs, the 1 s accept pause, and idle evidence at R2 (design `docs/design/2026-11-04-440-vm-zephyr-seam-crc-design.md`); public, history kept, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE mainline + pair-once (#439); custos = nsl peer (D1–D2); share only `protocol/` (#338); agent first (D11–D12).
- **Weekly review:** `gh issue list -l P3` (unscheduled), newest #463 (no C6 http board scenario).
