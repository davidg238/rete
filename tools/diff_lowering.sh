#!/usr/bin/env bash
# diff_lowering.sh — master-vs-branch differential harness for the stratify-execution tasks.
#
# Purpose: prove that Tasks 1–4 (new opcodes) preserve byte-identical program stdout
# for every test in the corpus. An empty diff == semantics preserved == PASS.
#
# Design:
#   TWO master worktrees (OLD compiler + OLD VM) are compared against the branch
#   (NEW compiler + NEW VM). This is the correct oracle: Tasks 1–4 change BOTH the
#   ns-oevm compiler (ns_compile_oevm.py) and the oevm VM (vm.c), so comparing only
#   the VM would cause the old compiler to emit opcodes the new VM can't decode.
#
#   stratify-old-nsoevm: master ns-oevm (OLD compiler, solidify.py)
#   stratify-old-oevm:   master oevm (OLD VM / C runtime)
#
#   The runner (run_tests_oevm.py) is NOT part of the system-under-test — it is
#   the observation camera. We therefore overlay the BRANCH runner onto the old
#   worktree so OEVM_DUMP_DIR support (added in Task 0) is available on both sides.
#   The OLD compiler (ns_compile_oevm.py) and OLD kernel (solidify.py) stay at master.
#   This is deliberate, not cheating: the runner only drives the test, it does not
#   alter lowering or VM semantics.
#
# At Task 0 the old and new code are identical (only ns-tests gains the battery,
# and both runs read the same ns-tests corpus), so this script MUST produce an empty
# diff. That empty diff is the harness self-test.
#
# Tasks 1–4 keep their test gate here and leave the worktrees in place.
# Task 4 (or a cleanup task) removes them when they are no longer needed.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

OLD_NSOEVM="/home/david/workspace/stratify-old-nsoevm"
OLD_OEVM="/home/david/workspace/stratify-old-oevm"
NEW_NSOEVM="$ROOT"
NEW_OEVM="/home/david/workspace/oevm"

DUMP_OLD="/tmp/lower_old"
DUMP_NEW="/tmp/lower_new"

# ── Create master worktrees (idempotent: ignore "already exists" errors) ─────
git -C /home/david/workspace/oevm worktree add -f "$OLD_OEVM" master 2>/dev/null || true
git -C /home/david/workspace/ns-oevm worktree add -f "$OLD_NSOEVM" master 2>/dev/null || true

# Symlink the ohm-js dependency so the old compiler can parse (node_modules is
# not tracked in git; it lives only in the real ns-oevm working directory).
if [ ! -e "$OLD_NSOEVM/grammar/node_modules" ]; then
    ln -s /home/david/workspace/ns-oevm/grammar/node_modules \
          "$OLD_NSOEVM/grammar/node_modules"
fi

# Overlay the BRANCH runner onto the old worktree (pure observation camera,
# not compiler or kernel — the OLD lowering stays at master).
# Use a plain copy rather than a git-checkout overlay so the old worktree's
# index stays clean against its own branch (no unintended staged change).
cp "$NEW_NSOEVM/tools/run_tests_oevm.py" "$OLD_NSOEVM/tools/run_tests_oevm.py"

# ── Clean dump dirs ────────────────────────────────────────────────────────────
rm -rf "$DUMP_OLD" "$DUMP_NEW"

# ── OLD run (master compiler + master VM) ─────────────────────────────────────
echo "==> Running OLD (master compiler + master VM) ..."
cd "$OLD_NSOEVM"
OEVM_DIR="$OLD_OEVM" \
OEVM_DUMP_DIR="$DUMP_OLD" \
OEVM_PATH=AB \
    python3 tools/run_tests_oevm.py
OLD_EXIT=$?

# ── NEW run (branch compiler + branch VM) ─────────────────────────────────────
echo ""
echo "==> Running NEW (branch compiler + branch VM) ..."
cd "$NEW_NSOEVM"
OEVM_DIR="$NEW_OEVM" \
OEVM_DUMP_DIR="$DUMP_NEW" \
OEVM_PATH=AB \
    python3 tools/run_tests_oevm.py
NEW_EXIT=$?

# ── Diff ──────────────────────────────────────────────────────────────────────
echo ""
echo "==> Diffing stdout dumps ($DUMP_OLD vs $DUMP_NEW) ..."
DIFF_OUT="$(diff -r "$DUMP_OLD" "$DUMP_NEW" 2>&1)" || true

if [ -z "$DIFF_OUT" ] && [ "$OLD_EXIT" -eq 0 ] && [ "$NEW_EXIT" -eq 0 ]; then
    echo "PASS — diff is empty; master and branch produce identical stdout for all tests."
    exit 0
else
    if [ -n "$DIFF_OUT" ]; then
        echo "FAIL — stdout differs between master and branch:"
        echo "$DIFF_OUT"
    fi
    if [ "$OLD_EXIT" -ne 0 ]; then
        echo "FAIL — OLD run exited with code $OLD_EXIT"
    fi
    if [ "$NEW_EXIT" -ne 0 ]; then
        echo "FAIL — NEW run exited with code $NEW_EXIT"
    fi
    exit 1
fi
