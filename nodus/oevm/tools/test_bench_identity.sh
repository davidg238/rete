#!/usr/bin/env bash
# nodus/oevm/tools/test_bench_identity.sh — the bell interlock (rete#185) and the
# instance<->adapter resolver (rete#170), both falsified.
#
# Pure host test: no board is touched and no flash is ever issued. Every port
# this exercises is either a synthetic path under a temp dir or a read-only
# lookup of /dev/serial/by-id, and the end-to-end cases below are deliberately
# aimed at a port that DOES NOT EXIST — so if the guard ever regresses, the
# worst outcome is esptool failing to open a bogus path, never a write to the
# bell. A test that proves a safety interlock must not be able to trip it.
#
# Run:  bash nodus/oevm/tools/test_bell_identity.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNSH="$HERE/../zephyr/run.sh"
. "$HERE/bench-identity.sh"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  ok   $*"; }
bad()  { fail=$((fail+1)); echo "  FAIL $*" >&2; }
deny() { if bell::is_bell_port "$1"; then ok "denied: $2"; else bad "NOT denied: $2 ($1)"; fi; }
allow(){ if bell::is_bell_port "$1"; then bad "wrongly denied: $2 ($1)"; else ok "allowed: $2"; fi; }

echo "-- the two adapters, by name"
# LITERAL, never derived from the constants under test. Building these out of
# $BELL_BYID_* is what an earlier draft of this file did, and it passed with the
# JTAG identity replaced by a bogus string — the test constructed whatever path
# the constant described, so it could not detect a wrong constant at all. The
# only thing that catches "the hash is wrong" is a hash written down twice, in
# two places, by two hands.
BELL_JTAG="usb-Espressif_USB_JTAG_serial_debug_unit_20:6E:F1:19:54:38-if00"
BELL_CONS="usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_4e5729ee0172f01199e0049f1045c30f-if00-port0"
deny  "/dev/serial/by-id/$BELL_JTAG" "bell JTAG — the port zephyr/run.sh flashes"
deny  "/dev/serial/by-id/$BELL_CONS" "bell console — the port the harness flashes"
allow "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:44:6E:60-if00" "c6spare JTAG"
allow "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:45:47:A4-if00" "c6bench JTAG"
allow "/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_d2e21074c7beed11a5935a4371c9e8b5-if00-port0" "c6spare console"
allow "" "empty port"

echo "-- the constants themselves, against the literals above"
case "$BELL_JTAG" in *"$BELL_BYID_FLASH_SUBSTR"*) ok "BELL_BYID_FLASH_SUBSTR names the bell's JTAG" ;;
  *) bad "BELL_BYID_FLASH_SUBSTR='$BELL_BYID_FLASH_SUBSTR' does not occur in $BELL_JTAG" ;; esac
case "$BELL_CONS" in *"$BELL_BYID_CONSOLE_SUBSTR"*) ok "BELL_BYID_CONSOLE_SUBSTR names the bell's console" ;;
  *) bad "BELL_BYID_CONSOLE_SUBSTR='$BELL_BYID_CONSOLE_SUBSTR' does not occur in $BELL_CONS" ;; esac

echo "-- THE REGRESSION #185 REPORTED: the console hash alone cannot see the flash path"
# This is the whole defect in one assertion. Before the fix, target.sh's
# TARGET_DENY_BYID_SUBSTR was the console hash and run.sh had nothing; a
# deny-list naming one adapter reads as protection of the board and is not.
case "/dev/serial/by-id/$BELL_JTAG" in
  *"$BELL_BYID_CONSOLE_SUBSTR"*) bad "console hash matched the JTAG path — premise of this test is wrong" ;;
  *) ok "console hash does NOT match the bell's JTAG path (so the old guard could not have fired)" ;;
esac

echo "-- an explicit pin carrying no identity, resolved to the device node"
# rete#185's live case: ESPTOOL_PORT pinned to a /dev/ttyACM* name. pick_port
# returns early on it, so this is the one input no substring test can see.
mkdir -p "$TMP/dev/serial/by-id"
: > "$TMP/dev/ttyACM9"
ln -s "$TMP/dev/ttyACM9" "$TMP/dev/serial/by-id/$BELL_JTAG"
# Same canonical target, different name — the predicate must resolve, not match.
if [ "$(readlink -f "$TMP/dev/serial/by-id/$BELL_JTAG")" = "$(readlink -f "$TMP/dev/ttyACM9")" ]; then
  ok "fixture: the anonymous name and the by-id name resolve to one device"
else
  bad "fixture is broken"
fi
if [ -e "/dev/serial/by-id/$BELL_JTAG" ]; then
  real=$(readlink -f "/dev/serial/by-id/$BELL_JTAG")
  deny "$real" "the bell's REAL resolved device node ($real) via an anonymous name"
else
  echo "  skip the bell's JTAG is not attached to this bench — anonymous-pin case not exercised"
fi

