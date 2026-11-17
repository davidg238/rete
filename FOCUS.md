# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (David, 2026-11-17):** focus **#521**, now ztoit on Zephyr over WiFi on a WROOM-32 (no longer plain Toit; #521 comment 2026-11-17). Fix Toit heap pressure first: `out of memory in 'stack grow'` every other cycle at the 48 KB arena. Work in worktrees `~/ztoit-wroom32` + `~/toit-wroom32` (branches `wroom32-wifi`, ztoit `4436c2b`, fork `8b45073e`, local). Build: `source env.sh; export TOIT_SRC=~/toit-wroom32 TOIT_HOST=~/.local/state/ztoit/sdk-fork-wroom32/toit/bin/toit`; board on `/dev/ttyUSB0` (EzSBC Feather, disposable, David). Then the CoAP agent.
- **ztoit-11 merges** both `wroom32-wifi` branches after its C6/nRF soaks and David's word; send it each new commit hash. Do NOT touch: `~/zephyr-esp32/zephyr` (its uncommitted ztoit#23 fix), the main ztoit worktree, `~/.local/state/ztoit/sdk-fork`, nRF dongle 2dbf, the J-Link nRF52840, C6 47a4 (ttyUSB1).
- **Then:** #443 on `443-outbox-journal` (tasks 1–6 done; task 7 needs a working OCM.1/ACC01 + porta with 6-element rows on gw; vm.md journal section unwritten); #440 R2; #432; #449; #296/#297; VM stack size; #383, #437; #455.
- **faber thread:** open for faber #516, #517, #518, #519. **Toit thread:** #508, #507, #506 only when ztoit asks.
- **Sensors (queued):** ocm#2 first (gates #447); OCM.1 silent since ~11-11, ACC01 since ~11-13; #498; #459; #503.
- **gw:** production `porta-515` = master `84316bc4`, `--coap :5683`, in `~/porta-v2`; built with cgo. The bell and `d2666d…` are refused every push (#512).
- **Awaiting David:** #443 task 7 route (merge first vs one-off porta); PR #500; #511; #81 `review/`; #458/#459 setup; #472; #455; #201/#202; #396; #429; #175; #413/#424; #294/#295; #427 delayed.
- **Decided — do not re-litigate:** default Thread build is MTD (#524); #521 = ztoit on Zephyr, not plain Toit (2026-11-17); WiFi build IPv4-only, socket family = build's (no mapping); Toit debug pair in `gw/apps` (#515); CoAP, no DTLS yet (2026-11-16-505); store-until-acked (#443); OTA via A/B (#449); share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #524, #523, #522, #519, #518, #513, #512.
