# nodus/oevm/tests/target/lib/target.sh
#
# The parts every target scenario shares: board resolution and the two deploy
# interlocks, locking, collector preflight, and TAP-lite verification.
#
# Sourced, never executed. Design:
# docs/design/2026-08-05-target-test-harness-design.md
#
# THE RULE THIS FILE EXISTS TO ENFORCE: nothing here knows what any scenario
# means. It sees a board, a class, a build id and TAP lines. Scenario semantics
# live in the workload (D1), which is what lets the same verification run over a
# different transport later (D2).

# ---- the board registry ----------------------------------------------------
# THREE physically identical ESP32-C6 devkits as of 2026-08-06; only the CP2102N
# by-id hash tells them apart, and mistaking one for the other cost most of a
# morning on 2026-08-05.
#
# c6      THE SHIP'S BELL. Its whole value is soaking uninterrupted for days.
#         Usually wallwart-only with console over Thread — but it was given a
#         data cable on 2026-08-06, so `port=down` describes its usual state and
#         not an invariant. Refused on the deploy path either way, by hash.
# c6bench The original spare. Disposable. Off the bench since 2026-08-06 07:46.
# c6spare A third devkit, adopted 2026-08-06 (console d2e21074…, JTAG
#         40:4C:CA:44:6E:60), found running a stock ESP-IDF example. Disposable,
#         and currently the one physically attached.
#
# There is no table of hashes here on purpose: target::board_byid resolves every
# board through the collector's own env file, so a board exists for this harness
# exactly when ~/.config/rete/console-<board>.env exists. Adding a board is
# adding that file and starting its collector instance — never repointing an
# existing one at different hardware, which is the mix-up the pinning prevents.
TARGET_STORE=${TARGET_STORE:-$HOME/.local/state/rete/console}
TARGET_LOCKDIR=${TARGET_LOCKDIR:-$HOME/.local/state/rete/target}
# THE DISPOSABLE BOARD, and the only one a class=disruptive scenario may touch.
# It follows whichever disposable devkit is actually on the bench: edit this line
# when the bench changes, or override for one run with
# `TARGET_DEFAULT_BOARD=c6bench ./run.sh …`. It is a single name rather than a
# list because "may I brick this?" has exactly one safe answer per bench day, and
# a list is a thing that goes stale pointing at hardware nobody has seen in weeks.
TARGET_DEFAULT_BOARD=${TARGET_DEFAULT_BOARD:-c6spare}

# The bell's adapters, and the predicate over them, live in ONE place shared
# with zephyr/run.sh — the hand deploy path, which flashes the C6 over a
# DIFFERENT adapter than this harness does and was therefore never covered by
# the console hash that used to be hardcoded here (rete#185).
#
# What stays here is the POLICY: refused with NO override env, and that
# asymmetry is deliberate: a mislabelled workload is a plausible mistake and an
# ended multi-day soak is not recoverable. If the bell genuinely must be
# reflashed, that is a hand operation with zephyr/run.sh — not something a test
# suite has a code path for.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../tools" && pwd)/bench-identity.sh"

target::say()  { echo "  $*"; }
target::warn() { echo "  ! $*" >&2; }
target::die()  { echo "FAIL: $*" >&2; exit 1; }

# target::board_byid <board> -> the /dev/serial/by-id path for that instance.
#
# Resolved from the collector's own env file rather than from a table here, so
# there is ONE place on this bench that maps an instance to an adapter. A table
# here that disagreed with the collector's would send the flash to one board and
# read the console of the other, and every symptom of that reads as "the board
# went quiet".
target::board_byid() {
  local board=$1 port
  port=$(bench::port_for_board "$board") \
    || target::die "no usable collector env at $(bench::env_file "$board") — instance '$board' is not configured on this bench, or the file names no RETE_CONSOLE_PORT"
  echo "$port"
}

