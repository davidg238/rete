cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(oevm_pulse)

# Task 7 (native_sim bring-up gate): the Model A ("solidified image") boot
# path — vm.c/gc.c/dispatch.c + a per-program src/solidified_image.c baked by
# tools/ns_compile_oevm.py from zephyr/PulseNode.nsl, exactly the recipe
# tools/run_tests_oevm.py uses for the host Path-A test runner (oevm_runner),
# minus src/oevm_main.c (this directory's src/main.c is the Zephyr entry
# point instead). Only src/kernel_boot.c is mutually exclusive with
# solidified_image.c: it defines 13 `Vtable_*` symbols (Vtable_SymbolVtable,
# Vtable_IntegerVtable, ...) that solidified_image.c also bakes for THIS program
# — verified, the overlap is exactly those 13. src/oimg_loader.c does NOT clash
# and is linked under BOTH models: it shares no symbol with solidified_image.c,
# and the host has always linked the two together — see ENG_A / ENG_B in
# nodus/nsl/tools/run_tests_oevm.py (ENG_A = oimg_loader.c + solidified_image.c,
# ENG_B = oimg_loader.c + kernel_boot.c). The OEVM_MODEL knob below is that same
# ENG_A/ENG_B split.
# (This citation has now been wrong in BOTH directions. An earlier revision cited
# a run_c_unit_tests() in tools/run_tests_oevm.py that did not exist, and was
# corrected to name tools/run_c_tests.sh; rete#275 then DELETED that script — it
# was a second, unrun runner — and moved the link sets into run_c_unit_tests(),
# which is where they are now. Check the file before trusting the line.)
# src/serial.c and src/udp.c are linked even
# though this gate's node grants no serial/net capability: src/dispatch.c's
# send() fast path unconditionally calls serial_object()/udp_object() to test
# receiver identity, so both — and the zephyr backend files that back them —
# must link regardless (hence prj.conf's native_sim NSOS networking block).
set(OEVM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_sources(app PRIVATE
  src/main.c
  src/log_udp.c
  # rete#215: the retained panic note (.rtc_noinit) + the k_sys_fatal_error_handler
  # override. App-side, not an engine file: platform.c externs the five seam symbols the
  # way it already externs bell/ntp/rtc, so none of the nine engine source lists (#63)
  # changes.
  src/reboot_note.c
  # rete#215: the wedge detector. App-side beside reboot_note.c for the same reason — it
  # is reached only from main.c, and placing it in src/backend/zephyr/ would cost a posix
  # stub in nine source lists (#63) that nothing on the host would call.
  src/observer.c
  ${OEVM_SRC}/src/vm.c ${OEVM_SRC}/src/gc.c ${OEVM_SRC}/src/dispatch.c
  ${OEVM_SRC}/src/platform.c ${OEVM_SRC}/src/space.c ${OEVM_SRC}/src/handles.c
  ${OEVM_SRC}/src/span.c
  ${OEVM_SRC}/src/serial.c ${OEVM_SRC}/src/udp.c ${OEVM_SRC}/src/mirror.c
  ${OEVM_SRC}/src/backend/zephyr/platform_zephyr.c
  ${OEVM_SRC}/src/backend/zephyr/serial_zephyr.c
  ${OEVM_SRC}/src/backend/zephyr/udp_zephyr.c
  ${OEVM_SRC}/src/backend/zephyr/pulse_zephyr.c
  ${OEVM_SRC}/src/backend/zephyr/timer_zephyr.c
  ${OEVM_SRC}/src/backend/zephyr/span_zephyr.c
  ${OEVM_SRC}/src/backend/zephyr/sensor_zephyr.c
  ${OEVM_SRC}/src/http.c
  ${OEVM_SRC}/src/http_core.c
  ${OEVM_SRC}/src/backend/zephyr/http_zephyr.c
  # rete#25: the image-push seam. NOT optional here — platform.c's prims 145-150
  # call ota_running_hash() and its siblings unconditionally, so omitting these
  # two files fails the link on nine undefined symbols. ota_zephyr.c is
  # deliberately inert (no flash, no reboot); read its header comment before
  # assuming the omission was the safer choice.
  ${OEVM_SRC}/src/ota.c
  ${OEVM_SRC}/src/backend/zephyr/ota_zephyr.c
)
target_include_directories(app PRIVATE ${OEVM_SRC}/src)

# ---- Build id (#56 §5.4, part of #74) --------------------------------------
# The firmware prints this before the VM starts, and run.sh files the flashed
# ELF under the same string, so a log segment always names the binary it came
# from. OUTPUT lists a file the command never creates, which is what forces the
# command to re-run on every build — the timestamp must be this build's, not the
# first build's. Two builds keyed alike would silently archive one binary over
# another, which is the failure D4's refusal exists to prevent.
#
# NOT GNU build-id: that was tried on hardware and rejected. Any SHT_NOTE input
# section makes GNU ld synthesise a PT_NOTE spanning the whole rodata region and
# the Espressif image builder mishandles it — the board boots, then emits
# garbage. Three-boot A/B/A attribution in the design doc §7. Do not re-attempt
# without reading it.
set(OEVM_BUILD_ID_C   ${CMAKE_CURRENT_BINARY_DIR}/oevm_build_id.c)
set(OEVM_BUILD_ID_TXT ${CMAKE_CURRENT_BINARY_DIR}/oevm_build_id.txt)
add_custom_command(
  OUTPUT ${OEVM_BUILD_ID_C} ${OEVM_BUILD_ID_TXT} ${CMAKE_CURRENT_BINARY_DIR}/build_id.always
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT_C=${OEVM_BUILD_ID_C}
          -DOUT_TXT=${OEVM_BUILD_ID_TXT}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
  COMMENT "Baking oevm build id")
add_custom_target(oevm_build_id_gen DEPENDS ${OEVM_BUILD_ID_C})
target_sources(app PRIVATE ${OEVM_BUILD_ID_C})
add_dependencies(app oevm_build_id_gen)

# rete#243 — the manifest pin, baked beside the build id and re-made every
# build for the same reason: the pin names the contract this exact image was
# built against. cmake/gen_manifest_pin.cmake wraps
# protocol/tools/pin_manifest.py (fetch-from-porta with loud repo fallback);
# archive.sh copies the pinned file + hash + source into the build-id entry.
set(OEVM_MANIFEST_PIN_C   ${CMAKE_CURRENT_BINARY_DIR}/oevm_manifest_pin.c)
set(OEVM_MANIFEST_PIN_TXT ${CMAKE_CURRENT_BINARY_DIR}/oevm_manifest_pin.txt)
add_custom_command(
  OUTPUT ${OEVM_MANIFEST_PIN_C} ${OEVM_MANIFEST_PIN_TXT} ${CMAKE_CURRENT_BINARY_DIR}/manifest_pin.always
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DBUILD=${CMAKE_CURRENT_BINARY_DIR}
          -DOUT_C=${OEVM_MANIFEST_PIN_C}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_manifest_pin.cmake
  COMMENT "Baking manifest pin")
add_custom_target(oevm_manifest_pin_gen DEPENDS ${OEVM_MANIFEST_PIN_C})
target_sources(app PRIVATE ${OEVM_MANIFEST_PIN_C})
add_dependencies(app oevm_manifest_pin_gen)

# THE SAME ENV-NOT-`-D` RULE THE TUNABLES BELOW ALREADY FOLLOW, applied to the three
# variables that select WHICH PROGRAM this image contains (rete#281, 2026-08-30).
#
# It was applied to the tunables and not to these, and that asymmetry is the whole
# defect: under NCS sysbuild a bare `-DOEVM_BLOB=...` after `--` lands in the SYSBUILD
# cache and never reaches this image. The four consequences all look like success —
# OEVM_MODEL falls back to the "B" default, OEVM_BLOB to the committed TimerNode.oimg
# "so it just works", OEVM_NODE_ID to nothing — so `run.sh --node PortaBridgeNodeNrf.nsl`
# baked the right .oimg, printed the right lines, and then built TimerNode with no node
# id. A dongle flashed that way goes dark: no mesh, no console, nothing to see.
#
# Image-scoped `-Dzephyr_OEVM_BLOB=...` was tried first and DOES NOT WORK: it reaches
# zephyr_sysbuild_cache.txt and still leaves the image's own cache untouched (measured,
# same day). Env is the mechanism that already works here for five other variables, so
# this adds none.
if(DEFINED ENV{OEVM_BLOB})
  set(OEVM_BLOB $ENV{OEVM_BLOB})
endif()
if(DEFINED ENV{OEVM_NODE_ID})
  set(OEVM_NODE_ID $ENV{OEVM_NODE_ID})
endif()

# rete#186 — WHICH BOARD this image is for. Sibling of the build id above; see
# cmake/gen_node_id.cmake for why it is configure-time where that one is build-time,
# and why an unset id bakes "unnamed" rather than a guess.
set(OEVM_NODE_ID_C ${CMAKE_CURRENT_BINARY_DIR}/oevm_node_id.c)
execute_process(COMMAND ${CMAKE_COMMAND}
                -DNODE_ID=${OEVM_NODE_ID}
                -DOUT_C=${OEVM_NODE_ID_C}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_node_id.cmake)
target_sources(app PRIVATE ${OEVM_NODE_ID_C})
message(STATUS "oevm node id: ${OEVM_NODE_ID}")

# OpenThread bring-up TU (oevm_thread_bringup). Gated on OPENTHREAD so the WROOM
# (WiFi, no Thread) compiles it away. ONE shared impl: nRF (NCS) and C6 (upstream
# 4.2) expose the same no-arg OT API, so the earlier board split was redundant
# (#59, 2026-07-20). Re-split behind thread_bringup.h only if the APIs diverge.
if (CONFIG_NET_L2_OPENTHREAD)
  target_sources(app PRIVATE src/thread_bringup.c)
endif()

# ---- Model A/B knob -------------------------------------------------------
# A (default): the per-program baked image, src/solidified_image.c.
# B: a generic VM + a .oimg blob .incbin'd into flash .rodata.
# The link sets are MUTUALLY EXCLUSIVE: solidified_image.c XOR kernel_boot.c —
# see the 13-symbol Vtable_* overlap explained at the top of this file. This is
# the ENG_A/ENG_B split the host C unit gate has always used
# (nodus/nsl/tools/run_tests_oevm.py, C_UNIT_TESTS).
#
# Model A links no oimg_loader.c: zephyr/src/main.c stubs the four globals it
# would own (oimg_blob_base/_end, builtin_registry, oimg_singleton), and
# oimg.h's is_in_blob() returns 0 while oimg_blob_base is NULL, so the XIP
# accessor paths are never taken.
set(OEVM_MODEL "B" CACHE STRING "oevm image model: A (solidified) or B (.oimg blob)")
set_property(CACHE OEVM_MODEL PROPERTY STRINGS A B)
# OEVM_MODEL's env override belongs HERE, not with the other two above: it has to
# follow its own `set(... CACHE ...)` line. A normal variable set before the cache
# declaration and a cache entry created after it is the CMP0126 shadowing question,
# whose answer depends on a policy setting rather than on this file — and "which of
# two same-named variables wins" is precisely the class of silence this whole change
# exists to remove.
if(DEFINED ENV{OEVM_MODEL})
  set(OEVM_MODEL $ENV{OEVM_MODEL})
endif()

if(OEVM_MODEL STREQUAL "A")
  target_sources(app PRIVATE ${OEVM_SRC}/src/solidified_image.c)
elseif(OEVM_MODEL STREQUAL "B")
  if(NOT DEFINED OEVM_BLOB)
    # Model B is the device default; a bare `west build` (no run.sh, no
    # -DOEVM_BLOB) falls back to the committed TimerNode image so it just works.
    # run.sh bakes a fresh <node>.oimg and passes -DOEVM_BLOB, overriding this.
    set(OEVM_BLOB ${CMAKE_CURRENT_SOURCE_DIR}/TimerNode.oimg)
    message(STATUS "oevm: OEVM_BLOB defaulted to ${OEVM_BLOB}")
  endif()
  if(NOT EXISTS "${OEVM_BLOB}")
    message(FATAL_ERROR "OEVM_BLOB does not exist: ${OEVM_BLOB}")
  endif()
  target_sources(app PRIVATE
    ${OEVM_SRC}/src/oimg_loader.c
    ${OEVM_SRC}/src/kernel_boot.c
    ${OEVM_SRC}/src/hanoi_native.c
    ${CMAKE_CURRENT_SOURCE_DIR}/blob.S)
  # OEVM_NO_FILE_IO compiles out oimg_loader.c's oimg_load() mmap/open path,
  # which has no meaning on this target.
  target_compile_definitions(app PRIVATE OEVM_MODEL_B OEVM_NO_FILE_IO)
  # blob.S is preprocessed (.S, capital) so OEVM_BLOB_FILE expands.
  set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S APPEND PROPERTY
    COMPILE_DEFINITIONS "OEVM_BLOB_FILE=\"${OEVM_BLOB}\"")
  # Rebuild when the blob changes.
  set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/blob.S APPEND PROPERTY
    OBJECT_DEPENDS "${OEVM_BLOB}")
