# nodus/tuvm/tools/bell-identity.sh
#
# THE ONE HOME for the ship's bell's adapter identities, and the predicate that
# answers "would writing to this port hit the bell?".
#
# Sourced, never executed. Sourced by BOTH deploy paths, which is the whole
# point (rete#185):
#   nodus/tuvm/zephyr/run.sh              the hand path
#   nodus/tuvm/tests/target/lib/target.sh the harness path
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
