#!/bin/bash
# Structural guard (mirror step): the 12 reflective prims must NOT be reachable as
# ambient receiver-side primitives -- they live behind the `mirror` capability only.
# Fails if any reflective `case PRIM_x:` reappears in the universal switch(pid) of
# dispatch.c. Run from the oevm repo root. Companion to the conformance suite, which
# proves nothing load-bearing was confined; this proves confinement actually happened
# (a behavior oracle alone cannot -- a still-working ambient prim looks identical).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DISPATCH="$HERE/../src/dispatch.c"
if [ ! -f "$DISPATCH" ]; then
  echo "check_no_ambient_reflection: cannot find $DISPATCH" >&2
  exit 2
fi
hits=$(grep -nE 'case PRIM_(vtable|delegate|bindings|sender|pc|home|registerDebuggerHook|regAt|regAt_put|singleStep|bind|instructionAt_put):' "$DISPATCH" || true)
if [ -n "$hits" ]; then
  echo "FAIL: ambient reflective case(s) still present in switch(pid):"
  echo "$hits"
  exit 1
fi
echo "OK: no ambient reflective cases in dispatch.c switch(pid)"