else()
  message(FATAL_ERROR "OEVM_MODEL must be A or B, got: ${OEVM_MODEL}")
endif()
message(STATUS "oevm: OEVM_MODEL=${OEVM_MODEL}")

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

# The boxed-Float arena (gc.c) mallocs slabs of OEVM_FLOAT_ARENA_SLAB_CELLS floats
# (default 4096 ≈ 64 KB/slab — sized for a host's big libc heap). On the DK the
# newlib k_malloc pool is a few tens of KB, so a float-heavy program (e.g.
# perfstex's floatMath) must shrink the slab and bound the slab count via env.
if(DEFINED ENV{OEVM_FLOAT_ARENA_SLAB_CELLS})
  target_compile_definitions(app PRIVATE OEVM_FLOAT_ARENA_SLAB_CELLS=$ENV{OEVM_FLOAT_ARENA_SLAB_CELLS})
endif()
if(DEFINED ENV{OEVM_FLOAT_ARENA_SLAB_BUDGET})
  target_compile_definitions(app PRIVATE OEVM_FLOAT_ARENA_SLAB_BUDGET=$ENV{OEVM_FLOAT_ARENA_SLAB_BUDGET})
endif()

# The VM execution stack (vm.c) is a fixed heap Stack object of VM_STACK_INIT_WORDS
# oops, allocated at boot INSIDE the OEVM_HEAP_SIZE arena — the single largest boot
# allocation (default 16384 words = 64 KB on the 32-bit DK). Shrinking it lowers the
# arena floor, so OEVM_HEAP_SIZE can drop too. OEVM_MARK_STACK_SIZE (gc.c) is the GC
# mark worklist. Both are embedded RAM knobs; passed via env for the same NCS-sysbuild
# reason as the arena/cache defines above.
if(DEFINED ENV{VM_STACK_INIT_WORDS})
  target_compile_definitions(app PRIVATE VM_STACK_INIT_WORDS=$ENV{VM_STACK_INIT_WORDS})
