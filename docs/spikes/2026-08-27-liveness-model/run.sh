#!/usr/bin/env bash
# rete — model-check the liveness contract.
#
# EVERY CONFIG NAMES THE ANSWER IT EXPECTS, and the run compares. A config that
# stops matching its expectation is the finding — in EITHER direction. Before
# rete#272 this file said "all three are expected to violate"; that was true of
# the three configs that existed and became false the moment a rule that holds
# was found, which is the shape of stale record this program keeps hitting.
#
# The RULE constant picks which verdict porta applies (see Liveness.tla):
#   arrival      now - last_seen <= bud            — what porta ships today
#   sweepDelta   ... AND wak moved between sweeps  — rete#272, read as a delta
#   progressAge  now - last_PROGRESS <= bud        — rete#272, read as an age
#   rateAge      now - last_seen <= bud, AND wak at the declared rate, else
#                "stalled"                          — rete#272 D3 (2026-09-14)
#
# TLC is not vendored. Fetch it once:
#   curl -sSLo /tmp/tla2tools.jar \
#     https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar
set -u
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
cd "$(dirname "$0")" || exit 2
[ -f "$JAR" ] || { echo "no tla2tools.jar at $JAR (set TLA_JAR)"; exit 2; }

# config : expectation : what the run is asking
CASES=(
  "bench:violated:arrival, the bench node — H3, and the evidence rete#272 was filed on"
  "slow:violated:arrival, the compiled-in default's shape — the same hole, wider"
  "h1:violated:the delivered period against the declared one (rete#271)"
  "bench-delta:violated:rete#272 read as a between-sweeps DELTA — insufficient, and the trace says why"
  "slow-delta:violated:the same delta rule on the wide-budget node"
  "bench-age:violated:rete#272 read as an AGE — right signal, and the residue is rete#273's arithmetic"
  "slow-age:violated:the same age rule, budget far past the deadline"
  "sound:completed:the age rule with Bud + Sweep <= Cad * Slack — THE ONE THAT HOLDS"
  "sound-vacuity:violated:proof that 'live' is reachable in sound.cfg, so its green is not vacuous"
  "sound-age-behind:violated:the age rule calls a deaf node live — what rete#272 D3's stalled is for"
  "sound-rate:completed:the RATE rule (D3) — no stale live, no behind live, no false stall"
  "sound-rate-live:violated:proof that 'live' is reachable under the rate rule"
  "sound-rate-stalled:violated:proof that 'stalled' fires — reachability before health"
)
rc=0
for entry in "${CASES[@]}"; do
  c=${entry%%:*}; rest=${entry#*:}; want=${rest%%:*}; why=${rest#*:}
  printf '%-14s ' "$c"
  java -cp "$JAR" tlc2.TLC -config "$c.cfg" Liveness.tla > "/tmp/tlc_$c.txt" 2>&1
  got=violated
  grep -q "Model checking completed" "/tmp/tlc_$c.txt" && got=completed
  grep -q "Invariant [A-Za-z]* is violated" "/tmp/tlc_$c.txt" || \
    { [ "$got" = completed ] || got=ERROR; }
  if [ "$got" = "$want" ]; then printf 'ok   %-9s  %s\n' "$got" "$why"
  else printf 'MISMATCH want=%s got=%s  (trace: /tmp/tlc_%s.txt)\n' "$want" "$got" "$c"; rc=1; fi
done
exit $rc
