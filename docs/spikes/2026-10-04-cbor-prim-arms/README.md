# #297 spike — where should Cbor>>encode: live? (2026-10-04, host, THROWAWAY)

Arms, one binary, same payloads, all byte-identical: **N** today's nsl encoder; **B** nsl walk
+ C leaf prims; **A** whole walk in C (C compares against the four value classes);
**A′** (`A2` in `report.txt`) the A walk with OEOM-style dispatch, one `lookup_method_cached`
per container. `Z` is the empty loop. ns/encode, min of 20 samples: `report.txt`.

Result: B ÷ A = 45× on the 425 B `batch` (the walk's sends are the cost, not the bytes);
A′ ÷ A = 1.04–1.07 on `batch`, ≤1.12 on `map8`. Decision → design
`docs/design/2026-10-04-297-cbor-native-walk-design.md`.

The VM/kernel half (`nodus/oevm/src/cbor_spike.inc`, five spike prims, `CborB` in
`kernel.nsl`) is committed ONLY on branch `297-cbor-spike`; `run.sh` needs that branch.
`TwoTrues.nsl` is the #330 repro found here.