endif()
if(DEFINED ENV{OEVM_MARK_STACK_SIZE})
  target_compile_definitions(app PRIVATE OEVM_MARK_STACK_SIZE=$ENV{OEVM_MARK_STACK_SIZE})
endif()

# shipsBellsV3 §11.2 bench instrumentation: a single strike-solenoid pulse of N ms
# fired from C at boot (src/main.c solenoid_oneshot). ABSENT BY DEFAULT and env-gated
# rather than Kconfig-gated so that the only way to build an image that energises a
# coil is to say so on the command line, in the same breath as the width:
#
#   OEVM_SOLENOID_PULSE_MS=15 ./run.sh --board esp32c6 --flash ...
#
# Walking §11.2's ladder (100 -> 50 -> 30 -> 25 -> 20 -> 15 -> 10 ms) is then one env
# var per rebuild. main.c BUILD_ASSERTs the value against §8's 1 s on-time cap.
if(DEFINED ENV{OEVM_SOLENOID_PULSE_MS})
  target_compile_definitions(app PRIVATE OEVM_SOLENOID_PULSE_MS=$ENV{OEVM_SOLENOID_PULSE_MS})
endif()

# Strike on demand from the console (src/main.c strike_console): type <ms><Enter>.
# Separate knob from OEVM_SOLENOID_PULSE_MS on purpose — walking §11.2's ladder by
# ear wants NO boot strike, so that every ring heard is one somebody typed.
#
#   OEVM_SOLENOID_CONSOLE=1 ./run.sh --board esp32c6 --flash ...
if(DEFINED ENV{OEVM_SOLENOID_CONSOLE})
  target_compile_definitions(app PRIVATE OEVM_SOLENOID_CONSOLE=1)
