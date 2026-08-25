# 14_cbor — CBOR encode both-path golden tests (Rung 3)

Differential (Path A vs Path B) goldens for the nsl `Cbor` codec (`lib/kernel.ns`).
`platform Cbor encode: aMessage` walks the message algebra to canonical CBOR and
answers a `Bytes`; `Bytes printHex` renders it as lowercase hex (no separators),
which is what these tests assert against a golden authored from an **independent**
oracle.

## Oracle

`cbor2` 6.1.3 in a venv (system `python3` has no `cbor2`). Goldens are authored
from the oracle, never back-derived from the nsl encoder (that would be
tautological). The algebra→CBOR mapping the oracle mirrors (design spec §4):

| algebra | CBOR |
|---|---|
| Integer | major 0/1, shortest form |
| Float | major 7, shortest of f16/f32/f64 that round-trips |
| String | major 3 (text) |
| Bytes | major 2 |
| Symbol | **tag 39** wrapping major-3 UTF-8 text — cbor2 `CBORTag(39, "name")` |
| List | major 4 array |
| Map | major 5 map, keys in RFC 8949 §4.2 bytewise order over encoded key bytes |
| Record | **tag 27** wrapping a 2-element array `[tagSymbol(tag39), fieldsMap]` |
| nil / true / false | `0xf6` / `0xf5` / `0xf4` |

## Map key order (the load-bearing rule)

RFC 8949 §4.2: keys are ordered **bytewise over their ENCODED key bytes**, NOT
length-first and NOT the algebra Map's stored `valueCompare:` order. cbor2's
`canonical=True` was not blindly trusted — for every fixture it was VERIFIED equal
to an explicit sort that encodes each key, then sorts `(encodedKeyBytes,
encodedValBytes)` pairs bytewise by `encodedKeyBytes` and concatenates. Both
fixtures below: **cbor2 canonical == explicit bytewise sort** (so canonical was
used, and it is provably the RFC rule).

## Reproduce the goldens

```python
# /tmp/.../scratchpad/cbor-oracle/bin/python
from cbor2 import CBORTag, dumps
def sym(s): return CBORTag(39, s)

def bytewise_map(pairs):
    enc = [(dumps(k), dumps(v)) for (k, v) in pairs]
    enc.sort(key=lambda p: p[0])            # RFC 8949 bytewise over encoded key
    head = bytes([0xa0 | len(enc)])          # major-5 map, n<24 entries
    return head + b''.join(kb + vb for kb, vb in enc)

# 02_golden_hex.ns : {#n: 42, #s: 'hi'}
p = [(sym('n'), 42), (sym('s'), 'hi')]
assert dumps({sym('n'):42, sym('s'):'hi'}, canonical=True) == bytewise_map(p)
# -> a2d827616e182ad8276173626869

# 03_map_order.ns : {#z: 1, #aa: 2}
p = [(sym('z'), 1), (sym('aa'), 2)]
assert dumps({sym('z'):1, sym('aa'):2}, canonical=True) == bytewise_map(p)
# -> a2d827617a01d82762616102
```

### 02_golden_hex.ns → `a2d827616e182ad8276173626869`

`a2` map(2) · `d827616e` #n (tag39,'n') · `182a` 42 · `d8276173` #s · `626869` 'hi'.
Keys sort n (`…616e`) < s (`…6173`).

### 03_map_order.ns → `a2d827617a01d82762616102`

`a2` map(2) · `d827617a` #z (tag39,'z') · `01` 1 · `d827626161` #aa (tag39,'aa') · `02` 2.

Divergence test: the algebra Map stores keys in `valueCompare:` order (over
printString `#z` vs `#aa`), which puts **#aa before #z**. But bytewise over the
encoded keys, `#z` (`d827617a`) < `#aa` (`d827626161`) because they differ in
encoded length (1-byte vs 2-byte text head), so the encoder must **re-sort at
emit time** and emit **#z first**. This golden fails if the walk emits in the
Map's stored order.

## Running

```bash
cd /home/david/workspace/nsl-tuvm/tools
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB python3 - <<'PY'
from pathlib import Path; import run_tests_oevm as R
for f in ['02_golden_hex.ns','03_map_order.ns']:
    p=Path('/home/david/workspace/nsl-tests/14_cbor/'+f)
    print(f, 'A', R.run_test_file(p)[0], 'B', R.run_test_file_oimg(p)[0])
PY
```

Path A regenerates `oevm/src/solidified_image.c` + `.stmap` from `kernel.ns`;
restore them (`git -C ../oevm checkout -- src/solidified_image.c
src/solidified_image.stmap`) before committing oevm.
