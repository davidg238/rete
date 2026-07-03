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
CC=${CC:-gcc}; STD="-std=c99 -Isrc"; OUT=/tmp/tuvm_ctest; fail=0
ENG_COMMON="src/platform.c src/serial.c src/udp.c src/mirror.c \
  src/backend/posix/platform_posix.c src/backend/posix/serial_posix.c src/backend/posix/udp_posix.c"
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
run test_method_cache "$ENG_A" -DOEVM_CACHE_STATS
# Model-B-linked tests (test_int_compare's own header comment specifies the
# Model-B link set — kernel_boot.c present, solidified_image.c absent — so it
# runs here, not with the Model-A group)
for t in test_int_compare test_millis test_platform; do run $t "$ENG_B"; done
# Backend-only smoke tests (link only the posix seam file, per each test's own
# header comment — NOT serial.c/udp.c, which need the full VM)
run test_serial "src/backend/posix/serial_posix.c"
run test_udp    "src/backend/posix/udp_posix.c"
# Header-only frame ABI test
printf '%-28s ' test_frame_abi
if $CC $STD tests/test_frame_abi.c -o $OUT 2>/tmp/tuvm_ctest.log && $OUT >/dev/null 2>&1; then echo PASS; else echo FAIL; cat /tmp/tuvm_ctest.log; fail=1; fi

# --- Pre-existing seed breakage, unrelated to the fiber/NLR/reify/growable-stack
# balloon this milestone removes. Confirmed by reading each test's compile/link
# error against current src/ + the repo's own docs (docs/ROADMAP.md P2;
# docs/fiber_derecursify_core_design.md Phases 2-3, "DONE" before this seed was
# cut). Excluded from the KEEP gate rather than silently patched or force-passed;
# revisit under a dedicated task, not Milestone A.
skip test_gc_mark_iterative "references deleted vm_c_stack_base/gc_set_stack_base (conservative C-stack scan removed in derecursify Phase 3, fiber_derecursify_core_design.md 6d)"
skip test_gc_trigger        "references deleted vm_c_stack_base/gc_set_stack_base (conservative C-stack scan removed in derecursify Phase 3, fiber_derecursify_core_design.md 6d)"
skip test_float_gc          "references deleted vm_c_stack_base/gc_set_stack_base (conservative C-stack scan removed in derecursify Phase 3, fiber_derecursify_core_design.md 6d)"
skip test_oevm              "test_inline_cache references struct InlineCache/send_ic, deleted in derecursify Phase 2 (oevm.h:244-246, fiber_derecursify_core_design.md)"
skip test_layout             "asserts MethodContext.cleanup_cl (field deleted in derecursify Phase 3, fiber_derecursify_core_design.md 6b) and sizeof(struct InlineCache) (deleted Phase 2)"
skip test_solidified        "hard-codes CounterVtable/Symbol_increment from an old per-test fixture the current generated image no longer provides (docs/ROADMAP.md P2, tracked there for a dedicated fixture)"
skip test_oimg              "designed to link gc.c standalone via its own stub globals, but gc.c now also requires StringVtable/StackVtable/ContextVtable/g_active_stack/debugger_hook/method_cache_flush/g_boot_inflight_receiver, which the test's stub block predates and does not provide; no existing-file link-set resolves this without either duplicate definitions (vm.c/dispatch.c) or missing symbols"
skip test_kernel_boot       "asserts BUILTIN_REGISTRY_VERSION == 2; current src/oimg_loader.c defines it as 4 (registry evolved, test constant not updated)"

exit $fail
