# nodus/tuvm/tests/target/lib/deploy_flash.sh
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

# shellcheck disable=SC2154   # NODE/BOARD_ID/CONF_EXTRA come from the manifest

deploy::flash() {
  local zdir=$1 board=$2 out port

  # ESPTOOL_PORT always wins in run.sh and skips its own detection. Setting it
  # from the adapter we already resolved and vetted is what makes D5's deny-list
  # protective rather than advisory: without it, run.sh detects a port itself and
  # could pick the other board entirely.
  port=$(target::board_byid "$board")
  export ESPTOOL_PORT="$port"

  local args=(--board "${BOARD_ID:-esp32c6}" --node "$NODE" --flash)
  [ -n "${CONF_EXTRA:-}" ] && args+=(--conf-extra "$CONF_EXTRA")

  # The collector must let go of the tty for the duration, or it and esptool
  # split the ROM handshake between them. See target::collector_pause.
  target::collector_pause "$board"
  out=$(bash "$zdir/run.sh" "${args[@]}" 2>&1) || {
    target::collector_resume
    echo "$out" | tail -20 >&2
    target::die "run.sh failed for $NODE"
  }
  target::collector_resume

  # esptool already reset the board, but it did so while the collector was down,
  # so that boot — and the banner this run is anchored on — went nowhere. Reset
  # again now that there is a reader.
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
