#!/usr/bin/env bash
# porta space-sync plan 2, the host interop gate: PortaBridgeNode.nsl (nsl/oevm)
# against a real `porta serve` (Go), over real UDP loopback. Two independently
# written implementations of one wire — which is the only kind of test that can
# catch a protocol MISREADING, and the reason it runs before hardware.
#
# What it witnesses (the host-side legs of the rung's gate):
#   1. `goal/name` written at porta appears in the node's space — proven by the
#      node echoing it back to `obs/name`, which porta then reads.
#   2. `obs/health` pushed by the node appears in porta's cell tree. An idle
#      check-in IS the heartbeat, so this is also the liveness proof.
#   3. `tel/log` entries arrive EXACTLY ONCE: every entry carries a monotone tick
#      counter, so a re-pushed entry would show up as a repeated 'tick N'.
#   4. a `dbg/req` written at porta opens a per-method profile window on the node
#      and the window CLOSES ITSELF (#165 D2/D6) — the only off-target proof that
#      the request survives the wire, since the node-side chain alone is covered
#      by nodus/tests/23_mprof/04_dbg_req_chain.nsl.
# Leg 3 under a DELIBERATE datagram drop is plan 3's, on the C6 over the mesh.
#
# Not wired into run_tests_oevm.py — it builds a separate Go binary and a
# second VM image, heavier than the rest of that suite, so it runs standalone.
# porta lives in-repo now (rete consolidation, Task 4) at porta/, so a missing
# PORTA_DIR here is a broken checkout, not an absent sibling repo — this gate
# FAILS rather than skipping it. A missing `go` toolchain is still a genuine
# environment SKIP (below).
set -euo pipefail
cd "$(dirname "$0")/.."

PORTA_DIR="${PORTA_DIR:-$(git rev-parse --show-toplevel)/porta}"
NSL="$(git rev-parse --show-toplevel)/nodus/nsl/tools/ns_compile_oevm.py"
TARGET="${OEVM_TARGET:-posix}"
DB=/tmp/porta_sync_gate.db
# rete#241: a per-run id, handed to the node through OEVM_NODE_ID (the same
# explicit-override door zephyr/run.sh uses on boards). The node answers
# `platform nodeId`, so nothing here is a slot two concurrent runs would share
# — the gate knows the id in advance because it chose it, not because the node
# hardcodes it.
NODE="bench-node-$$"

if [ ! -d "$PORTA_DIR" ]; then
  echo "FAIL: no porta at $PORTA_DIR"; exit 1
fi
if ! command -v go >/dev/null 2>&1; then
  echo "SKIP: go is not on PATH — cannot build the porta counterparty"; exit 0
fi

echo "[1/8] build porta"
( cd "$PORTA_DIR" && go build -o /tmp/porta ./cmd/porta )

echo "[2/8] host-ABI layout descriptor"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json

echo "[3/8] compile PortaBridgeNode.nsl -> .oimg"
python3 "$NSL" zephyr/PortaBridgeNode.nsl /tmp/porta_node.oimg --format=oimg --layout=/tmp/layout.json

echo "[4/8] build the image VM"
# -DOEVM_METHOD_PROFILE: leg 8 below drives a real dbg/req profile window end
# to end. Without the define `profileStart`/`profileStop` are silent no-ops
# (platform.c:1217), so the leg would pass on a node that never counted — the
# gate would be green with the whole feature compiled out. The counter costs
# ~0.9% of activations, which no leg above is timed tightly enough to notice.
gcc -Wall -std=c99 -DOEVM_METHOD_PROFILE -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    src/span.c \
    "src/backend/$TARGET/platform_$TARGET.c" src/serial.c \
    "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" \
    src/http.c src/http_core.c "src/backend/$TARGET/http_$TARGET.c" \
    src/ota.c "src/backend/$TARGET/ota_$TARGET.c" \
    "src/backend/$TARGET/pulse_$TARGET.c" "src/backend/$TARGET/timer_$TARGET.c" \
    "src/backend/$TARGET/span_$TARGET.c" \
    "src/backend/$TARGET/sensor_$TARGET.c" \
    src/mirror.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_porta -lm