endif()

# The NTP server the bell corrects itself against (src/main.c ntp_target_sync).
#
# OEVM_NTP_SERVER6 is a LITERAL IPv6 address and WINS over OEVM_NTP_SERVER when
# both are set — it is the Thread path, where the mesh is a ULA with no global
# IPv6 and no DNS64, so a hostname has nothing to resolve against. Env rather than
# Kconfig for the same NCS-sysbuild reason as every knob above.
#
#   OEVM_NTP_SERVER6=fd77:9957:d9f3:1:2935:a821:1d56:c329 ./run.sh --board esp32c6 ...
if(DEFINED ENV{OEVM_NTP_SERVER})
  target_compile_definitions(app PRIVATE OEVM_NTP_SERVER="$ENV{OEVM_NTP_SERVER}")
endif()
if(DEFINED ENV{OEVM_NTP_SERVER6})
  target_compile_definitions(app PRIVATE OEVM_NTP_SERVER6="$ENV{OEVM_NTP_SERVER6}")
endif()

# THE REMOTE CONSOLE (src/log_udp.c). A LITERAL IPv6 address, for the same reason
# as OEVM_NTP_SERVER6 above — the mesh has nothing to resolve a name against. Set
# it and printk is TEED to one UDP datagram per line; leave it unset and the image
# says so at boot rather than looking like an unreachable gateway.
#
#   OEVM_LOG_UDP6=fdc5:9109:9afe:97ea:9541:d963:f8d9:8567 ./run.sh --board esp32c6 ...
#
# OEVM_LOG_UDP_PORT is optional (default 54330). It is deliberately NOT any of the
# ports the soak dashboard scores (:54360/:54361) or the gw echo target (:54331).
if(DEFINED ENV{OEVM_LOG_UDP6})
  target_compile_definitions(app PRIVATE OEVM_LOG_UDP6="$ENV{OEVM_LOG_UDP6}")
