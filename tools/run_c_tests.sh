#!/usr/bin/env bash
# Aggregate C unit-test runner for tuvm (host/posix). No aggregate runner existed
# pre-Milestone-A; this is it. KEEP tests only — fiber/NLR/reify/stack tests are
# deleted in later Milestone-A tasks and must NOT be listed here.
#
# NOTE on link-sets (discovered building this runner against the seed): dispatch.c/
# vm.c/oimg_loader.c now call into platform.c/serial.c/udp.c/mirror.c (indexable_new,
# {platform,serial,udp,mirror}_object/_dispatch), so the "engine" link-set is wider
# than the two-file Model A/B split some individual test header comments still show
# (see tests/test_coalesce.c's header for the up-to-date reference link line, which
# this script's ENG_A/ENG_B mirror).
set -u
cd "$(dirname "$0")/.." || exit 2
CC=${CC:-gcc}
# OEVM_BUILD=asan|ubsan: sanitizer coverage for the ISR-adjacent C (mailbox ring,
# handle table). Injected via CC (the only build knob run() interpolates). Host-only.
case "${OEVM_BUILD:-}" in
  asan)  CC="$CC -fsanitize=address -fno-omit-frame-pointer -g" ;;
  ubsan) CC="$CC -fsanitize=undefined -fno-sanitize-recover=undefined -g" ;;
  "" )   ;;
  * ) echo "unknown OEVM_BUILD='$OEVM_BUILD' (want asan|ubsan)"; exit 2 ;;
esac
STD="-std=c99 -Isrc"; OUT=/tmp/tuvm_ctest; fail=0
ENG_COMMON="src/platform.c src/serial.c src/udp.c src/mirror.c src/space.c src/handles.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c \
  src/backend/posix/pulse_posix.c"
ENG_A="src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c $ENG_COMMON src/solidified_image.c"
ENG_B="src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c $ENG_COMMON src/kernel_boot.c"
run() { # $1=name  $2=link-set  $3...=extra srcs/flags
  local name=$1; shift; local link=$1; shift
  printf '%-28s ' "$name"
  if $CC $STD tests/$name.c $link "$@" -lm -o $OUT 2>/tmp/tuvm_ctest.log && $OUT >/tmp/tuvm_ctest.out 2>&1; then
    echo PASS
  else echo FAIL; sed 's/^/    /' /tmp/tuvm_ctest.log /tmp/tuvm_ctest.out; fail=1; fi
}
skip() { # $1=name  $2=reason — pre-existing seed breakage unrelated to Milestone A;
         # documented so "green" stays honest instead of silently dropping the test.
  printf '%-28s SKIP (pre-existing: %s)\n' "$1" "$2"
}
# Model-A-linked core tests
for t in test_arena test_float_arena; do run $t "$ENG_A"; done
run test_coalesce "$ENG_A" -DOEVM_HEAP_SIZE=8192
# G3/M2 (nsl-tuvm#3): heap telemetry counters (live_bytes/high_water/
# largest_free_block/gc_cycles) — fragmentation early-warning API.
run test_heap_stats "$ENG_A" -DOEVM_HEAP_SIZE=65536
run test_method_cache "$ENG_A" -DOEVM_CACHE_STATS
# Model-B-linked tests (test_int_compare's own header comment specifies the
# Model-B link set — kernel_boot.c present, solidified_image.c absent — so it
# runs here, not with the Model-A group)
for t in test_int_compare test_millis test_platform; do run $t "$ENG_B"; done
# Milestone B Task 3: RawBytes leaf storage (basicNewBytes:/at:/at:put:/size/
# freezeBytes:) — Model-B link set, same reasoning as test_platform above.
run test_raw_bytes "$ENG_B"
# Milestone C Task 1: channel-space C substrate (space.c) — Model-B link set
# (space.c itself now rides ENG_COMMON, folded into both ENG_A/ENG_B above).
# Small capacity overrides make the boundary-fill/overflow properties fast
# and deterministic (same idiom as test_coalesce/test_heap_stats above).
run test_space "$ENG_B" -DOEVM_SPACE_MAX_CHANNELS=8 -DOEVM_SPACE_QUEUE_DEPTH=4
# M6 C-boundary handle table (src/handles.c): JNI-global-ref-style GC roots for
# C code (the Zephyr ISR->channel pulse seam, timer callbacks). Tiny capacity
# override makes the overflow property fast/deterministic (same idiom as
# test_space above).
run test_handles "$ENG_B" -DOEVM_HANDLE_TABLE_MAX=4
# ISR->channel pulse seam (src/pulse_backend.h + src/backend/posix/pulse_posix.c):
# posix in-process ring backing pulse_post_isr/pulse_pump, exercised end-to-end
# through the M6 handle table and the space queue. Tiny mailbox depth override,
# same idiom as test_space/test_handles above.
run test_pulse "$ENG_B" -DOEVM_PULSE_MAILBOX_DEPTH=4
# Rung 3 Task 2: CBOR integer leaf primitive `cborEncodeInt:` (prim 97) — Model-B
# link set (prim/gc/alloc test), same reasoning as test_platform/test_raw_bytes.
run test_cbor "$ENG_B"
# Backend-only smoke tests (link only the posix seam file, per each test's own
# header comment — NOT serial.c/udp.c, which need the full VM)
run test_serial "src/backend/posix/serial_posix.c"
run test_udp    "src/backend/posix/udp_posix.c"
# Header-only frame ABI test
printf '%-28s ' test_frame_abi
if $CC $STD tests/test_frame_abi.c -o $OUT 2>/tmp/tuvm_ctest.log && $OUT >/dev/null 2>&1; then echo PASS; else echo FAIL; cat /tmp/tuvm_ctest.log; fail=1; fi