echo "[5/8] start porta serve on udp 127.0.0.1:16969"
rm -f "$DB"
# --manifest arms the rete#243 kind+schema gate for the WHOLE run: every path
# the real node pushes below must clear the closed manifest, so a green run is
# also the completeness proof for protocol/MANIFEST.cddl's row table.
MANIFEST="$(git rev-parse --show-toplevel)/protocol/MANIFEST.cddl"
/tmp/porta serve --db "$DB" --udp 127.0.0.1:16969 --http-port 0 --sweep-tick 1s --manifest "$MANIFEST" >/tmp/porta_serve.log 2>&1 &
PORTA_PID=$!
NODE_PID=""
trap 'kill $PORTA_PID ${NODE_PID:-} 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do grep -q "serving the space" /tmp/porta_serve.log && break; sleep 0.1; done
grep -q "serving the space" /tmp/porta_serve.log || {
  echo "FAIL: porta did not start"; cat /tmp/porta_serve.log; exit 1; }

# The node's cadence runs on real time; the suite's default virtual clock never
# advances on its own, so the timer would never fire (the same knob the http gate
# needs, for the same reason).
#
# OEVM_BOOTID_FILE gives this node a REAL per-boot counter (rete#162). Without it
# the host backend answers the 0 sentinel — "no persistent boot identity" — and
# this gate, the only one that runs both halves against each other, would exercise
# only the degraded path while every real node on a board runs the other one. The
# file is SEEDED rather than removed, and the value matters: starting from a
# removed file the node latches boot 1, which is exactly the constant rete#211's
# publishHealth used to hardcode — so a gate run at boot 1 could not tell a real
# counter from the literal, and would have stayed green if anyone put it back.
# Seeding 40 makes the node latch 41, a value nothing hardcodes.
echo "[6/8] start the node"
echo 40 > /tmp/porta_node_bootid
NODE_BOOT=41
OEVM_TIMER_WALLCLOCK=1 OEVM_BOOTID_FILE=/tmp/porta_node_bootid OEVM_NODE_ID="$NODE" \
  /tmp/oevm_porta /tmp/porta_node.oimg >/tmp/porta_node.log 2>&1 &
NODE_PID=$!
for _ in $(seq 1 50); do grep -q "PORTA NODE READY" /tmp/porta_node.log && break; sleep 0.1; done
grep -q "PORTA NODE READY" /tmp/porta_node.log || {
  echo "FAIL: node did not start"; cat /tmp/porta_node.log; exit 1; }

echo "[7/8] drive it"
# Every piped grep below is `grep X >/dev/null`, never `grep -q`: this script
# runs under pipefail, and -q exits at the first match — the still-writing
# `porta get` then dies of SIGPIPE and pipefail scores a SUCCESSFUL match as a
# failure. Exposure scales with how early the match sits in how large a dump,
# so it strikes the healthiest-looking checks first (rete#260; the discovery
# story is on the diag/spans leg below). Plain `grep -q pattern file` is fine —
# no pipe, no writer to kill.
# Leg 2 first: it needs no operator write at all.
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" obs 2>/dev/null | grep "obs/health" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" obs | grep "obs/health" >/dev/null || {
  echo "FAIL: obs/health never reached porta"; /tmp/porta get --db "$DB" "$NODE"; cat /tmp/porta_serve.log; exit 1; }
echo "  obs/health is in porta's cell tree"

# rete#211: obs/health's own bid field must be the REAL boot id, not a literal.
# It is the only place the boot id reaches a path — the envelope's copy is
# transport and never lands — so a constant here made the space's answer to
# "which boot is this node on?" wrong on every node, for every boot, while
# PROTOCOL-NSL-RECORDS §4.4 claimed rete#162 had fixed it. Both halves are
# checked: the RECORD field, and porta's own per-row boot (rete#210) that the
# ENVELOPE carried. Asserting they agree is what makes a future divergence a
# failure rather than a thing someone notices on a bench two months later.
/tmp/porta get --db "$DB" "$NODE" obs/health | grep "#bid: $NODE_BOOT" >/dev/null || {
  echo "FAIL: obs/health reports the wrong boot id — want #bid: $NODE_BOOT"
  /tmp/porta get --db "$DB" "$NODE" obs/health; exit 1; }
