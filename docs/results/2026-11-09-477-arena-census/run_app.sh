#!/usr/bin/env bash
# rete#477: build a census VM and run PortaBridgeNode against porta through porta_census.sh.
# Usage: run_app.sh TAG [cflags...]   e.g.  run_app.sh q28 -DVM_STACK_INIT_WORDS=1024 -DOEVM_HEAP_SIZE=28672
# Env: SEG=1 (+ -DOEVM_SEG_SPIKE) applies the size-segregation spike; KERNEL_FROM=<kernel.nsl> and
#      NSL_OVERRIDE=<ns_compile_oevm.py> take the kernel/compiler from a worktree (it needs a node_modules link).
# Out: OUT/TAG.run (harness), OUT/TAG.node.log (VM, CENSUS lines). Pass = "stop=1" (reached step 8).
set -uo pipefail
H=$(cd "$(dirname "$0")" && pwd); OUT=${OUT:-$HOME/census}; mkdir -p "$OUT"
tag=$1; shift
"$H/build.sh" "$OUT/$tag" "$@" -DOEVM_METHOD_PROFILE >/dev/null 2>&1 || { echo "$tag build fail"; exit 1; }
rm -f /tmp/porta_node.log   # a stale log from an earlier run must not read as this one's
( cd ~/workspace/rete/nodus/oevm && CENSUS_VM="$OUT/$tag/vm" CENSUS_LAYOUT="$OUT/$tag/layout.json" \
    timeout 900 bash "$H/porta_census.sh" > "$OUT/$tag.run" 2>&1 )
rc=$?
cp /tmp/porta_node.log "$OUT/$tag.node.log" 2>/dev/null || { echo "$tag: no node log"; tail -3 "$OUT/$tag.run"; exit 1; }
echo "$tag rc=$rc stop=$(grep -c CENSUS-STOP "$OUT/$tag.run") $(grep -m1 'CENSUS OOM' "$OUT/$tag.node.log") $(grep -E '^CENSUS gcs' "$OUT/$tag.node.log")"
