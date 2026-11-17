# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (David, 2026-11-17):** **ztoit#30** = WiFi credentials from a run-time store, device side (the image carries no SSID/PSK; faber writes them later, no Zephyr-shell stopgap). Base: ztoit main `29683dc` (local, deliberately unpushed; its README WiFi paragraph is rewritten with #30), fork `wip-plan2-fix-a` `d6a1d7fe`. First: measure `CONFIG_WIFI_CREDENTIALS` + `NET_REQUEST_WIFI_CONNECT_STORED` RAM on the WROOM-32 (`/dev/ttyUSB0`, mine); delete `TOIT_WIFI_SSID/_PSK` + the `.example` lines in the same change; hashes to ztoit-11. Do NOT reflash C6 47a4 or dongle 2dbf (ztoit#25 soak).
- **#521 (still open):** merged into ztoit via ztoit#25 (ztoit `a60917e`). Left: agent slices 2+ (goals, `obs/liveness` with reportS), gw test once #443 merges (gw porta has 5-element rows). WiFi blob leak guard `86a1e21`, upstream zephyr#126157.
- **Then:** #443 on `443-outbox-journal` (tasks 1–6 done; task 7 needs a working OCM.1/ACC01 + porta with 6-element rows on gw; vm.md journal section unwritten); #440 R2; #432; #449; #296/#297; VM stack size; #383, #437; #455.
- **faber thread:** open for faber #516, #517, #518, #519. **Toit thread:** #508, #507, #506 only when ztoit asks.
- **Sensors (queued):** ocm#2 first (gates #447); OCM.1 silent since ~11-11, ACC01 since ~11-13; #498; #459; #503.
- **gw:** production `porta-515` = master `84316bc4`, `--coap :5683`, in `~/porta-v2`; built with cgo. The bell and `d2666d…` are refused every push (#512).
- **Awaiting David:** #443 task 7 route (merge first vs one-off porta); PR #500; #511; #81 `review/`; #458/#459 setup; #472; #455; #201/#202; #396; #429; #175; #413/#424; #294/#295; #427 delayed.
- **Decided — do not re-litigate:** default Thread build is MTD (#524); #521 = ztoit on Zephyr, not plain Toit (2026-11-17); WiFi build IPv4-only; the agent's check-in stays in C (safe mode must reach porta); zcbor for the agent's CBOR; Toit debug pair in `gw/apps` (#515); CoAP, no DTLS yet (2026-11-16-505); store-until-acked (#443); OTA via A/B (#449); share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #525, #524, #523, #522, #519, #518, #513, #512.
