source ~/zephyr-esp32/.venv/bin/activate; export ZEPHYR_BASE=$HOME/zephyr-esp32/zephyr
cd ~/rete-reflash/nodus/oevm/zephyr && git log --oneline -1 | cut -c1-50
env OEVM_HEAP_SIZE=98304 METHOD_CACHE_SIZE=256 VM_STACK_INIT_WORDS=4096 OEVM_MARK_STACK_SIZE=512 OEVM_FLOAT_ARENA_SLAB_CELLS=128 OEVM_FLOAT_ARENA_SLAB_BUDGET=4 \
  bash run.sh --board esp32 --node ../../tests/00_perf/performance.nsl --conf-extra 37-esp32-nowifi.conf --build-dir build-esp32-wroom --pristine; echo "ESP32-RC $?"
