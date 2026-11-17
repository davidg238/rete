# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (2026-11-16, David's pick):** focus **#521** (P1, a porta client for plain Toit on ESP-IDF over WiFi), new branch from master. First action: brainstorm the shape — read the retired keeper (`~/workspaceToit/nodus/src/supervisor.toit`), ztoit's design (`~/workspace/ztoit/docs/design/2026-11-15-nodus-toit-design.md`, the C agent's CoAP verbs) and `~/workspaceToit/uCoAP_toit`, then ask David which board and which Toit SDK.
- **Then, one per session (David, 2026-11-16: finish the unfinished):** #443 on `443-outbox-journal` (2 commits + 9 uncommitted files, untouched; rebase onto master and renumber — prims 169–174 and tests `22_spacesync/37`, `38` collide with #493's); #440 R2 on `440-r2-bench` (`~/rete-ocm`; David flashes OCM.1), then #458; #432 on `432-patch-porta`; #449 on `449-ota-nrf` (unchecked); #296/#297. Then the VM stack size (1536 words); #383, #437; #455.
- **faber thread (`~/workspace/faber`, faber#1/#2):** #515 closed (App `keep`; `gw/apps/<name>` = `AppDebug{program, snapshot}`, never on the wire). Open for faber: #516 (events read by program id), #517 (list `goal/apps/*`), #518 (blob prune, later), #519 (PROTOCOL-NSL says stop = `runlevel: #stopped`; MANIFEST says `lifecycle`).
- **Toit thread:** #505 closed (CoAP listener). On rete's side only when ztoit asks: #508 (`sys/trace`), #507, #506. Node agent is ztoit#10.
- **Sensors thread (queued):** ocm#2 first (gates #447); rebuild OCM.1 from master; #498; #459; #503.
- **gw (one server, one codebase):** production is `porta-515` = master `84316bc4`, `--coap :5683`, `MANIFEST-515.cddl`, in `~/porta-v2`; `porta-505` kept for rollback. Build with cgo. A probe `gw/apps/probe` cell sits on c6spare (`404cca446e60`), harmless.
- **Bench:** c6spare and C6 `47a4` are ztoit's (`~/workspace/ztoit/FOCUS.md`). OCM.1 silent since ~11-11, ACC01 since ~11-13, not looked at. The bell and `d2666d…` run pre-#444 firmware and are refused every push (#512).
- **Awaiting David:** PR #500 (close it?); #511 (suite red on master, one tutorial line); #81 `review/` promotions; `review/` lines naming `startTicking:`; #458/#459 setup; #472; #455; #368's `review/` files; #201/#202; #396; #429; #175; #413/#424; bands #294/#295. Delayed: #427.
- **Decided — do not re-litigate:** Toit debug pair lives in `gw/apps`, not the App (#515, 256 B cap); CoAP beside UDP, no DTLS yet (design 2026-11-16-505); compaction waits for its earning condition (design 2026-11-09-477); a full timer table raises (#81); public, Apache-2.0 (#368); OTA via A/B (#449); store-until-acked (#443); BLE pair-once (#439); share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #519, #518, #513, #512, #511, #509, #508.
