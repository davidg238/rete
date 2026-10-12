#!/usr/bin/env bash
# rete#37 send cost on c6spare: build blockcost.nsl at REV ($1) as tag $2, OEVM_IRAM_HOT=$3.
set -u
source ~/zephyr-esp32/.venv/bin/activate; export ZEPHYR_BASE=$HOME/zephyr-esp32/zephyr
mkdir -p ~/37-c6; cd ~/rete-37c6 && git checkout -q -f "$1" && git log --oneline -1 | cut -c1-60
cd nodus/oevm/zephyr
env OEVM_IRAM_HOT=$3 bash run.sh --board esp32c6 --node ../../tests/00_perf/blockcost.nsl --build-dir build-37-c6 --pristine > ~/37-c6/$2.build.log 2>&1 \
  || { echo BUILD FAIL; tail -30 ~/37-c6/$2.build.log; exit 1; }
cp build-37-c6/zephyr/zephyr.bin ~/37-c6/$2.bin; cp ../../tests/00_perf/blockcost.oimg ~/37-c6/$2.oimg
grep -E "IRAM|hot path|FLASH:|program_a" ~/37-c6/$2.build.log | tail -5
echo BUILT $2
