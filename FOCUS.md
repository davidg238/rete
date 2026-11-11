# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (2026-11-12, msi, #440 seam):** master `f2863a7f` (R5 #488 and R6a #489 merged). First action: `invarlint-review 493` on `440-r3-radio` (`e0ff2115`; R5 merged in, `linkBudget` 22 s), then David's merge word; then R6b (prim switch per seam) and R6c (one source list).
- **Bench:** OCM.1 is on the **Thread mesh** (David 2026-11-11; the phone bridge is ttgm's): firmware `b00fdfef` + OTA key, app PortaBridgeNodeNrf `4cb2ff89` (OTA'd), `goal/config/radio/policy` `#perExchange`, boot 5. Its Pixel bond is gone (erased). Production porta `porta-493`; Claude manages gw (`--actor claude`). c6spare regular build; MagTag `gcc792554-dirty`, app `faa71dfc`.
- **Parked:** the Pi BLE relay, `498-relay-linux` (#498 closed, not planned). ttgm's `AccMotionBle` no longer runs on OCM.1.
- **Awaiting David:** R4 (#459) measurement setup; `review/program-sequence.md:2697` and `review/tests/CLAIMS.md:80` still describe the OTA fetch thread (R5 deleted it); #432 Task 9; PR #476 then the #81 `review/` promotions; #472; ACC01 reflash; #455; #201/#202; #396; #429; #175; #413/#424; bands #294/#295. Delayed: #427.
- **Decided — do not re-litigate:** R5 finished = idle, OTA gate on Thread (design 2026-11-04-440, addendum R5); `linkBudget` = twice the worst reconnect on any transport (kernel.nsl, #458); OCM.1 on Thread, phone bridge is ttgm's (2026-11-11); R6 in three revisions; stop at 20 KB (2026-11-09-477); full timer table raises (#81); OTA via A/B (#449); BLE pair-once (#439); share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #499 (no OTA key builds silently), #497, #495, #494, #492, #491, #490, #482.
