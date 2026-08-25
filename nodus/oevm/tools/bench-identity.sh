# nodus/oevm/tools/bench-identity.sh
#
# THE ONE HOME for who-is-who on this bench: which adapter belongs to which
# collector instance (both directions), where that instance's log lives, and the
# ship's bell's identities with the predicate that answers "would writing to this
# port hit the bell?".
#
# Two callers, two different questions, ONE mapping (rete#185, rete#170):
#   target.sh  has a board name and needs its port      -> bench::port_for_board
#   run.sh     has a resolved port and needs its board  -> bench::board_for_port
#
# Both read `~/.config/rete/console-<board>.env`, which is the actual source of
# truth — a board exists on this bench exactly when that file does. Nothing here
# carries a table of hashes, and that is deliberate: a table that disagreed with
# the collector's own config would send the flash to one board and read the
# console of the other, and every symptom of that reads as "the board went quiet".
#
# Sourced, never executed. Sourced by BOTH deploy paths, which is the whole
# point (rete#185):
#   nodus/oevm/zephyr/run.sh              the hand path
#   nodus/oevm/tests/target/lib/target.sh the harness path
#
# WHY A SHARED FILE AND NOT A CONSTANT IN EACH: two copies of a hash that
# protects a multi-day soak go stale silently, and the failure is invisible
# until the soak is already over. The two callers do NOT share a policy, only
# the identity — see "policy lives at the call site" below.
#
# WHY TWO IDENTITIES AND NOT ONE: the C6 has two physically separate channels
# and the two deploy paths use DIFFERENT ONES.
#
#   console   CP2102N UART bridge   4e5729ee…            the harness flashes here
#   flash     native USB-JTAG       20:6E:F1:19:54:38     zephyr/run.sh flashes here
#
# This is the defect rete#185 reported: `TARGET_DENY_BYID_SUBSTR=4e5729ee` is
# the CONSOLE hash, so it could never have protected run.sh — run.sh resolves
# `/dev/serial/by-id/*Espressif*`, which the console hash does not match. A
# deny-list naming one adapter reads as protection of the board. It is not.
#
# The other two C6 devkits, for contrast (neither is denied — both are
# disposable, and c6bench has been off the bench since 2026-08-06):
#   c6bench   console ac2dce6f…  JTAG 40:4C:CA:45:47:A4
#   c6spare   console d2e21074…  JTAG 40:4C:CA:44:6E:60
BELL_BYID_CONSOLE_SUBSTR="4e5729ee"
BELL_BYID_FLASH_SUBSTR="20:6E:F1:19:54:38"