/tmp/porta get --db "$DB" "$NODE" obs/health | grep "boot=$NODE_BOOT" >/dev/null || {
  echo "FAIL: the push envelope carried a different boot than the record"
  /tmp/porta get --db "$DB" "$NODE" obs/health; exit 1; }
echo "  obs/health's bid is the real boot ($NODE_BOOT), and the record agrees with the envelope"
/tmp/porta get --db "$DB" "$NODE" obs | grep "obs/build" >/dev/null || { echo "FAIL: obs/build missing"; exit 1; }
echo "  obs/build carries the node's provenance"

# rete#231: the node's own declaration of how long its silence may last. This is
# the leg that proves the record survives the wire — the node builds it with its
# Cbor class and porta decodes it with a hand-rolled codec that was written from
# the record catalogue, so a shape either side got wrong shows up HERE and not on
# a board six weeks later.
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" obs 2>/dev/null | grep "obs/liveness" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" obs/liveness | grep "#cad: 2000" >/dev/null || {
  echo "FAIL: obs/liveness does not carry the node's declared 2 s cadence"
  /tmp/porta get --db "$DB" "$NODE" obs/liveness; exit 1; }
/tmp/porta get --db "$DB" "$NODE" obs/liveness | grep "#bud: 16000" >/dev/null || {
  echo "FAIL: obs/liveness does not carry the declared 16 s budget (8 x cadence)"
  /tmp/porta get --db "$DB" "$NODE" obs/liveness; exit 1; }
echo "  obs/liveness carries the node's declared cadence and budget"

# rete#231: porta's verdict, formed from the number the node declared. The sweep's
# default period (space.SweepTick, 30 s) outlasts this whole gate, so serve is
# started with --sweep-tick 1s above rather than the script sleeping 31 s for a
# tick it would also be racing.
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" gw 2>/dev/null | grep "gw/liveness" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" gw/liveness | grep "#live" >/dev/null || {
  echo "FAIL: porta did not judge a node checking in every 2 s as #live"
  /tmp/porta get --db "$DB" "$NODE" gw; cat /tmp/porta_serve.log; exit 1; }
echo "  gw/liveness reads #live, formed from the declared 16 s budget"

# sys/metrics — #56 capability (3). RECORDS §4.4 has pinned this as a CELL since
# 2026-07-21, and it was UNSENDABLE until the cell scan reached sys/ (design D1):
# collectOutbound only ever scanned obs/**, so the value sat in the node's own space
# forever and no app ever bothered to publish it. Assert the path is PRESENT — an
# assertion that merely checks nothing errored would pass with the feature removed.
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" sys 2>/dev/null | grep "sys/metrics" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" sys | grep "sys/metrics" >/dev/null || {
  echo "FAIL: sys/metrics never reached porta — the cell scan is not covering sys/"
  /tmp/porta get --db "$DB" "$NODE"; cat /tmp/porta_serve.log; exit 1; }
echo "  sys/metrics is in porta's cell tree"

# sys/cheap — rete#190. Same hazard as sys/metrics just above: a cell can sit in the
# node's own space forever without anyone noticing it never left, which is exactly
# how sys/metrics stayed unpublished for a rung. This matters more than it looks for
# rete#190's next task, the hardware leak measurement: if this gauge silently failed
# to arrive, "the gauge shows nothing" would be indistinguishable from "the leak is
# outside the VM's allocation sites", and the wrong conclusion would be drawn.
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" sys 2>/dev/null | grep "sys/cheap" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" sys | grep "sys/cheap" >/dev/null || {
  echo "FAIL: sys/cheap never reached porta — the cell scan is not covering sys/"
  /tmp/porta get --db "$DB" "$NODE"; cat /tmp/porta_serve.log; exit 1; }
