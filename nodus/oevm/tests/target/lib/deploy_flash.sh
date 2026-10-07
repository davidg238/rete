# nodus/oevm/tests/target/lib/deploy_flash.sh
#
# The BENCH implementation of D2's deploy seam: get a workload onto a disposable
# board by building and flashing it.
#
# The field implementation (deploy::push — an .oimg or job push over porta, under
# a capability rooted at the diag subtree) is not built here. It plugs in at this
# same signature, which is the point of having the seam at all.
#
#   deploy::flash   -> prints the build id, and NOTHING else, on stdout
#
# Everything diagnostic goes to stderr, because the caller captures stdout.

# THE WEST ENV IS THIS FUNCTION'S JOB. zephyr/run.sh owns every step that has a
# wrong-by-default EXCEPT this one — it does not source the venv (BOARD_PORTS.md
# §"a missing env"), and a missing env fails as `west: command not found` buried
# in a build log. Every caller has therefore carried its own copy; this is the
# copy that survives, because it is the one the harness runs. Without it "one
# command" is true only from a shell somebody remembered to activate first.
TARGET_WEST_VENV=${TARGET_WEST_VENV:-$HOME/zephyr-esp32/.venv}
TARGET_ZEPHYR_BASE=${TARGET_ZEPHYR_BASE:-$HOME/zephyr-esp32/zephyr}
TARGET_ZEPHYR_SDK=${TARGET_ZEPHYR_SDK:-$HOME/zephyr-sdk-0.17.4}

# shellcheck disable=SC2154   # NODE/BOARD_ID/CONF_EXTRA come from the manifest

