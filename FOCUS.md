# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (2026-11-21):** David picks the #443 task 7 route (merge `443-outbox-journal` first vs a one-off porta on gw), then do it. Branch `443-outbox-journal`. ztoit#30 is closed (ztoit main `2cf8ebe`, fork zephyr-port `7fb2838e`); build ztoit from origin, not the old local bases.
- **Unchecked:** a gate run in the 11-17 session showed `nsl rc=1` (Go and PORTA SYNC green); rerun the suite before any merge.
- **#521 (still open):** agent slices 2+ (goals, `obs/liveness` with reportS), gw test once #443 merges. faber#2: `keep: false` install seen on gw, run-once-then-erase not yet (rete#519 comment).
- **Then:** #440 R2; #432; #449; #296/#297; VM stack size; #383, #437; #455.
- **faber thread:** open for faber #516, #517, #518, #519. **Toit thread:** #508, #507, #506 only when ztoit asks.
- **Sensors (queued):** ocm#2 first (gates #447); OCM.1 silent since ~11-11, ACC01 since ~11-13; #498; #459; #503.
- **gw:** production `porta-515` = master `84316bc4`, `--coap :5683`, in `~/porta-v2`; built with cgo. The bell and `d2666d…` are refused every push (#512).
- **Awaiting David:** #443 task 7 route; PR #500; #511; #81 `review/`; #458/#459 setup; #472; #455; #201/#202; #396; #429; #175; #413/#424; #294/#295; #427 delayed.
- **Decided — do not re-litigate:** nsl is "Newspeak-derived" in README (David, 2026-11-21); default Thread build is MTD (#524); #521 = ztoit on Zephyr (2026-11-17); WiFi build IPv4-only; agent check-in stays in C; zcbor for the agent's CBOR; Toit debug pair in `gw/apps` (#515); CoAP, no DTLS yet; store-until-acked (#443); OTA via A/B (#449); share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #525, #524, #523, #522, #519, #518, #513, #512.