# target::assert_not_the_bell <board>
#
# The board-level half of D5's backstop, and it MUST run before anything else
# touches the board — earlier than the collector check, earlier than the lock.
#
# Learned by falsifying this file, 2026-08-05: with the collector check first, a
# deploy aimed at the bell was refused with "port=down — wrong by-id, or
# unplugged", which is both a guess and actively harmful. The bell's serial
# instance sits at port=down BECAUSE IT IS HEALTHY — wallwart power, no data
# cable, console over Thread — so that message invites someone to go and plug the
# bell into USB, which is the one action the interlock exists to prevent.
target::assert_not_the_bell() {
  local board=$1 port
  port=$(target::board_byid "$board")
  if bell::is_bell_port "$port"; then
    target::die "refusing to deploy to '$board': $port is the ship's bell. Flashing it ends a soak that cannot be restarted, and its port=down is healthy (wallwart, console over Thread). Deploy by hand with zephyr/run.sh if you really mean to."
  fi
}

# target::assert_deployable <board> <class>
#
# The two interlocks of D5, in the order that matters: the class check is the
# model, the by-id deny-list is the backstop. Both run; neither is skippable.
target::assert_deployable() {
  local board=$1 class=$2 port

  case "$class" in
    observe)
      : ;;                       # safe anywhere, including a live field node
    disruptive)
      # Disposable targets only. There is no consent mechanism for a field node
      # yet, so there is no third answer to invent here.
      [ "$board" = "$TARGET_DEFAULT_BOARD" ] \
        || target::die "scenario is class=disruptive and board '$board' is not the disposable one ($TARGET_DEFAULT_BOARD)"
      ;;
    *)
      target::die "unknown class '$class' — expected observe or disruptive" ;;
  esac

  # The backstop. Resolve the adapter this deploy would actually write to and
  # refuse the bell's, whatever the manifest claimed.
  port=$(target::board_byid "$board")
  if bell::is_bell_port "$port"; then
    target::die "refusing to deploy: $port is the ship's bell. Flashing it ends a soak that cannot be restarted. Do it by hand with zephyr/run.sh if you really mean to."
  fi

  [ -e "$port" ] || target::die "adapter $port is not present — is '$board' attached?"
}

# target::lock <board> — one runner per board. Two runs racing over one board is
# a failure mode that only appears once there is a suite, so it is closed before
# there is one. Uses its own lock dir, NOT the collector's .lock files.
target::lock() {
  local board=$1
  mkdir -p "$TARGET_LOCKDIR"
  exec 9>"$TARGET_LOCKDIR/$board.lock"
  flock -n 9 || target::die "another target run holds $board (see $TARGET_LOCKDIR/$board.lock)"
}

# target::logfile <board> — re-resolved on every poll, never cached: the
# collector rolls its log at midnight and a long run can outlive the path.
target::logfile() { BENCH_CONSOLE_STORE="$TARGET_STORE" bench::log_file "$1"; }

# target::collector_ok <board>
#
# Every check FAILS rather than SKIPS, and each names a VERIFIED cause. A skip
# whose message names an unverified cause is how a gate reports success for a
# run that never happened.
target::collector_ok() {
  local board=$1 logfile
  case "$(bench::collector_state "$board")" in
    active) : ;;
    unserved) target::die "custos ($RETE_CUSTOS) does not serve $board; this harness reads its log and never opens the port" ;;
    lent)     target::die "$board's cable is lent (POST /board/$board/lend) — another flash is in progress" ;;
    no-log)   target::die "no console log at $(target::logfile "$board")" ;;
    # port-down means custos is alive and seeing NOTHING, which would turn every
    # assertion below into a timeout with a misleading cause. Say it now, and say
    # only what has been VERIFIED.
    port-down) target::die "custos reports $board's cable down: it is serving and reading nothing, so every assertion here would time out with a misleading cause. (On the ship's bell this state is normal — its console goes over Thread, not this cable.)" ;;
  esac
  logfile=$(target::logfile "$board")
  [ -f "$logfile" ] || target::die "no console log at $logfile"
}

