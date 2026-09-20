cmake_minimum_required(VERSION 3.20.0)
# Out-of-tree boards under zephyr/boards/<vendor>/<board>/ — the Adafruit Feather
# RP2040 (rete#302's rehearsal board, absent from Zephyr v4.2). Must precede
# find_package(Zephyr); the same directory keeps holding the per-board .overlay
# files, which Zephyr discovers by a different rule.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(oevm_layoutdump)
# tools/dump_layout.c is `int main(void)` and links nothing — it only prints
# sizeof/offsetof for the TARGET ABI. Compiling it as the Zephyr app IS the test.
#
# Deliberately does NOT plumb the five OEVM_* env knobs that zephyr/CMakeLists.txt
# forwards: they are all `if(DEFINED ENV{...})` there, and dump_layout.c needs only
# oevm.h's struct declarations to compute sizeof/offsetof. Sizes and offsets do not
# depend on the heap/cache tunables, so a bare compile yields the same layout.
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/dump_layout.c)
# Beside dump_layout.c, never inside it: that file is shared with the host gate.
# src/console_prep.c unbuffers stdout and prints a printk marker — see its header.
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/console_prep.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