deploy::flash() {
  local zdir=$1 board=$2 out port

  # Named BEFORE the collector is paused, so a bench missing its toolchain says
  # so instead of stopping the console reader and then failing to build.
  [ -f "$TARGET_WEST_VENV/bin/activate" ] \
    || target::die "no west venv at $TARGET_WEST_VENV — zephyr/run.sh needs it and does not source it"
  [ -d "$TARGET_ZEPHYR_SDK" ] \
    || target::die "no Zephyr SDK at $TARGET_ZEPHYR_SDK"

  # ESPTOOL_PORT always wins in run.sh and skips its own detection. Setting it
  # from the adapter we already resolved and vetted is what makes D5's deny-list
  # protective rather than advisory: without it, run.sh detects a port itself and
  # could pick the other board entirely.
  #
  # CONSOLE_PORT GETS THE SAME TREATMENT, and for the same reason (2026-08-09).
  # Pinning only the flash port left run.sh detecting the CONSOLE port itself,
  # which is the identical hazard one line up — and it went unnoticed for as long
  # as exactly one CP2102N was ever attached. The moment a second board joined the
  # bench, run.sh refused outright:
  #
  #   more than one CP2102N console candidate: … Set CONSOLE_PORT explicitly and retry.
  #
  # Failing closed is the right behaviour and it is still a bench that cannot run
  # its own gates with two boards plugged in. Both ports now come from the single
  # resolved adapter, so neither is ever detected and the deny-list covers both.
  port=$(target::board_byid "$board")
  export ESPTOOL_PORT="$port" CONSOLE_PORT="$port"

  local args=(--board "${BOARD_ID:-esp32c6}" --node "$NODE" --flash)
  [ -n "${CONF_EXTRA:-}" ] && args+=(--conf-extra "$CONF_EXTRA")

  # rete#215 — A WARM BUILD DIR CARRIES THE PREVIOUS SCENARIO'S KCONFIG, and it
  # produces a gate that passes while testing the wrong thing.
  #
  # Caught 2026-08-19. `wedge-note` builds with prj_esp32c6_wedgegate.conf; `wedge-detect`
  # declares NO CONF_EXTRA and must run the default build, because the whole difference
  # between those two scenarios is that one wedges synthetically and the other wedges for
  # real. Run back to back they share build-esp32c6-B, and CMake keeps
  # `EXTRA_CONF_FILE:UNINITIALIZED=…` in its cache when the variable is not re-specified —
  # so wedge-detect ran with CONFIG_OEVM_WEDGE_TRIGGER=y, rebooted at the synthetic 15 s
  # instead of its own 30 s wedge, and reported PASS 2/2. It tested wedge-note twice. The
  # console said so (`SYNTHETIC WEDGE TRIGGER is compiled in` in a build that asks for no
  # overlay) and the verdict could not.
  #
  # THE CMAKE CACHE IS ASKED DIRECTLY rather than a stamp file being kept beside it: the
  # cache is what actually decides the build, a stamp is a claim about it, and --pristine
  # deletes the build directory — so a stamp living inside it would vanish on every
  # pristine build and force the next one to be pristine too, forever.
  #
  # Two conservative rules, because run.sh APPENDS overlays of its own (the gitignored
  # thread dataset) and an equality test would therefore fire on every radio build:
  #   - manifest wants none, cache has some  -> pristine. This is the direction that bit.
  #   - manifest wants some, cache is missing any of them -> pristine.
  # Anything else builds incrementally, so the cost falls on the run that would have lied.
  local cache="$zdir/build-${BOARD_ID:-esp32c6}-B/CMakeCache.txt"
  local cached="" need_pristine=0
  [ -f "$cache" ] && cached=$(sed -n 's/^EXTRA_CONF_FILE:[A-Z]*=//p' "$cache" | head -1)
  if [ -z "${CONF_EXTRA:-}" ]; then
    [ -n "$cached" ] && need_pristine=1
  else
    local one
    IFS=';' read -ra _want <<< "$CONF_EXTRA"
    for one in "${_want[@]}"; do
      case ";$cached;" in *";$one;"*) ;; *) need_pristine=1 ;; esac
    done
  fi
  if [ "$need_pristine" = 1 ]; then
    # target::warn, NOT target::say: this function returns the build id on STDOUT and
    # the runner captures it. A say() here lands inside the anchor string, and an anchor
    # that is really a sentence matches no log line — which reads exactly like a board
    # that never booted. Cost: one confusing red run, 2026-08-19.
    target::warn "build dir was configured for '${cached:-<none>}', this scenario wants '${CONF_EXTRA:-<none>}' — forcing --pristine"
    args+=(--pristine)
  fi

  # `set +u` for the activate script, which reads unset variables; scoped to the
  # subshell so the runner keeps its own -u.
  #
  # rete#236 — THE BUILD'S STDERR IS NOT CAPTURED, and that is the whole fix.
  #
  # This used to end `2>&1`, folding the build's diagnostics into `$out` — which is
  # echoed ONLY on failure and otherwise read for one line, the `[archive]` build id.
  # A build that succeeded therefore discarded every warning it had emitted.
  #
  # `zephyr/run.sh` prints "[thread] radio layer not requested — dataset overlay NOT
  # applied" for a C6 built with no OpenThread, and its own rete#165 comment records
  # what that costs: "shipped an image with no radio: PortaBridgeNodeC6 failed its UDP
  # bind at boot and sat dead for over an hour". On 2026-08-22 it fired for
  # liveness-heartbeat, was swallowed here because the build exited 0, and the node sat
  # silent for 300 s while this harness reported only `TIMEOUT … never reached its done
  # line` — true, and naming nothing. One board cycle to rediscover what had already
  # been printed.
  #
  # NOT A GREP FOR KNOWN WARNINGS, deliberately. A pattern list is a thing that goes
  # stale pointing at the warnings someone thought of, and the next one to matter is by
  # definition the one nobody wrote a pattern for. Letting stderr through costs no list
  # and covers warnings that do not exist yet.
  #
  # It is also just this file's OWN stated contract, applied to the process it runs:
  # "prints the build id, and NOTHING else, on stdout / everything diagnostic goes to
  # stderr". run.sh already obeys it — `[archive] <id> -> <path>` is stdout
  # (nodus/oevm/zephyr/archive.sh:48), the `[thread] !` lines are stderr. `2>&1` was the one
  # place the harness broke its own rule, and removing it is a mechanism deleted rather
  # than one added.
  out=$( set +u
         # shellcheck disable=SC1091
         source "$TARGET_WEST_VENV/bin/activate"
         export ZEPHYR_BASE="$TARGET_ZEPHYR_BASE" \
                ZEPHYR_SDK_INSTALL_DIR="$TARGET_ZEPHYR_SDK" \
                ZEPHYR_TOOLCHAIN_VARIANT=zephyr
         bash "$zdir/run.sh" "${args[@]}" ) || {
    # The build's own error already streamed to stderr above; this adds the stdout tail,
    # which carries the progress lines that say how far it got.
    echo "$out" | tail -20 >&2
    target::die "run.sh failed for $NODE"
  }
  # esptool already reset the board, but it did so while run.sh had the cable lent
  # (rete#154), so that boot — and the banner this run is anchored on — went
  # nowhere. Reset again now that custos is reading.
  target::reset_board "$board"

  # run.sh prints: [archive] <build-id> -> <path>
  local build_id
  build_id=$(echo "$out" | sed -n 's/^\[archive\] \([^ ]*\) ->.*/\1/p' | tail -1)
  [ -n "$build_id" ] || {
    echo "$out" | tail -20 >&2
    target::die "no build id in run.sh output — every assertion downstream is anchored on it, so there is nothing to run"
  }
  echo "$build_id"
}