endif()
if(DEFINED ENV{OEVM_LOG_UDP_PORT})
  target_compile_definitions(app PRIVATE OEVM_LOG_UDP_PORT=$ENV{OEVM_LOG_UDP_PORT})
endif()

# Hot-path IRAM placement. Routes the interpreter's hottest functions
# (oevm.h OEVM_HOT) into internal IRAM instead of flash XIP; ends the
# combinator-row cache roulette (ledger E8: ocDo −36%, ~8 KB IRAM).
# Xtensa-only: -mtext-section-literals keeps literal pools in the relocated
# section (an L32R in IRAM cannot reach flash). DEFAULT ON for Xtensa
# (accepted 2026-07-17); export OEVM_IRAM_HOT=0 to opt out. Env, not -D,
# for the same NCS-sysbuild reason as the defines above.
if(CONFIG_XTENSA AND NOT "$ENV{OEVM_IRAM_HOT}" STREQUAL "0")
  target_compile_definitions(app PRIVATE OEVM_HOT_SECTION_IRAM=1)
  target_compile_options(app PRIVATE -mtext-section-literals)
elseif(NOT CONFIG_XTENSA AND "$ENV{OEVM_IRAM_HOT}" STREQUAL "1")
  message(FATAL_ERROR "OEVM_IRAM_HOT is an ESP32/Xtensa knob; unset it for this target")
endif()

# Experiment (2026-07-17): OEOM native-substitution Hanoi spike. OFF by default.
# export OEVM_HANOI_NATIVE=1 to build the native hanoiMove: PrimitiveClosure path
# (src/hanoi_native.c + the strcmp gate in dispatch.c). See
# docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md.
if("$ENV{OEVM_HANOI_NATIVE}" STREQUAL "1")
  target_compile_definitions(app PRIVATE OEVM_HANOI_NATIVE=1)
endif()

