#!/usr/bin/env bash
# T0.2 — differential oracle: run the shared (pared) tests on the FROZEN original VM
# and the seeded tuvm, and diff stdout. Divergence = the deletion changed semantics.
# The frozen repos are a free oracle; this gate turns "did A change behavior?" into a
# mechanical question.
#
# OPTIONAL, STANDALONE gate (Task 10b): it depends on the frozen trees
# (~/workspace/oevm, ~/workspace/ns-oevm) existing, which CI-like contexts may lack —
# so it is NOT wired into run_tests_oevm.py's mandatory gates. Run it directly:
#   bash tuvm/tools/diff_oracle.sh
#
# Semantic caveat: the frozen oevm still has fibers/NLR and the pre-renumber
# opcodes/format; only tests whose SOURCE is byte-identical across both corpora AND
# whose stdout is verified identical on both VMs belong in the manifest (see
# nsl-tests/pared-subset.txt's header comment for the exact exclusion list and
# reasons — e.g. `^` in an escaping block compiles fine on frozen's real-NLR
# compiler but is a compile error on tuvm's no-NLR compiler, so that test cannot be
# an oracle test). Re-verify the manifest whenever either side's corpus changes.
#
# Each side compiles with its OWN tools (no cross-wiring):
#   frozen: ~/workspace/ns-oevm/tools/run_tests_oevm.py against ~/workspace/oevm
#   tuvm:   nsl-tuvm/tools/run_tests_oevm.py         against ~/workspace/tuvm
# Both runners build inside their own OEVM_DIR (src/solidified_image.c + .stmap are
# tracked files that get overwritten per test — this is normal harness behavior, see
# each repo's own test cycle). This script restores those two tracked files in the
# FROZEN tree afterward so `git -C ~/workspace/oevm status` stays clean; all other
# build artifacts (oevm_runner, /tmp/oevm_img, /tmp/*.oimg, /tmp/oevm_layout.json)
# are gitignored or already outside the tree.
#
# Run SOLO + FOREGROUND (harness not concurrency-safe; never run two suite runs at
# the same time — this script itself runs the two sides sequentially, never in
# parallel, for that reason).
#
# Usage: diff_oracle.sh
# Exit: 0 = no divergence, 1 = divergence (report printed), 2 = setup error.
set -u

FROZEN_VM_DIR=${FROZEN_VM_DIR:-$HOME/workspace/oevm}          # frozen VM (never modified)
FROZEN_TOOLS_DIR=${FROZEN_TOOLS_DIR:-$HOME/workspace/ns-oevm/tools}
TUVM_VM_DIR=${TUVM_VM_DIR:-$HOME/workspace/tuvm}              # seeded VM under test
TUVM_TOOLS_DIR=${TUVM_TOOLS_DIR:-$HOME/workspace/nsl-tuvm/tools}
TESTS_DIR=${TESTS_DIR:-$HOME/workspace/nsl-tests}
MANIFEST=${MANIFEST:-$TESTS_DIR/pared-subset.txt}             # shared-test list (Milestone A artifact)
OUT=${OUT:-$(mktemp -d)}

[ -x "$(command -v python3)" ] || { echo "python3 missing" >&2; exit 2; }
[ -f "$MANIFEST" ] || { echo "pared manifest not found: $MANIFEST" >&2; exit 2; }
[ -d "$FROZEN_VM_DIR" ] || { echo "frozen VM dir not found: $FROZEN_VM_DIR (frozen trees not present — this optional gate cannot run here)" >&2; exit 2; }
[ -d "$FROZEN_TOOLS_DIR" ] || { echo "frozen tools dir not found: $FROZEN_TOOLS_DIR" >&2; exit 2; }
[ -f "$FROZEN_TOOLS_DIR/run_tests_oevm.py" ] || { echo "frozen runner not found: $FROZEN_TOOLS_DIR/run_tests_oevm.py" >&2; exit 2; }
[ -f "$TUVM_TOOLS_DIR/run_tests_oevm.py" ] || { echo "tuvm runner not found: $TUVM_TOOLS_DIR/run_tests_oevm.py" >&2; exit 2; }

# Restore the frozen tree's tracked-but-harness-overwritten build artifacts no matter
# how this script exits (the harness rewrites these two files on every Path-A compile
# — see FROZEN_VM_DIR/src/solidified_image.{c,stmap} in git history).
cleanup() {
  git -C "$FROZEN_VM_DIR" checkout -- src/solidified_image.c src/solidified_image.stmap 2>/dev/null || true
}
trap cleanup EXIT

echo "== running shared corpus on FROZEN oevm ($FROZEN_VM_DIR, via $FROZEN_TOOLS_DIR) =="
OEVM_DIR="$FROZEN_VM_DIR" OEVM_PATH=AB OEVM_DUMP_DIR="$OUT/frozen" \
  python3 "$FROZEN_TOOLS_DIR/run_tests_oevm.py" > "$OUT/frozen_summary.txt" 2>&1
frozen_rc=$?
tail -6 "$OUT/frozen_summary.txt"
cleanup   # restore immediately; the tuvm run below cannot touch the frozen tree anyway

echo "== running shared corpus on TUVM ($TUVM_VM_DIR, via $TUVM_TOOLS_DIR) =="
OEVM_DIR="$TUVM_VM_DIR" OEVM_PATH=AB OEVM_DUMP_DIR="$OUT/tuvm" \
  python3 "$TUVM_TOOLS_DIR/run_tests_oevm.py" > "$OUT/tuvm_summary.txt" 2>&1
tuvm_rc=$?
tail -6 "$OUT/tuvm_summary.txt"

# Full-suite PASS/FAIL on either side is informational only here — a test can be a
# legitimate FAIL on one side's own harness (e.g. it's not in this manifest at all)
# without affecting the oracle; only the manifest's stdout diffs decide divergence.
: "$frozen_rc" "$tuvm_rc"

DIVERGED=0
CHECKED=0
while IFS= read -r t; do
  case "$t" in \#*|"") continue ;; esac
  CHECKED=$((CHECKED + 1))
  for label in A B; do
    f="$OUT/frozen/${t}.${label}.out"
    s="$OUT/tuvm/${t}.${label}.out"
    if [ ! -f "$f" ] || [ ! -f "$s" ]; then
      DIVERGED=1
      echo "MISSING DUMP: $t (path $label) — frozen:$( [ -f "$f" ] && echo present || echo missing ) tuvm:$( [ -f "$s" ] && echo present || echo missing )"
      echo "---"
      continue
    fi
    if ! diff -q "$f" "$s" >/dev/null 2>&1; then
      DIVERGED=1
      echo "DIVERGENCE: $t (path $label)"
      diff "$f" "$s" | head -20
      echo "---"
    fi
  done
done < "$MANIFEST"

if [ "$DIVERGED" -eq 0 ]; then
  echo "oracle: no divergence across $CHECKED shared tests (Path A + Path B)"
else
  echo "oracle: DIVERGENCE — the deletion changed observable semantics (see above)" >&2
fi
exit $DIVERGED
