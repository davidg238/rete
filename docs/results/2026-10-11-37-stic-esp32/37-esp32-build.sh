#!/usr/bin/env bash
# rete#37 STIC on the plain ESP32: build performance.nsl at REV ($1) as tag $2, WiFi off, 96 KB arena (E4)
set -u
source ~/zephyr-esp32/.venv/bin/activate; export ZEPHYR_BASE=$HOME/zephyr-esp32/zephyr
mkdir -p ~/37-esp32; cd ~/rete-37 && git checkout -q -f "$1" && git log --oneline -1 | cut -c1-60
cp ~/layout-esp32.json nodus/oevm/zephyr/layout-esp32.json
cp ~/37-esp32-nowifi.conf nodus/oevm/zephyr/37-esp32-nowifi.conf
cd nodus/oevm/zephyr
env OEVM_HEAP_SIZE=98304 METHOD_CACHE_SIZE=256 PRIM_CACHE_SIZE=256 VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512 OEVM_FLOAT_ARENA_SLAB_CELLS=128 OEVM_FLOAT_ARENA_SLAB_BUDGET=4 bash run.sh --board esp32 --node ../../tests/00_perf/performance.nsl \
  --conf-extra 37-esp32-nowifi.conf --build-dir build-37-esp32 --pristine > ~/37-esp32/$2.build.log 2>&1 \
  || { echo BUILD FAIL; tail -30 ~/37-esp32/$2.build.log; exit 1; }
rm -f 37-esp32-nowifi.conf; git -C ~/rete-37 checkout -q -- nodus/oevm/zephyr/layout-esp32.json
cp build-37-esp32/zephyr/zephyr.bin ~/37-esp32/$2.bin
grep -E "DRAM|IRAM|FLASH:|\[blob\]|build-id|HEAP" ~/37-esp32/$2.build.log | tail -6
echo BUILT $2
