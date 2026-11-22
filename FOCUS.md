# Focus

_Rewritten (never appended) at every `/conclude`; under ~15 lines. History: `git log -p FOCUS.md`; live state is in
the issues and `docs/status/dev-status.html`._

- **Next session starts with (2026-11-22):** #526 (P1) — the liveness watchdog on every Zephyr board. First: David confirms the stickered WROOM-32 **b37c** (`84:cc:a8:11:b3:7c`, `/dev/serial/by-path/…usb-0:1.3:1.0-port0`); add `WATCHDOG`/`TASK_WDT`/`REBOOT` to `prj_esp32.conf` (C6's timings), flash an `OEVM_WEDGE_TRIGGER` build, show reboot + RebootNote. Then MagTag (deep sleep, #479), then nRF (OCM.1/ACC01 on the J-Link). New branch `526-watchdog` from master.
- **Boards:** identify by `by-path`, never `by-id` (both EzSBC boards share a USB serial); `read_mac` RESETS. ztoit owns C6 47a4 (`ttyUSB0`) and WROOM f58c (usb 1.4.1); keep off. `~/zephyr-esp32` stays until #528.
- **Just closed:** #443 (board gate PASS on OCM.1, 2026-11-21). OCM.1 runs master `gcfa3b099` + NOR outbox; it had wedged ~10 days with no watchdog (ocm#3 → #526).
- **Suite 2026-11-21:** 324/0; only aux red is `tutorial-board` (#511).
- **Then:** #527 (Newspeak-list post: re-measure the paper, "dialect" vs "derived", #368, draft); #521 agent slices 2+; #440 R2; #432; #449; #296/#297; VM stack size; #383, #437; #455.
- **faber thread:** faber #516–#519. **Toit thread:** #508, #507, #506 only when ztoit asks.
- **Sensors (queued):** ocm#2 (PIR wake; gates #447); ACC01 silent since ~11-13; #498; #459; #503.
- **gw:** production `porta-443` = master `5398746d` (no porta code changed since), `MANIFEST-443.cddl`, `--coap :5683`, `~/porta-v2`, cgo. The bell and `d2666d…` are refused every push (#512).
- **Awaiting David:** board stickers; master fast-forward to the branch head (the classifier blocks my push); PR #500; #511; #81 `review/`; #458/#459 setup; #472; #455; #201/#202; #396; #429; #175; #413/#424; #294/#295; #427 delayed.
- **Decided — do not re-litigate:** nsl is "Newspeak-derived" (README, David 2026-11-21); #443 gate ≥5 rows, not 50 (design addendum); default Thread build is MTD (#524); #521 = ztoit on Zephyr; WiFi IPv4-only; agent check-in in C; zcbor; Toit debug pair in `gw/apps` (#515); CoAP, no DTLS yet; store-until-acked (#443); OTA via A/B (#449); share only `protocol/` (#338); agent first.
- **Weekly review:** `gh issue list -l P3`, newest #528, #525, #524, #523, #522, #519, #518, #513.
