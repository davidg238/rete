#!/usr/bin/env bash
# #163 — the ALLOCATION arm: does the encode rework reduce GC pressure?
#
# Runs the same bench against three states of `nodus/nsl/lib/kernel.nsl` on a
# heap constrained to the C6's 128 KB, where a collection is a real event. At a
# fixed arena, GC cycles per N encodes are proportional to bytes allocated.
#
#   baseline  the codec as #163 found it
#   step1     ByteBuilder holds the raw indexable
#   step3     + encodeMap: buffers only keys and streams values
#
# The working tree is restored on ANY exit path, including a failed build — a
# measurement script that can leave kernel.nsl swapped is a way to lose an
# afternoon and blame the compiler.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
KERNEL="$REPO/nodus/nsl/lib/kernel.nsl"
STEP1_COMMIT="${STEP1_COMMIT:-216a38b}"
HEAP="${HEAP:-131072}"

SAVED="$(mktemp)"
cp "$KERNEL" "$SAVED"
restore() { cp "$SAVED" "$KERNEL"; rm -f "$SAVED"; }
trap restore EXIT

run_arm() {
    local name="$1"
    echo "=== $name (heap ${HEAP} B) ==="
    EXTRA_CFLAGS="-DOEVM_HEAP_SIZE=${HEAP}" OUT="/tmp/cbor-gc-$name" \
        bash "$HERE/run.sh" >/dev/null 2>&1 || {
            echo "  BUILD OR RUN FAILED — see /tmp/cbor-gc-$name/stderr.txt"
            tail -3 "/tmp/cbor-gc-$name/stderr.txt" 2>/dev/null | sed 's/^/  /'
            return
        }
    grep -E "^(str|ints|map|list|batch)" "/tmp/cbor-gc-$name/report.txt" \
        | sed 's/ ns\/encode=[0-9]*//; s/ ns\/byte=[0-9]*//; s/^/  /'
}

git -C "$REPO" show "${STEP1_COMMIT}~1:nodus/nsl/lib/kernel.nsl" > "$KERNEL"
run_arm baseline

git -C "$REPO" show "${STEP1_COMMIT}:nodus/nsl/lib/kernel.nsl" > "$KERNEL"
run_arm step1

cp "$SAVED" "$KERNEL"
run_arm step3

echo
echo "gc = GC cycles across 400 encodes of that payload, at a fixed ${HEAP} B arena."