# ---- the board must be re-reset after a flash ----
#
# run.sh has custos lend the cable for the esptool call (rete#154), and esptool
# resets the board when it finishes — while the cable is still lent. So the boot
# banner, which IS the anchor, goes unlogged, and the board is reset AGAIN once
# custos is reading.

# target::reset_board <board> — pulse RTS (EN) to reboot, leaving DTR (IO0) high
# so it boots the application rather than the ROM downloader.
#
# The C6's console does NOT reset on open, so merely attaching cannot produce a
# banner — the reset has to be driven. Held open only for the pulse, and nothing
# is read through this handle, so the collector stays the single reader.
target::reset_board() {
  local port
  port=$(target::board_byid "$1")
  python3 - "$port" <<'PY'
import sys, time, serial
p = serial.Serial(sys.argv[1], 115200, timeout=0.1)
p.dtr = False          # IO0 high — boot the application
p.rts = True           # EN low  — hold in reset
time.sleep(0.15)
p.rts = False          # release
time.sleep(0.05)
p.close()
PY
}

# target::tap_verify <scenario>   [TAP lines on stdin]
#
# Prints "<verdict> <pass>/<plan> <detail>" and returns 0 only on PASS.
#
# A truncated run and a passing run MUST NOT look alike, which is the whole
# reason the workload declares a plan before emitting results. Checked here:
#
#   - exactly one plan line and exactly one done line
#   - pass + fail == plan                     (nothing vanished in aggregate)
#   - result indices are exactly 1..plan,     (and nothing vanished individually
#     each appearing once                      — an aggregate that happens to add
#                                              up cannot hide a swapped index)
#   - the ok/not-ok tallies match done's own  (the workload's arithmetic agrees
#     pass/fail counts                         with its results)
#   - fail == 0
target::tap_verify() {
  # The collector stamps every line with an ISO timestamp, so `TAP` is not field
  # 1 on the bench — and over porta it will not be field 1 either, for different
  # reasons. Normalise to the grammar first and let the parser below assume
  # nothing about what precedes it.
  sed -n 's/.*\(TAP .*\)/\1/p' |
  awk -v want="$1" '
    $1 != "TAP" || $2 != want { next }
    $3 ~ /^plan=/ { split($3, a, "="); plan = a[2]; plans++; next }
    $3 == "done" {
      for (i = 4; i <= NF; i++) {
        if ($i ~ /^pass=/) { split($i, p, "="); dpass = p[2] }
        if ($i ~ /^fail=/) { split($i, f, "="); dfail = f[2] }
      }
      dones++; next
    }
    $3 == "ok"                { seen[$4]++; oks++;    next }
    $3 == "not" && $4 == "ok" { seen[$5]++; notoks++; bad = bad " " $5; next }
    END {
      if (plans != 1) { print "FAIL 0/0 expected exactly one plan line, saw " plans+0; exit 1 }
      if (dones != 1) { print "FAIL " oks+0 "/" plan " no done line — the workload did not finish (saw " oks+0 " ok, " notoks+0 " not ok of " plan ")"; exit 1 }
      if (oks + notoks != plan) { print "FAIL " oks+0 "/" plan " plan promised " plan " results, " oks+notoks+0 " arrived"; exit 1 }
      if (dpass + dfail != plan) { print "FAIL " oks+0 "/" plan " done says pass=" dpass+0 " fail=" dfail+0 ", which is not the promised " plan; exit 1 }
      if (dpass != oks || dfail != notoks) { print "FAIL " oks+0 "/" plan " done says " dpass+0 "/" dfail+0 " but " oks+0 " ok and " notoks+0 " not ok arrived"; exit 1 }
      for (i = 1; i <= plan; i++) {
        if (seen[i] != 1) { print "FAIL " oks+0 "/" plan " result " i " appeared " seen[i]+0 " times, expected once"; exit 1 }
      }
      if (notoks > 0) { print "FAIL " oks+0 "/" plan " not ok:" bad; exit 1 }
      print "PASS " oks+0 "/" plan
    }
  '
}
