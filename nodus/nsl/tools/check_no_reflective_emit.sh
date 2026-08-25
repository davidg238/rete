#!/usr/bin/env bash
# check_no_reflective_emit.sh — assert base-level execution emits no reflective sends.
# Pass means the stratification is complete: OP_LD_LEXICAL/OP_ST_LEXICAL/OP_CLOSURE/OP_LD_SENDER
# handle what was previously emitted as reflective message-sends.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# --- Part 1: compiler must not emit reflective selectors for base-level operations ---
for sel in '"home"' '"regAt:"' '"regAt:put:"' '"createClosure:"'; do
  if grep -n "add_constant($sel)" tools/ns_compile_oevm.py; then
    echo "FAIL: compiler still emits reflective send for $sel"
    fail=1
  fi
done

# --- Part 2: Path B — unwindTo:value: method consts must not reference KSym_sender ---
# NOTE: KSym_sender legitimately remains in kernel_boot.c (Symbol definition + builtin_registry).
# We scope the check to ONLY the Method_unwindTo_value__consts[] array block.
uw_consts=$(awk '/Method_unwindTo_value__consts\[\]/,/};/' ../oevm/src/kernel_boot.c)
# grep consumes the stream (no -q) in both checks: under pipefail a first-match
# exit SIGPIPEs the echo and 141 reads as "no match" — hiding a violation (rete#260).
if echo "$uw_consts" | grep "KSym_sender" >/dev/null; then
  echo "FAIL: Path B Method_unwindTo_value__consts[] still references KSym_sender"
  fail=1
fi

# --- Part 3: Path A — ns_compile_oevm.py unwindTo:value: add_method block must not contain "sender" in constants ---
# "sender" may legitimately appear elsewhere in ns_compile_oevm.py (prim registration etc.).
# We scope the check to ONLY the selector="unwindTo:value:" add_method block (~25 lines).
uw_block=$(awk '/selector="unwindTo:value:"/{found=1} found{print; if (/^\s*\]$/ && printed++) exit; printed++}' tools/ns_compile_oevm.py)
if echo "$uw_block" | grep '"sender"' >/dev/null; then
  echo "FAIL: Path A ns_compile_oevm.py unwindTo:value: add_method block still contains \"sender\" in constants"
  fail=1
fi

[ $fail -eq 0 ] && echo "OK: base-level execution emits no reflective sends"
exit $fail
