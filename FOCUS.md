# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS: #233 — program OTA through porta.** Merged to master 2026-10-10 (`ed41787`). S1, T9, S2, S3 done:
  all four scenarios proven on c6spare (#233 comments, 2026-10-10); E §9 carries the bench numbers. **Next session starts
  with:** branch from master; the MagTag on USB (reflash with the
  two-slot layout, `OEVM_OTA_PUBKEY=$(cat ~/porta-v2/porta.pub on gw)` exported before `run.sh`), one push over WiFi; then
  the bell (`flash-bell.sh`, daylight), one push over Thread; then #398 (porta HTTP push front, Appendix D).
- **Awaiting David:** bring the MagTag to the bench; read E draft 4 (§9 OTA line changed today) and L;
  LICENSE and history rewrite (#368); close #314; #396; #175's memo (the runner already holds the rejected hash).
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on USB runs the S2 firmware (build `…T111913`), slot a =
  v2' (`a9af3a…`, cable), slot b = v3 (`726e8b…`, signed, active), goal = v3 (`~/porta-v2/goal-v3.hex` on gw). Reset it with
  `POST /board/c6spare/lend?s=25` then esptool `--after hard_reset chip_id`, then `/return`. MagTag `7cdfa11aaaaa` fridge clock.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  **OTA: code-push design §10 T1–T9 (2026-10-10)** — firmware is the runner, two 768K slots, NVS manifest, MCUboot's ed25519,
  the cable rule, one-boot trial + 150 s, the fetch on its own thread, RAM cursor (T7: a reset refetches), CLI push front for now.
- **Weekly review, not scheduled:** #175, #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399.
