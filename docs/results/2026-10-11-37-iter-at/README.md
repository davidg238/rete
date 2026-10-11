# rete#37 — `OP_ITER_AT` on the WeAct: transcripts (2026-10-11)

The record is `nodus/oevm/docs/optimizations.md` Opt #7; the design is
`docs/design/2026-10-11-37-iter-at-design.md`.

- Image: branch `37-iter-at` at `3cb7d8f`, plus the spike's bench node (`Spike296Node.nsl`, `main.c`,
  `CMakeLists.txt`, `run.sh` from `37-ocdo-spike`, uncommitted, hence the `-dirty` build id), built on msi-7d43 by
  `~/37-iter-build.sh` with `OEVM_SPIKE_296=1`, `rpi_pico2`, `-O2`, USB console, **no image patch**.
- Driver: `docs/spikes/2026-10-10-296-image-in-sram/run_412.sh`, three boots, blob-in-flash arm
  (`logs.driver.log`, `logs/weact-37-iter-flash-{1,2,3}.log`).
- The rows `ocDoLowered` and `ocDoFused` are the spike's rows **unpatched**: six plain `oc do:` calls per outer
  iteration, so 32548 / 6 ≈ 5425 ms per `do:`, agreeing with `ocDo` (5594). They carry no other meaning here.
