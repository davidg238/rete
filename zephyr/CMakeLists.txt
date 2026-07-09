cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tuvm_pulse)

# Task 7 (native_sim bring-up gate): the Model A ("solidified image") boot
# path — vm.c/gc.c/dispatch.c + a per-program src/solidified_image.c baked by
# tools/ns_compile_oevm.py from zephyr/PulseNode.ns, exactly the recipe
# tools/run_tests_oevm.py uses for the host Path-A test runner (oevm_runner),
# minus src/oevm_main.c (this directory's src/main.c is the Zephyr entry
# point instead) and minus src/oimg_loader.c + src/kernel_boot.c — both of
# those define their own `Vtable_*`/kernel-symbol statics that CLASH with the
# ones solidified_image.c bakes for THIS specific program (see
# tools/run_tests_oevm.py's run_c_unit_tests() docstring for the identical
# constraint on the host build). src/serial.c and src/udp.c are linked even
# though this gate's node grants no serial/net capability: src/dispatch.c's
# send() fast path unconditionally calls serial_object()/udp_object() to test
# receiver identity, so both — and the zephyr backend files that back them —
# must link regardless (hence prj.conf's native_sim NSOS networking block).
set(TUVM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_sources(app PRIVATE
  src/main.c
  ${TUVM_SRC}/src/vm.c ${TUVM_SRC}/src/gc.c ${TUVM_SRC}/src/dispatch.c
  ${TUVM_SRC}/src/platform.c ${TUVM_SRC}/src/space.c ${TUVM_SRC}/src/handles.c
  ${TUVM_SRC}/src/serial.c ${TUVM_SRC}/src/udp.c ${TUVM_SRC}/src/mirror.c
  ${TUVM_SRC}/src/solidified_image.c
  ${TUVM_SRC}/src/backend/zephyr/platform_zephyr.c
  ${TUVM_SRC}/src/backend/zephyr/serial_zephyr.c
  ${TUVM_SRC}/src/backend/zephyr/udp_zephyr.c
  ${TUVM_SRC}/src/backend/zephyr/pulse_zephyr.c
)
target_include_directories(app PRIVATE ${TUVM_SRC}/src)

# Tunables via ENV, not `-D`: NCS sysbuild swallows a bare
# `west build -- -DOEVM_HEAP_SIZE=...` (it lands in the sysbuild cache, never
# the app image) — same wrinkle ns-zephyr/app/CMakeLists.txt documents. Env
# vars are inherited by this CMake invocation unconditionally.
if(DEFINED ENV{OEVM_HEAP_SIZE})
  target_compile_definitions(app PRIVATE OEVM_HEAP_SIZE=$ENV{OEVM_HEAP_SIZE})
endif()

# The VM's method/prim inline caches (dispatch.c) default to 4096 entries each
# — 64 KB + 32 KB of static SRAM, host-generous. On the 256 KB nRF52840 that
# dwarfs the arena, so Task 8's DK build shrinks them via env (same env-not-`-D`
# reason as OEVM_HEAP_SIZE above: NCS sysbuild swallows bare `-D` app defines).
# They are pure caches — a smaller table only trades a few more collisions, never
# correctness.
if(DEFINED ENV{METHOD_CACHE_SIZE})
  target_compile_definitions(app PRIVATE METHOD_CACHE_SIZE=$ENV{METHOD_CACHE_SIZE})
endif()
if(DEFINED ENV{PRIM_CACHE_SIZE})
  target_compile_definitions(app PRIVATE PRIM_CACHE_SIZE=$ENV{PRIM_CACHE_SIZE})
endif()
