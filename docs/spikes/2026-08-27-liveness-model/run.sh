#!/usr/bin/env bash
# rete — model-check the liveness contract. Three configs, three questions.
#
#   bench  the node as it flies (cad 2000, bud 16000) — H3: does porta ever
#          report #live while the node is provably stuck by its OWN deadline?
#   slow   a node whose declared budget far exceeds its reboot-loop gap, which
#          is what the compiled-in default (cad 30000 / bud 240000) produces.
#   h1     the delivered period against the declared one, on its own.
#
# ALL THREE ARE EXPECTED TO REPORT A VIOLATION. That is the point: each one is
# a hole, and the trace is the evidence. A run that goes green here means
# something upstream was fixed — read the trace before believing it.
#
# TLC is not vendored. Fetch it once:
#   curl -sSLo /tmp/tla2tools.jar \
#     https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar
set -u
JAR=${TLA_JAR:-/tmp/tla2tools.jar}
cd "$(dirname "$0")" || exit 2
[ -f "$JAR" ] || { echo "no tla2tools.jar at $JAR (set TLA_JAR)"; exit 2; }
for c in bench slow h1; do
  printf '%-6s ' "$c"
  java -cp "$JAR" tlc2.TLC -config "$c.cfg" Liveness.tla > "/tmp/tlc_$c.txt" 2>&1
  grep -oE "Invariant [A-Za-z]+ is violated|Model checking completed" "/tmp/tlc_$c.txt" | head -1
  echo "       trace: /tmp/tlc_$c.txt"
done