echo "-- end to end through zephyr/run.sh (aimed at a port that does not exist)"
GHOST="$TMP/dev/serial/by-id/ghost-$BELL_JTAG"     # matches by substring; no device behind it
out=$(ESPTOOL_PORT="$GHOST" bash "$RUNSH" --board esp32c6 --flash --node TimerNode.nsl 2>&1); rc=$?
# All piped greps consume the stream (no -q): under pipefail a first-match exit
# SIGPIPEs the writer and 141 flips the check — including the NEGATED one below,
# where it reads as a false green (rete#260).
if [ "$rc" = 2 ] && printf '%s' "$out" | grep "is the ship's bell" >/dev/null; then
  ok "run.sh --flash with the bell pinned explicitly: refused, exit 2"
else
  bad "run.sh did NOT refuse an explicit bell pin (rc=$rc)"; printf '%s\n' "$out" | tail -5 >&2
fi
if printf '%s' "$out" | grep "west build" >/dev/null; then
  bad "run.sh built before refusing — the guard is downstream of the build"
else
  ok "refused before the build, not beside the flash"
fi

out=$(I_REALLY_MEAN_THE_BELL=1 ESPTOOL_PORT="$GHOST" bash "$RUNSH" \
        --board esp32c6 --flash --node NoSuchNode.nsl 2>&1); rc=$?
# Asserts the door opens and NOT what happens after it: everything downstream is
# bench-dependent (how many CP2102Ns are plugged in), and pinning that here would
# make this test go red for a reason that has nothing to do with the interlock.
if printf '%s' "$out" | grep "flashing THE SHIP'S BELL" >/dev/null \
   && ! printf '%s' "$out" | grep "ERROR: .* is the ship's bell" >/dev/null; then
  ok "the deliberate door opens under I_REALLY_MEAN_THE_BELL=1"
else
  bad "I_REALLY_MEAN_THE_BELL=1 did not open the door (rc=$rc)"; printf '%s\n' "$out" | tail -5 >&2
fi

out=$(ESPTOOL_PORT="$TMP/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:44:6E:60-if00" \
        bash "$RUNSH" --board esp32c6 --flash --node NoSuchNode.nsl 2>&1); rc=$?
if ! printf '%s' "$out" | grep "ship's bell" >/dev/null; then
  ok "a disposable board is not refused (the guard is not a blanket no)"
else
  bad "the guard refused a disposable board (rc=$rc)"; printf '%s\n' "$out" | tail -5 >&2
fi

echo "-- rete#170: the instance <-> adapter resolver, on a synthetic bench"
# A private config dir, so this asserts the RESOLVER and not the bench's current
# cabling — which changes within a session and would make this test flap.
export BENCH_CONFIG_DIR="$TMP/config"
mkdir -p "$BENCH_CONFIG_DIR"
: > "$TMP/dev/ttyFAKE_bell"; : > "$TMP/dev/ttyFAKE_spare"
printf 'RETE_CONSOLE_PORT=%s\n' "$TMP/dev/ttyFAKE_bell"  > "$BENCH_CONFIG_DIR/console-c6.env"
printf 'RETE_CONSOLE_PORT=%s\n' "$TMP/dev/ttyFAKE_spare" > "$BENCH_CONFIG_DIR/console-c6spare.env"

[ "$(bench::board_for_port "$TMP/dev/ttyFAKE_spare")" = c6spare ] \
  && ok "port -> board (c6spare)" || bad "port -> board did not answer c6spare"
[ "$(bench::port_for_board c6)" = "$TMP/dev/ttyFAKE_bell" ] \
  && ok "board -> port (c6)" || bad "board -> port did not answer the bell's port"

# THE #170 DEFECT ITSELF: a flash to c6spare must not resolve to c6.
[ "$(bench::board_for_port "$TMP/dev/ttyFAKE_spare")" != c6 ] \
  && ok "c6spare's port does NOT resolve to c6 (the bug this closes)" \
  || bad "c6spare's port resolved to c6"

# A symlink under another name must resolve, not merely string-match — run.sh
# holds a by-id path where the env file may hold anything naming one adapter.
ln -s "$TMP/dev/ttyFAKE_spare" "$TMP/dev/by-another-name"
[ "$(bench::board_for_port "$TMP/dev/by-another-name")" = c6spare ] \
  && ok "resolves through a differently-named symlink" || bad "did not resolve a symlink to the same device"

# Refuse to guess rather than pick: two instances claiming one adapter is a
# misconfigured bench, and picking one is how the wrong board gets named.
printf 'RETE_CONSOLE_PORT=%s\n' "$TMP/dev/ttyFAKE_spare" > "$BENCH_CONFIG_DIR/console-dupe.env"
bench::board_for_port "$TMP/dev/ttyFAKE_spare" >/dev/null 2>&1 \
  && bad "resolved an ambiguous adapter instead of refusing" \
  || ok "two instances claiming one adapter -> refuses, does not guess"
rm -f "$BENCH_CONFIG_DIR/console-dupe.env"

bench::board_for_port "$TMP/dev/ttyACM9" >/dev/null 2>&1 \
  && bad "resolved a port no env file names" || ok "an unowned port -> refuses"
bench::board_for_port "" >/dev/null 2>&1 \
  && bad "resolved an empty port" || ok "an empty port -> refuses"
unset BENCH_CONFIG_DIR

echo
echo "passed: $pass  failed: $fail"
[ "$fail" = 0 ] || exit 1
echo "BELL INTERLOCK OK"