# bell::is_bell_port <port> -> 0 if that port is one of the bell's adapters.
#
# Substring first, then RESOLVED DEVICE NODE. The second half is not belt and
# braces: rete#185's live case is an explicitly pinned `ESPTOOL_PORT`, and an
# explicit pin is usually `/dev/ttyACM0` — a name that carries no identity at
# all, so no substring test can see through it. Both sides are resolved to
# their canonical /dev node and compared, which catches the pin.
#
# Answers "no" when it cannot tell (no by-id entry, port absent). That is
# fail-open, and it is the right default HERE because this predicate is a
# backstop behind pick_port's ambiguity check rather than the only guard — and
# because a bench with no bell attached must stay usable. The caller decides
# what "yes" costs.
bell::is_bell_port() {
  local port=${1:-} real bid
  [ -n "$port" ] || return 1

  case "$port" in
    *"$BELL_BYID_CONSOLE_SUBSTR"*|*"$BELL_BYID_FLASH_SUBSTR"*) return 0 ;;
  esac

  real=$(readlink -f "$port" 2>/dev/null) || return 1
  [ -n "$real" ] || return 1
  for bid in /dev/serial/by-id/*"$BELL_BYID_CONSOLE_SUBSTR"* \
             /dev/serial/by-id/*"$BELL_BYID_FLASH_SUBSTR"*; do
    [ -e "$bid" ] || continue
    [ "$(readlink -f "$bid" 2>/dev/null)" = "$real" ] && return 0
  done
  return 1
}

# ---- the instance <-> adapter mapping, both directions ---------------------
# rete#170. run.sh hardcoded the board name `c6` in its post-flash step, so every
# C6 flash archived under the ship's bell and compared against the bell's env
# file whichever board was actually flashed. It bit on 2026-08-09: flashing
# c6spare overwrote `last-flashed-c6` with c6spare's build id — a false statement
# about which firmware is on the board carrying a multi-day soak, written
# silently, during ordinary work.
#
# The resolver these functions implement already existed in one direction
# (`target::board_byid`), on the stated principle that there should be ONE place
# mapping an instance to an adapter. run.sh predates it and did not use it.

BENCH_CONFIG_DIR=${BENCH_CONFIG_DIR:-$HOME/.config/rete}
BENCH_CONSOLE_STORE=${BENCH_CONSOLE_STORE:-$HOME/.local/state/rete/console}

# bench::env_file <board> -> path to that instance's collector env file.
bench::env_file() { echo "$BENCH_CONFIG_DIR/console-$1.env"; }

# bench::log_file <board> -> today's log for that instance.
#
# Re-resolve on every use, never cache: the collector rolls its log at midnight
# and a long run can outlive the path.
bench::log_file() { echo "$BENCH_CONSOLE_STORE/console-$1-$(date +%F).log"; }

# bench::port_for_board <board> -> its console port, or rc 1 and nothing.
# Policy (die vs warn vs default) belongs to the caller.
bench::port_for_board() {
  local envf port
  envf=$(bench::env_file "$1")
  [ -f "$envf" ] || return 1
  port=$(sed -n 's/^RETE_CONSOLE_PORT=//p' "$envf")
  [ -n "$port" ] || return 1
  echo "$port"
}

# bench::board_for_port <port> -> the instance owning it, or rc 1 and nothing.
#
# The reverse direction, and the one rete#170 needed. Matches on the RESOLVED
# device node, not the string: run.sh may hold a by-id path, a /dev/ttyUSB0
# fallback, or an operator's explicit CONSOLE_PORT pin, and all three can name
# one adapter. Ambiguity answers rc 1 rather than picking — two instances
# claiming one adapter is a misconfigured bench, and guessing between them is
# exactly the mistake this file exists to prevent.
bench::board_for_port() {
  local port=${1:-} real envf board bport hits="" n=0
  [ -n "$port" ] || return 1
  real=$(readlink -f "$port" 2>/dev/null) || return 1
  [ -n "$real" ] || return 1
  for envf in "$BENCH_CONFIG_DIR"/console-*.env; do
    [ -f "$envf" ] || continue
    board=${envf##*/console-}; board=${board%.env}
    bport=$(bench::port_for_board "$board") || continue
    [ "$(readlink -f "$bport" 2>/dev/null)" = "$real" ] || continue
    hits="$hits $board"; n=$((n+1))
  done
  [ "$n" = 1 ] || return 1
  echo "${hits# }"
}

# bench::collector_state <board> -> one word on stdout, rc 0 always.
#
#   active     running and reading
#   inactive   the unit is not running
#   no-log     running, but today's log does not exist yet
#   port-down  running and seeing NOTHING (healthy on the bell, whose console
#              goes over Thread; a real fault on any other instance)
#
# Reports, never decides. target.sh dies on anything but `active`; a hand-run
# run.sh only warns, because a missing collector is not a reason to refuse a
# flash — it is a reason to say the boot will not be in the log.
bench::collector_state() {
  local board=$1 logfile
  systemctl --user is-active --quiet "rete-console@$board" || { echo inactive; return 0; }
  logfile=$(bench::log_file "$board")
  [ -f "$logfile" ] || { echo no-log; return 0; }
  # grep consumes the stream (no -q): zephyr/run.sh sources this under pipefail (rete#260).
  if tail -3 "$logfile" | grep "port=down" >/dev/null; then echo port-down; return 0; fi
  echo active
}

# POLICY LIVES AT THE CALL SITE, and the two callers deliberately disagree:
#
#   target.sh  refuses with NO override env. A mislabelled workload is a
#              plausible mistake and an ended multi-day soak is not
#              recoverable, so a test suite has no code path to the bell.
#   run.sh     refuses unless I_REALLY_MEAN_THE_BELL=1. The hand path is where
#              a deliberate reflash of the bell is supposed to happen, so it
#              needs a door — one that cannot be opened by accident.
#
# Keeping that asymmetry in the callers rather than in a parameter here is what
# stops the harness from ever growing an override by inheriting one.
