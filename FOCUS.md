# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; the pre-2026-09-30
blocks are in `docs/status/focus-history.md`; live state is in the issues and `docs/status/dev-status.html`._

- **0.1 (David, 2026-10-09): one node, every verb — load, debug, patch, profile — through porta, over the radio, while it sleeps.**
  Demos: the ship's bell twice, wall C6 always-on (#380) and MagTag deep-sleep (#379). Lifecycle table: #26.
- **FOCUS: #233 — program OTA through porta.** Branch `233-ota` (pushed, not merged; master has #380). S1, T9, S2 built;
  S3 scenarios 1 (clean deploy, 64 s push→confirmed) and 3 (bad sig rejected) proven on c6spare. **Next session starts with:**
  S3 scenario 4 — `porta put 404cca446e60 goal/apps/main --raw "$(cat ~/porta-v2/goal-never.hex)"` on gw, expect
  `runner: trial a` → 150 s → `runner: revert a -> b`; then scenario 2 (reset mid-fetch); restore `goal-v2.hex` after each.
  Then MagTag, the bell (never until proven), E §9, #398 (porta HTTP push front = Appendix D). Build with
  `OEVM_OTA_PUBKEY=<gw porta.pub hex>` exported before `run.sh`; the key is `~/porta-v2/porta.pub` on gw.
- **Before merging 233-ota:** rebuild + restart the desktop `rete-custos` (its region moved, #399).
- **Awaiting David:** read E draft 4 and L; LICENSE and history rewrite (#368); close #314; #396.
- **Bench:** the bell `c6` is on the WALL (never flash). `c6spare` on USB runs the S2 firmware (build `…T111913`), slot a =
  v2' (`a9af3a…`), goal = v2'. MagTag `7cdfa11aaaaa` fridge clock (old layout; reflash when its turn comes). `c6bench` soak.
- **Decided — do not re-litigate:** custos = nsl peer, porta language-neutral (D1–D2); share only `protocol/` (#338); agent first,
  HTTP the only agent surface (D11–D12); D13–D19; hot patch debug-build only (#313); one suite lock (#314); no third node (#384);
  debug goal = whole set, sha-gated (#375–#377); WiFi node addresses porta by IPv6 (#379); bell = classes by CRC (#380);
  **OTA: code-push design §10 T1–T9 (2026-10-10)** — firmware is the runner, two 768K slots, NVS manifest, MCUboot's ed25519,
  the cable rule, one-boot trial + 150 s, the fetch on its own thread, CLI push front for now and #398 for 0.1.
- **Weekly review, not scheduled:** #280, #282–#287, #290, #291, #298, #299, #303, #304, #306–#309, #315–#322, #324–#328,
  #331–#335, #345–#357, #359–#361, #363, #366, #369–#374, #378, #382–#397, #399.