# --- Task 1b: revived de-recursify-stale tests (were SKIP; the underlying
# feature each verifies still exists on the seed, only the test's own use of a
# now-deleted API was stale — see docs/fiber_derecursify_core_design.md
# Phases 2-3, and the per-test comments left in tests/*.c for what changed).
run test_kernel_boot        "$ENG_B"
# Header-only layout test (offsetof asserts only; no src/ link needed, like
# test_frame_abi above). Offsets updated to match current struct MethodContext
# (cleanup_cl deleted, regs shifted down) and the deleted struct InlineCache
# size assert removed.
printf '%-28s ' test_layout
if $CC $STD tests/test_layout.c -o $OUT 2>/tmp/tuvm_ctest.log && $OUT >/tmp/tuvm_ctest.out 2>&1; then
  echo PASS
else echo FAIL; sed 's/^/    /' /tmp/tuvm_ctest.log /tmp/tuvm_ctest.out; fail=1; fi
run test_oevm                "$ENG_A"
run test_gc_mark_iterative   "$ENG_A" -DOEVM_HEAP_SIZE=33554432 -lpthread
run test_float_gc            "$ENG_A"
# tuvm#15: float-arena OOM under a small embedded malloc pool. --wrap=malloc caps
# float-slab allocations below the slab BUDGET (so the budget trigger never fires),
# proving float_alloc reclaims-on-malloc-failure instead of aborting. Small cells so
# the capped slabs are cheap; budget deliberately > the pool cap the test enforces.
run test_float_oom           "$ENG_A" -DOEVM_FLOAT_ARENA_SLAB_CELLS=512 -DOEVM_FLOAT_ARENA_SLAB_BUDGET=8 -Wl,--wrap=malloc
run test_gc_trigger          "$ENG_A" -DOEVM_HEAP_SIZE=65536

# --- Task 10a: revived KEEP-subsystem tests for the vestigial access control
# subsystem (oevm#36). Were dead (in no runner, hardcoded pre-renumber opcode
# bytes); revival was mechanical — hardcoded instruction words recomposed from
# the current enum Opcode names (OP_RET was pre-renumber 4, is now 3;
# OP_SEND_IMPLICIT was pre-renumber 0x0F, is now 0x0D) — same pattern Task 9b
# applied in tests/test_debugger.c. No other structural drift found.
run test_access              "$ENG_A"
run test_enclosures          "$ENG_A"

# --- Pre-existing seed breakage, unrelated to the fiber/NLR/reify/growable-stack
# balloon this milestone removes. Confirmed by reading each test's compile/link
# error against current src/ + the repo's own docs (docs/ROADMAP.md P2;
# docs/fiber_derecursify_core_design.md Phases 2-3, "DONE" before this seed was
# cut). Excluded from the KEEP gate rather than silently patched or force-passed;
# revisit under a dedicated task, not Milestone A. (test_kernel_boot, test_layout,
# test_oevm, test_gc_mark_iterative, test_float_gc, test_gc_trigger were also
# here pre-Task-1b; all six revived above — see tests/*.c for what changed.)
skip test_solidified        "stale per-test fixture: hard-codes CounterVtable/Symbol_increment from an old per-test fixture the current generated image no longer provides; tracked in docs/ROADMAP.md P2 for a dedicated fixture (real fixture work, out of Task 1b's cheap-revival scope)"
skip test_oimg              "needs stub globals for gc.c's grown trace surface: designed to link gc.c standalone via its own stub globals, but gc.c now also requires StringVtable/StackVtable/ContextVtable/g_active_stack/debugger_hook/method_cache_flush/g_boot_inflight_receiver, which the test's stub block predates and does not provide; no existing-file link-set resolves this without either duplicate definitions (vm.c/dispatch.c) or missing symbols (real fixture work, out of Task 1b's cheap-revival scope)"

exit $fail
