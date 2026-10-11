#!/usr/bin/env bash
# rete#37 STIC on c6spare (ESP32-C6): build performance.nsl at REV ($1) as tag $2, run.sh's C6 defaults (no radio).
set -u
source ~/zephyr-esp32/.venv/bin/activate; export ZEPHYR_BASE=$HOME/zephyr-esp32/zephyr
mkdir -p ~/37-c6; cd ~/rete-37c6 && git checkout -q -f "$1" && git log --oneline -1 | cut -c1-60
cd nodus/oevm/zephyr
bash run.sh --board esp32c6 --node ../../tests/00_perf/performance.nsl --build-dir build-37-c6 --pristine > ~/37-c6/$2.build.log 2>&1 \
  || { echo BUILD FAIL; tail -30 ~/37-c6/$2.build.log; exit 1; }
cp build-37-c6/zephyr/zephyr.bin ~/37-c6/$2.bin; cp ../../tests/00_perf/performance.oimg ~/37-c6/$2.oimg
grep -E "DLRAM|ILRAM|FLASH:|RAM:|\[blob\]|build-id|HEAP|PROG|program_a" ~/37-c6/$2.build.log | tail -8
echo BUILT $2
