# rete#199 — a node whose frames the gateway refuses, reproduced off hardware

Two artifacts, and a result that contradicts the hypothesis they were built to test.

- **`repro199-m32.sh`** — the reproduction, two legs. A post-#197 node against a pre-#197
  gateway (every frame refused with `missing #bootId`), and the same node against a matched
  gateway as the control. The node VM is built **`-m32` at the C6's own four knobs**, which
  makes the arena byte-for-byte and object-for-object the device's; a 64-bit host cannot host
  the device arena at all (see `docs/results/2026-08-11-host-arena-floor.md`).

  Supersedes an earlier `repro199.sh` whose result was retracted: it set `OEVM_HEAP_SIZE` as
  an environment variable, and that is a compile-time macro (`gc.c:18-19`), so it measured
  the 16 MB host default while reporting itself as a measurement of the device.

- **`oomprobe.c`** — a scratch translation unit that rides two seams the VM already exports
  (`oevm_millis`, `oevm_oom_handler`) to dump the arena's state and the nsl frame chain at
  the fatal allocation. Link it after `oevm_image_main.c`. It edits no tracked file, which
  is the point: the diagnostic leaves no residue to forget to revert.

## Result (2026-08-11)

| | board (`c6spare`) | host `-m32`, mismatched | host `-m32`, control |
|---|---|---|---|
| check-ins | 46 | ~55 | **450** |
| duration | 157 s | **114 s** | 900 s |
| outcome | OOM, halted | **OOM, aborted** | **alive** |
| fatal request | 4104 B | **4104 B** | — |

At the fatal allocation: `live=44396  largest_free=3976  gc_cycles=428` in a 131 072 B arena.

**The node dies with two-thirds of its arena free.** It is not a leak and not an unbounded
backlog — it cannot find 4104 contiguous bytes. The healthy control holds *more* live data
(78 132 B) at a 19× lower collection rate.

The 4104 B is `ByteBuilder>>grow` asking for a 4096 B byte buffer, so it is a **constant**,
not a collection-doubling boundary — which is why three independent events requested exactly
the same figure. The churn that fragments the arena is `pushFrame`'s trim loop re-encoding
the frame once per removed row, ~80 times per check-in, once `acked` freezes.

Full write-up: rete#199, and the corrected §11.6 addendum in
`docs/design/2026-08-10-node-memory-and-liveness-design.md`.