echo "  sys/cheap is in porta's cell tree"

# Leg 1: write the goal, wait for the node to echo it as obs.
# The gate's negative control at the seam (rete#243): a mis-kinded operator
# write must be refused BY NAME. A validation gate that has never been seen
# red is the false-green shape this bench keeps re-learning.
if /tmp/porta put --db "$DB" "$NODE" goal/name bench --kind stream 2>/tmp/porta_put_refused.log; then
  echo "FAIL: mis-kinded goal/name was accepted — the manifest gate is not armed"; exit 1
fi
grep -q "manifest:kind" /tmp/porta_put_refused.log || {
  echo "FAIL: mis-kinded put refused, but not by the manifest gate:"; cat /tmp/porta_put_refused.log; exit 1; }
echo "      manifest gate refusal witnessed: $(grep -o 'manifest:kind[^"]*' /tmp/porta_put_refused.log | head -1)"

/tmp/porta put --db "$DB" "$NODE" goal/name bench >/dev/null
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" obs/name 2>/dev/null | grep "bench" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" obs/name | grep "bench" >/dev/null || {
  echo "FAIL: goal/name never converged to obs/name"; /tmp/porta get --db "$DB" "$NODE"; exit 1; }
echo "  goal/name converged to obs/name — the diff IS the view"

# rete#231 D2, and this is the leg that proves the operator half. 45 s is not a
# whole number of the node's 2 s cadence, so the node must publish 46 000 and the
# two paths must DISAGREE — that disagreement is the answer, not a failure, and a
# gate that asserted agreement here would be asserting that the node's grid does
# not exist.
#
# `porta put` takes scalars or --raw hex, and a Record needs the hex. This
# constant is Liveness { cad: 2000, bud: 45000 } and it is pinned by
# TestGoalLivenessGoldenForTheHostGate in porta/internal/cval — a hex nobody
# checks is a constant that rots silently, and it would rot into a record the
# node logs as bad while this gate went on passing.
GOAL_LIVENESS_HEX=d81b82d827684c6976656e657373a2d8276362756419afc8d827636361641907d0
# --raw is a BOOLEAN flag: the hex is the positional VALUE, not the flag's argument.
/tmp/porta put --db "$DB" --raw "$NODE" goal/liveness "$GOAL_LIVENESS_HEX" >/dev/null
for _ in $(seq 1 100); do
  /tmp/porta get --db "$DB" "$NODE" obs/liveness 2>/dev/null | grep "46000" >/dev/null && break; sleep 0.2
done
/tmp/porta get --db "$DB" "$NODE" obs/liveness | grep "46000" >/dev/null || {
  echo "FAIL: the node did not snap a 45 s request onto its 2 s grid"
  /tmp/porta get --db "$DB" "$NODE" obs/liveness; exit 1; }
/tmp/porta get --db "$DB" "$NODE" goal/liveness | grep "45000" >/dev/null || {
  echo "FAIL: goal/liveness no longer holds what the operator asked for — the node overwrote a goal/ path"
  /tmp/porta get --db "$DB" "$NODE" goal/liveness; exit 1; }
echo "  goal/liveness 45 s -> obs/liveness 46 s: the node snapped, and the diff IS the answer"

