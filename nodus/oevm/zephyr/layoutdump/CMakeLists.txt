cmake_minimum_required(VERSION 3.20.0)
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
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
