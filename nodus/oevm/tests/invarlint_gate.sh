#!/usr/bin/env bash
# nodus/oevm/tests/invarlint_gate.sh — the architecture ratchet (invarlint#48, #54).
#
#   ./invarlint_gate.sh        invarlint check --baseline on oevm's photograph
#
# Exit 0 = PASS (no key new or stale against invarlint/out/findings.baseline.json),
# 1 = FAIL (the moved keys are named on stdout), 77 = SKIP (a missing input, named).
#
# WHAT IT READS. invarlint extracts from clang's JSON AST of the ESP32-C6 build's compile
# database (invarlint/c.yaml). The database is build output: zephyr/run.sh --board
# esp32c6 --model B with the Thread overlays and OEVM_OTA_PUBKEY, the configuration the
# baseline was photographed from. Another configuration photographs other code (Thread off
# drops thread_bringup.c) and reads as moved keys, not as a pass.
#
# WHAT IT WRITES. check writes invarlint/out/{facts,findings}.json and findings.md. When
# they were clean before the run they are restored after it: the suite must not dirty the tree.
set -u
cd "$(dirname "$0")/.."
IL="${INVARLINT:-$(command -v invarlint || echo "$HOME/invarlint/invarlint")}"
DB="$(sed -n 's/^compdb: *//p' invarlint/c.yaml)"
[ -x "$IL" ] || { echo "invarlint: SKIP (no invarlint binary; set INVARLINT)"; exit 77; }
command -v clang >/dev/null || { echo "invarlint: SKIP (no clang on PATH)"; exit 77; }
[ -f "$DB" ] || { echo "invarlint: SKIP (no $DB: build the C6 first, see c.yaml)"; exit 77; }
OUT="invarlint/out/facts.json invarlint/out/findings.json invarlint/out/findings.md invarlint/out/terms.md"
clean=$(git status --porcelain -- $OUT)
"$IL" check --baseline . ; rc=$?
[ -z "$clean" ] && git checkout -q -- $OUT
case $rc in
  0) echo "invarlint: PASS (no key moved against the baseline)"; exit 0 ;;
  1) echo "invarlint: FAIL (keys moved: see above, or invarlint/out/findings.md)"; exit 1 ;;
  *) echo "invarlint: FAIL (invarlint exit $rc)"; exit 1 ;;
esac
