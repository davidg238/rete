#!/usr/bin/env bash
# docs/tutorials/gate.sh — a tutorial's command blocks are executed, not trusted.
#
#   ./gate.sh <tutorial.md>      extract every ```bash gate block, run them in order
#   ./gate.sh --list <tutorial.md>   print the blocks it would run and exit
#
# Exit 0 = PASS, 1 = FAIL. Run by the nsl suite once per page, as the `tutorial-host` and
# `tutorial-porta` aux gates. A new tutorial is one more call to run_tutorial_gate().
#
# WHY THIS EXISTS. porta/docs/GETTING-STARTED.md walked an outsider through a command
# vocabulary that had been deleted for weeks; nothing said so until someone read it.
# A tutorial rots faster than any other document because it is the one nobody who knows
# the system re-reads. So the blocks here are run by the suite, and staleness reads red.
#
# HOW. Fenced blocks whose info string is exactly `bash gate` are extracted in document
# order and concatenated into ONE script, run under `bash -euo pipefail` in a fresh
# temporary directory with RETE exported as this repo's root. Plain ```bash blocks are
# shown to the reader and NOT run (the reader's shell setup, the four-minute suite run).
# One script rather than one per block, because the blocks build on each other's files
# the way the reader's session does.
#
# The gate checks its own blindness first: a tutorial consisting of a single failing
# block MUST fail. A gate that extracts nothing and passes on vacancy is a gate that
# would pass a deleted tutorial.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
export RETE="$(cd "$here/../.." && pwd)"

extract() {   # $1 = markdown file → stdout: the concatenated gate blocks
  awk '
    /^```bash gate[[:space:]]*$/ { inblock = 1; n++; printf "\n# --- block %d ---\n", n; next }
    /^```[[:space:]]*$/ && inblock { inblock = 0; next }
    inblock { print }
  ' "$1"
}

run_blocks() {   # $1 = markdown file → runs the blocks; returns their exit status
  local md="$1" script work
  script="$(mktemp)"
  extract "$md" > "$script"
  if ! grep -q '^# --- block 1 ---' "$script"; then
    echo "tutorial-gate: no 'bash gate' blocks found in $md" >&2
    rm -f "$script"; return 1
  fi
  work="$(mktemp -d)"
  ( cd "$work" && bash -euo pipefail "$script" )
  local rc=$?
  rm -rf "$work" "$script"
  return $rc
}

if [ "${1:-}" = "--list" ]; then
  extract "$2"; exit 0
fi
md="${1:?usage: gate.sh <tutorial.md>}"

# Canary: a failing block must be reported as a failure.
canary="$(mktemp --suffix=.md)"
printf '```bash gate\nfalse\n```\n' > "$canary"
if run_blocks "$canary" >/dev/null 2>&1; then
  echo "tutorial-gate: FAIL (canary — a failing block passed; the gate is blind)" >&2
  rm -f "$canary"; exit 1
fi
rm -f "$canary"

if run_blocks "$md"; then
  echo "tutorial-gate: PASS ($(basename "$md"))"
else
  echo "tutorial-gate: FAIL ($(basename "$md") — a block above exited non-zero)" >&2
  exit 1
fi
