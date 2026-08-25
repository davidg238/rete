#!/usr/bin/env bash
# tools/host-arena-floor.sh — bisect the SMALLEST OEVM_HEAP_SIZE that BOOTS on the host.
#
# The floor counterpart to tools/heap-ceiling.sh. The ceiling is a link-time
# property of a board; the floor is a run-time property of the host image VM, so
# this script actually boots what it builds.
#
# WHY IT EXISTS (rete#179). A host build at a device-sized arena aborts at boot:
#
#     VM GC Error: Out of Memory (requested 131096, used 360 of 131072)
#
# and for a year that read as "some boot-path allocation asks for the whole
# arena". It is the VM execution stack — vm.c:713 allocates a fixed-capacity
# Stack of VM_STACK_INIT_WORDS (default 16384) — and on a 64-bit host a word is
# 8 B, so the default stack is 131072 B, which is the C6's whole arena by
# coincidence. zephyr/run.sh exports VM_STACK_INIT_WORDS=4096 for every board,
# where a word is 4 B, so the device spends 16 408 B (12.5%) on the same object.
#
# THE FLOOR IS THEREFORE A FORMULA, not a mystery:
#
#     floor ~= VM_STACK_INIT_WORDS * oop_size + header + ~1520 B * (oop_size / 4)
#
# and the point of the script is to re-derive it after any change to the boot
# path, not to discover it again.
#
# BOTH KNOBS ARE COMPILE-TIME MACROS. Setting OEVM_HEAP_SIZE in the environment
# does nothing at all (gc.c:18-19 is an #ifndef with a 16 MB host default and is
# never getenv'd) — a run that does so measures the 16 MB default while claiming
# to measure the device. That is not hypothetical; it invalidated rete#199's
# first host reproduction, which is the more expensive half of #179.
#
# Usage: tools/host-arena-floor.sh [--words N] [--m32] [--test FILE.nsl] [--lo N] [--hi N]
#
#   --words N   VM_STACK_INIT_WORDS for the probe build (default 16384, the host
#               default; pass 4096 to ask the question the device answers).
#   --m32       Build 32-bit, i.e. at the device's word size. Needs gcc-multilib;
#               the .oimg is recompiled against the 32-bit layout automatically,
#               because a 64-bit image will not load into a 32-bit VM.
set -euo pipefail
cd "$(dirname "$0")/.."

WORDS=16384 M32="" TEST=../tests/01_expressions/01_addition.nsl EXPECT=7
LO=1024 HI=$((16 * 1024 * 1024))
while [ $# -gt 0 ]; do
  case "$1" in
    --words)  WORDS="$2";  shift 2 ;;
    --m32)    M32="-m32";  shift ;;
    --test)   TEST="$2";   shift 2 ;;
    --expect) EXPECT="$2"; shift 2 ;;
    --lo)     LO="$2";     shift 2 ;;
    --hi)     HI="$2";     shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -n "$M32" ]; then
  gcc -m32 -E -x c /dev/null >/dev/null 2>&1 || {
    echo "SKIP: gcc-multilib not installed. Run: sudo apt-get install gcc-multilib"; exit 77; }
fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
NSL=../nsl/tools/ns_compile_oevm.py
TARGET="${OEVM_TARGET:-posix}"

# The layout must match the probe's word size, or the image will not load.
gcc $M32 -w -std=c99 -Isrc tools/dump_layout.c -o "$WORK/dl"
"$WORK/dl" > "$WORK/layout.json"
python3 "$NSL" "$TEST" "$WORK/app.oimg" --format=oimg --layout="$WORK/layout.json" >/dev/null

SRC=(src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c src/span.c
     src/serial.c src/udp.c src/http.c src/http_core.c src/mirror.c src/ota.c
     "src/backend/$TARGET"/*.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c)

# 0 = boots (and produces the expected output), 1 = does not.
try() {
  local n="$1" bin="$WORK/probe"
  gcc $M32 -w -std=c99 -Isrc -O2 "${SRC[@]}" -o "$bin" \
      "-DOEVM_HEAP_SIZE=$n" "-DVM_STACK_INIT_WORDS=$WORDS" -lm 2>/dev/null || return 1
  local out; out=$("$bin" "$WORK/app.oimg" 2>&1) || return 1
  [ "$out" = "$EXPECT" ]
}

echo "probing: ${M32:-64-bit} VM_STACK_INIT_WORDS=$WORDS test=$(basename "$TEST")"
try "$HI" || { echo "does not boot even at OEVM_HEAP_SIZE=$HI — not a floor question"; exit 1; }
if try "$LO"; then echo "boots at the probe floor $LO; lower --lo to bisect"; exit 0; fi

while [ $((HI - LO)) -gt 8 ]; do
  MID=$(( (LO + HI) / 2 ))
  if try "$MID"; then HI=$MID; else LO=$MID; fi
done

echo "smallest booting OEVM_HEAP_SIZE = $HI   (largest failing = $LO)"
