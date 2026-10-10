# What would AOT-ing `Cbor>>encode:` return? — spike, 2026-10-09

*Throwaway spike for rete#395 (an aside off #380, extending the parked #45). Code and
both run logs: `docs/spikes/2026-10-09-cbor-aot/`. Nothing here is in the mainline
build; the hooks live in `hooks.patch`.*

## The question

Rather than write a second CBOR encoder in C, could the nsl one in
`nodus/nsl/lib/kernel.nsl` be compiled ahead of time — a larger method that sends to
other methods, not a Hanoi leaf — and what would that return against the C encoder?

## Method

Two native `encode:` substitutions, installed the way the Hanoi spike installed its
(`src/hanoi_native.c`: a firmware-static `PrimitiveClosure` bound to the interned
selector), timed against the nsl encoder **in the one binary**, host and c6spare, min of
20 samples, bytes asserted identical to the nsl encoder on every payload first:

| arm | what is native | what stays a `send()` back into the VM |
|---|---|---|
| **B — feedback AOT** | byte emission, String/Symbol bytes, the builtin ranks (what a compiler knows after a class guard on a kernel C type) | every send to an nsl-defined receiver: `valueRank`, `size`, `at:`, `keyAt:`, `valueAt:`, `tag`, `fields`, `byteSize`, `u8At:`. No nsl method inlined. |
| **C — ceiling** | the whole walk, reading List/Map/Record/Bytes slots | nothing. This *is* the C encoder. |
| **A — mechanical AOT, no feedback** | not built: priced from the 2026-08-09 activation counts | everything |

Payloads are the 2026-08-09 bench's: `str256` (per byte), `ints64` (per element),
`mapNest4` (containers and the key sort), and `batch`, the real 425 B push frame.

## The numbers

Per encode, c6spare (ESP32-C6), build `porta-v1-final-1524-g08d2fce-dirty`:

| payload | nsl | arm B | arm C | B vs nsl | C vs nsl |
|---|---:|---:|---:|---:|---:|
| str256, 259 B | 11.97 ms | 32.8 µs | 32.8 µs | 365× | 365× |
| ints64, 66 B | 6.93 ms | 1.54 ms | 48.6 µs | 4.5× | 143× |
| mapNest4, 279 B | 15.33 ms | 204 µs | 48.2 µs | 75× | 318× |
| **batch, 425 B** | **44.65 ms** | **1.21 ms** | **160 µs** | **37×** | **280×** |

Host (one laptop, same ranking): batch 285 µs → 7.3 µs (B) → 1.28 µs (C).

Byte identity: `armB=true armC=true` on all four, both machines.

## What the numbers say

- **A kept send costs what an activation costs.** `ints64` isolates it: arm B differs
  from arm C by 64 `at:` sends and 1.49 ms, so a re-entrant `send()` from native code
  costs **~23 µs on the C6** — the same 23 µs/activation #163 measured for the
  interpreter. The whole gap between B and C on `batch` (1.05 ms) is ~45 sends. AOT
  removes the compiled method's *own* frames; a send it keeps pays full price.
- **Arm A, mechanical AOT with every send kept, is worth at most ~1.5×.** The encoder's
  own methods and blocks are 443 of `batch`'s 2,325 activations in the top-12 rows of
  the 2026-08-09 counts, and at most 802 counting every row below the cut (19–34 %).
  Without type feedback nothing else goes away.
- **Feedback AOT gets the value.** 44.6 ms → 1.2 ms takes encode from the dominant
  term of a check-in to noise; the remaining 7.6× to the C encoder is the per-send cost
  above and is reachable only by inlining through nsl methods (`List>>at:` →
  `Array>>at:` → `basicAt:`), i.e. small-method inlining behind class guards. That is
  the compiler #45 would be.
- **So: yes, AOT the nsl encoder rather than ship a C one** — one source of truth, the
  goldens stay the guard — *provided* the compiler has send-site receiver classes to
  guard on. The per-method call counter collects counts, not classes; that is the
  instrument to build first.

## Costs and limits of the spike

- Static 2 KB output buffer, 32 keys per map, no Float leaf (the bench carries none).
- Arm B allocates nothing until the final `Bytes`, so the **rooting cost of a native
  method that allocates across a send is not measured here**. The one oop held across
  an allocation uses `gc_temp_root_push`, the boot driver's 16-slot precise set.
- A nested-class instance (List, Map, Record, Bytes live inside `Kernel`) keeps its
  `outer` in slot 0; the declared slots follow (`dispatch.c` `run_method_on`). The
  first build read `items` from slot 0 and DNU'd on `isValueObject:`; any future native
  code over kernel objects needs the same `+1`.
- The C6 figure for nsl `batch` is 44.6 ms here against 53.6 ms in the 2026-08-09
  record: a different image, two months of kernel changes, and the within-binary ratios
  are the claim, not the absolute.
- Host: the lost `mprof_resolve.py` (only in an old worktree) means no activation
  counts were taken this time; the arm-A bound reuses the August counts.

## Record

- Issue: rete#395 (results comment), rete#45 (the AOT phase this informs).
- Lineage: `docs/design/2026-07-17-oeom-native-substitution-hanoi-spike-design.md`,
  `docs/results/2026-08-09-cbor-encode-shape.md`.