# Leg 3: exactly once, which is THREE claims and not one.
#   no duplicates  -> rows == distinct ticks
#   no gaps        -> the distinct ticks run contiguously
#   pinned start   -> the series starts at 1
# Loopback loses nothing, so only the first can fail here in the ordinary run;
# the other two exist so this gate and the C6 gate (plan 3) score the same
# property. The pinned-start check exists because contiguity alone does not
# pin where the series starts: `porta log --limit N` truncates from the
# OLDEST end (Store.Events is ORDER BY id DESC LIMIT ?), so a truncated log
# would leave a contiguous TAIL and pass anyway; and the bridge's own
# maxPending eviction is oldest-first, so a long pre-connection outage
# produces the identical shape on hardware. Parse with `grep -o` rather than
# `sed 's/.*tick //'` — the record's formatting puts punctuation after the
# number, which sort -u tolerates and arithmetic does not.
sleep 3
TICKS=$(/tmp/porta log --db "$DB" "$NODE" --limit 200 | grep -o 'tick [0-9]\+' | awk '{print $2}' || true)
LOGS=$(printf '%s\n' "$TICKS" | grep -c . || true)
UNIQ=$(printf '%s\n' "$TICKS" | sort -n -u | grep -c . || true)
echo "  tel/log rows=$LOGS distinct ticks=$UNIQ"
test "$LOGS" -ge 2 || { echo "FAIL: fewer than two tel/log entries — the stream never rode"; exit 1; }
test "$LOGS" = "$UNIQ" || {
  echo "FAIL: a tel/log entry was delivered more than once"
  /tmp/porta log --db "$DB" "$NODE" --limit 200; exit 1; }

FIRST=$(printf '%s\n' "$TICKS" | sort -n -u | head -1)
LAST=$(printf '%s\n' "$TICKS" | sort -n -u | tail -1)
test $((LAST - FIRST + 1)) -eq "$UNIQ" || {
  echo "FAIL: a gap in the tick series ($FIRST..$LAST holds only $UNIQ entries) — an entry was lost"
  /tmp/porta log --db "$DB" "$NODE" --limit 200; exit 1; }
test "$FIRST" = 1 || {
  echo "FAIL: the series starts at $FIRST, not 1 — either --limit truncated the OLDEST rows (raise it) or the bridge's maxPending eviction dropped the head of the series under a long outage"
  /tmp/porta log --db "$DB" "$NODE" --limit 200; exit 1; }
echo "  tel/log arrived exactly once per entry, with no gaps ($FIRST..$LAST)"

# THE RUN-UP. #56 design §4 claims porta retains metrics history for free, because
# store.go appends an event for every accepted entry with no value dedup — so repeated
# pushes of one cell ACCUMULATE rather than collapsing to latest-value. That claim was
# read out of the source and never once run. It is the load-bearing simplification of
# the whole design: if it is false, the run-up needs a mechanism after all.
# An empty or non-integer count makes `test` error, which fails — the safe direction.
#
# WAITS FOR THE SECOND ROW rather than counting after a fixed sleep, and the
# assertion is unchanged in strength: still >= 2, still a hard FAIL on timeout,
# never a skip. What changed is that the window is now stated instead of assumed.
# Found by tuvm#161, which added three span cells and turned this red: the capture
# publishes one diag/spans/<slot> cell PER SLOT in one burst, and that burst is a
# backlog bigger than the 512 B frame, so the bridge trims — from the TAIL, which
# is the HIGHEST seq, which is sys/metrics, written immediately before the push
# (tuvm#150). Until the burst drains, the newest cell is the one that never rides.
# Measured on this gate's own setup: with 14 span slots the drain takes ~8 ticks
# (~16 s) and metrics accumulates one per tick thereafter; the old fixed `sleep 3`
# sampled at ~7 ticks, so it was passing on the drain finishing first — an
# assumption that got shorter every time a node registered another span.
for _ in $(seq 1 150); do
  METRICS_ROWS=$(/tmp/porta log --db "$DB" "$NODE" --limit 500 | grep -c 'sys/metrics' || true)
  test "$METRICS_ROWS" -ge 2 && break; sleep 0.4
done
test "$METRICS_ROWS" -ge 2 || {
  echo "FAIL: sys/metrics appears $METRICS_ROWS time(s) in the event log after 60s, expected >= 2."
  echo "      Either a cell push is being collapsed to latest-value instead of APPENDED,"
  echo "      or the push backlog never drained and metrics was trimmed every tick (tuvm#150)."
  /tmp/porta log --db "$DB" "$NODE" --limit 500; exit 1; }
echo "  sys/metrics accumulates in the event log: $METRICS_ROWS rows — the run-up is retained"