# Spike (2026-08-06, #164): exact per-method call counts at push_frame. OFF by
# default — the table is ~2 KB of .bss, which the ESP32's 816 B of dram0 spare
# (#53) cannot carry and the C6 can. export OEVM_METHOD_PROFILE=1 to build it in.
# OEVM_MPROF_RUNTIME_GATE + OEVM_MPROF_FLIP_AT/DUMP_AT are the MEASUREMENT seam:
# both arms in one binary, flipped at an activation count, because cross-binary
# timing cannot resolve a hook this small. Scaffolding, not a feature.
if("$ENV{OEVM_METHOD_PROFILE}" STREQUAL "1")
  target_compile_definitions(app PRIVATE OEVM_METHOD_PROFILE=1)
  # Table capacity, settable because 256 is NOT enough for every app. A steady
  # bridging window on PortaBridgeNodeC6 reaches 223 methods of 256 (load 0.87)
  # and saturates; at saturation every probe walks the whole table. See
  # docs/results/2026-08-12-allocation-attribution-c6.md and rete#166.
  if(NOT "$ENV{OEVM_MPROF_SLOTS}" STREQUAL "")
    target_compile_definitions(app PRIVATE OEVM_MPROF_SLOTS=$ENV{OEVM_MPROF_SLOTS})
  endif()
  if("$ENV{OEVM_MPROF_RUNTIME_GATE}" STREQUAL "1")
    target_compile_definitions(app PRIVATE OEVM_MPROF_RUNTIME_GATE=1)
    if(NOT "$ENV{OEVM_MPROF_FLIP_AT}" STREQUAL "")
      target_compile_definitions(app PRIVATE OEVM_MPROF_FLIP_AT=$ENV{OEVM_MPROF_FLIP_AT})
    endif()
    if(NOT "$ENV{OEVM_MPROF_DUMP_AT}" STREQUAL "")
      target_compile_definitions(app PRIVATE OEVM_MPROF_DUMP_AT=$ENV{OEVM_MPROF_DUMP_AT})
    endif()
  endif()
endif()

# Production default (accepted 2026-07-17): the three per-bytecode debugger
# checks (oevm.h OEVM_NO_DEBUG_HOOKS) are compiled OUT of Zephyr builds —
# no on-target debugger transport exists, and the checks cost 10-16% per
# bytecode (ledger E8). Export OEVM_DEBUG_HOOKS=1 to build a hooks-enabled
# image (required for any future on-target debugging work). Hosted builds
# and the test suites are unaffected — they never define the macro, and the
# debugger-integration gate exercises the hooks there.
if(NOT "$ENV{OEVM_DEBUG_HOOKS}" STREQUAL "1")
  target_compile_definitions(app PRIVATE OEVM_NO_DEBUG_HOOKS=1)
endif()

# Config-D DEFAULTS for every nRF52840 target — LOAD-BEARING, not tuning. The env
# vars above still override, but when the env is silent a 52840 build must NOT fall
# back to the host defaults: without VM_STACK_INIT_WORDS=4096 the VM boots by
# allocating its default 16384-word (64 KB) execution stack inside the 48 KB arena —
# a guaranteed boot OOM. That is precisely how the dongle shipped dead through
# rungs 7-8: built from a shell without the exports, it aborted at boot while
# OpenThread kept it "attached" (see zephyr/docs/BOARD_PORTS.md and
# review/ISSUE-dongle-usb-openthread.md). native_sim/host builds are untouched
# (CONFIG_SOC_NRF52840 is false there, and their 16 MB default arena is fine).
if(CONFIG_SOC_NRF52840)
  if(NOT DEFINED ENV{OEVM_HEAP_SIZE})
    target_compile_definitions(app PRIVATE OEVM_HEAP_SIZE=49152)
  endif()
  if(NOT DEFINED ENV{METHOD_CACHE_SIZE})
    target_compile_definitions(app PRIVATE METHOD_CACHE_SIZE=256)
  endif()
  if(NOT DEFINED ENV{VM_STACK_INIT_WORDS})
    target_compile_definitions(app PRIVATE VM_STACK_INIT_WORDS=4096)
  endif()
  if(NOT DEFINED ENV{OEVM_MARK_STACK_SIZE})
    target_compile_definitions(app PRIVATE OEVM_MARK_STACK_SIZE=512)
  endif()
endif()