# tuvm#107 + the diag/** outbound grant: a span capture has to survive the whole chain —
# split into sibling cells small enough for the 256 B cap, picked up by the cell scan,
# carried by the push, stored by porta. Any one of those missing and the capture is
# invisible, which is exactly how sys/metrics stayed unpublished for a rung.
#
# The node closes its one window on the first tick where the gateway has acked, so this
# waits rather than assuming a tick count.
# This leg is where the pipefail/SIGPIPE class (see [7/8] above) was found:
# diag/spans/checkIn matches on row 2 of a 16-row dump, maximum exposure, and
# rete#243's slightly slower store Open shifted the scheduling enough to trip
# it three runs out of three on 2026-08-25. rete#260 swept the rest.
for _ in $(seq 1 60); do
  /tmp/porta get --db "$DB" "$NODE" diag 2>/dev/null | grep "diag/spans/result" >/dev/null && break; sleep 0.4
done
/tmp/porta get --db "$DB" "$NODE" diag | grep "diag/spans/result" >/dev/null || {
  echo "FAIL: diag/spans/result never reached porta (tuvm#107 / diag outbound grant)"
  /tmp/porta get --db "$DB" "$NODE"; cat /tmp/porta_serve.log; exit 1; }

# The summary alone would pass while every row was silently dropped at the value cap —
# which is the failure this whole change exists to remove. Assert a ROW, by name.
for _ in $(seq 1 60); do
  /tmp/porta get --db "$DB" "$NODE" diag 2>/dev/null | grep "diag/spans/checkIn" >/dev/null && break; sleep 0.4
done
/tmp/porta get --db "$DB" "$NODE" diag | grep "diag/spans/checkIn" >/dev/null || {
  echo "FAIL: no per-slot span cell reached porta — only the summary did"
  /tmp/porta get --db "$DB" "$NODE" diag; exit 1; }
SPAN_ROWS=$(/tmp/porta get --db "$DB" "$NODE" diag | grep -c "diag/spans/")
echo "  diag/spans: $SPAN_ROWS cells in porta's tree, rows and summary (tuvm#107)"

# LEG 8 — the #165 profile chain, end to end over the real wire: porta writes
# dbg/req, the node's inbound filter accepts it (kernel `mayApply:`), the bridge
# applies it as a STREAM, the channel reaction self-sends `dbgReq:`, the parsed
# duration arms a one-shot on the app's ONE Timers instance, and the fired timer
# posts to sys/profileEnd, which closes the window and dumps.
#
# Every one of those hops was proven ONLY by the C6 hardware gate until now:
# nodus/tests/23_mprof/04_dbg_req_chain.nsl drives the node-side half on the
# host virtual clock, but nothing off-target had ever carried a dbg/req across
# the wire. This leg is the other half, and it is why the wiring was ported to
# PortaBridgeNode.nsl rather than left C6-only.
#
# `mprof[window]` is asserted by name, not `mprof`: mprof_report also prints
# `mprof[exit]` (vm.c's destructor, gated on OEVM_MPROF_DUMP) and `mprof[flip]`
# / `mprof[dump]` under the old countdown seam, and a grep for the bare prefix
# would score any of them as a window that closed.
echo "[8/8] dbg/req opens and closes a profile window"
/tmp/porta put --db "$DB" "$NODE" dbg/req "profile 3000" --kind stream >/dev/null
for _ in $(seq 1 100); do
  grep -q 'mprof\[window\]' /tmp/porta_node.log && break; sleep 0.4
done
grep -q 'mprof\[window\]' /tmp/porta_node.log || {
  echo "FAIL: dbg/req 'profile 3000' never closed a window — no mprof[window] in the node log."
  echo "      The chain is porta put -> mayApply: -> the dbg/req channel reaction ->"
  echo "      parseProfileMs: -> timers after: -> sys/profileEnd -> profileStop."
  tail -40 /tmp/porta_node.log; exit 1; }
echo "  the window closed itself and dumped — porta never sent a stop"

echo "PORTA SYNC OK"
