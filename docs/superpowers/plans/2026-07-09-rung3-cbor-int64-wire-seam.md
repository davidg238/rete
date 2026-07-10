# Rung 3 — CBOR + Int64 Wire Seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serialise/deserialise the message algebra to canonical CBOR bytes via an nsl `Cbor`
walk backed by six C leaf byte-primitives, reachable as `Cbor encode:` / `Cbor decode:ifInvalid:`
and proven on the differential (Path A vs Path B) harness.

**Architecture:** Recursive descent lives in an nsl `Cbor` class (module sibling of
`List`/`Map`/`Record` in `kernel.ns`); it drops to small pure C leaf prims only for the byte
work that needs C and must be byte-identical across platforms — integer↔major-0/1, float↔
shortest f16/f32/f64, CBOR head read, and UTF-8 validation. This is decision **(B)** in the
spec. `zcbor`/`cbor2` are external test oracles only, never linked.

**Tech Stack:** C (tuvm VM: `dispatch.c`/`platform.c`/`prim_ids.h`, `run_c_tests` harness),
Newspeak/nsl (`nsl-tuvm/lib/kernel.ns`), Python fixture authoring (`cbor2`), the
`run_tests_oevm.py` both-path differential harness.

**Spec:** `nsl-tuvm/docs/superpowers/specs/2026-07-09-rung3-cbor-int64-wire-seam-design.md`

## Global Constraints

- **One codec, both platforms.** No `zcbor` linked into any VM; nsl walk + shared C leaves
  give host/target byte-identity by construction.
- **Prim ids append-only.** Last existing prim is `PRIM_pf_pumpEvents = 96` (`prim_ids.h:98`,
  `dispatch.c` PRIM_NAMES). New leaves take ids `97…102` in `PRIM_NAMES` order; each id = its
  1-based position in `PRIM_NAMES`; add the matching `#define` to `prim_ids.h`; `builtins.def`
  untouched. Re-count, never compute.
- **Deterministic encoding (RFC 8949 §4.2):** shortest-form integers, definite lengths only,
  **map keys sorted bytewise by their encoded key bytes** (emit-time re-sort — `Map`'s
  `valueCompare:` order is NOT bytewise).
- **Decode is total & closed:** any byte sequence → one valid message or exactly one error
  routed to the `ifInvalid:` block. Reject indefinite lengths, unknown tags (≠27/39; bignum
  2/3 → `integer out of range`), duplicate map keys, depth > 32, invalid UTF-8, non-value CBOR
  (`undefined`), and the integer `−2^64`. Error strings are the errors.md vocabulary:
  `not a value`, `message depth exceeded`, `invalid utf-8`, `integer out of range`.
- **Integer contract = ratified L7 `±(2^64−1)`** (NOT the stale `cbor.md`/`errors.md` `2^63−1`):
  major 0 = `0…2^64−1`, major 1 = `−1…−2^64`; we hold `±(2^64−1)`, so only `−2^64` overflows.
  Read through both SMI and `LargeInteger`, value-keyed, shortest-form.
- **No float↔int coercion** either direction (`1.0 ≠ 1`).
- **Depth bound = 32**, enforced in the nsl walk (encode and decode).
- **Both-path discipline (from the SDD ledger):** the single-file both-path recipe CLOBBERS
  `tuvm/src/solidified_image.c` + `.stmap` — ALWAYS restore before commit:
  `git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap`
- **Controller owns full-suite runs;** implementers run single-file both-path FOREGROUND only.

---

## File Structure

- **`tuvm/src/dispatch.c`** — append 6 leaf prim enum entries + `PRIM_NAMES` names (ids 97–102).
  Leaf bodies for the pure ones may live here or in `platform.c`; put them in `platform.c`
  beside `PRIM_pf_string`/`basicNewBytes:` to keep `platform_dispatch` cohesive **and avoid a
  new `.c` file** (which would need adding to ≥8 build lists).
- **`tuvm/src/platform.c`** — 6 new `platform_dispatch` cases (the leaf implementations).
- **`tuvm/src/prim_ids.h`** — 6 new `#define PRIM_pf_cbor* 97…102` mirrors.
- **`tuvm/src/oevm.h`** — (only if a shared helper prototype is needed; prefer none.)
- **`tuvm/tests/test_cbor.c`** (NEW C unit-test file) — leaf-level golden/reject tests;
  register in `run_c_tests` ENG_COMMON like `test_handles`/`test_pulse`. (This is a *test*
  file, added to the test harness list, not the VM build lists.)
- **`nsl-tuvm/lib/kernel.ns`** — new `Cbor` class (module sibling of `List`/`Map`/`Record`);
  encode walk + decode walk; two doc corrections are separate (review/lib).
- **`nsl-tests/14_cbor/`** (NEW) — `01_roundtrip.ns`, `02_golden_hex.ns`, `03_map_order.ns`,
  `04_reject.ns`, `05_int_band.ns` — differential both-path.
- **`tuvm/review/lib/cbor.md`**, **`tuvm/review/lib/errors.md`** — doc reconciliation (Task 7).
- **`tuvm/review/program-sequence.md`** — mark rung-3 done (Task 7).
- **`tools/oimg_serialize.py`**, **`tools/ns_compile_oevm.py`** — `.oimg` large-SMI fix (Task 1).

**Interfaces produced (used across tasks):**
- C leaves (all args are oops; return via `*out`):
  - id 97 `cborEncodeInt:` — `anInteger(SMI|LargeInteger)` → RawBytes (major 0/1 shortest).
  - id 98 `cborEncodeFloat:` — `aFloat(Float)` → RawBytes (major 7 shortest f16/f32/f64).
  - id 99 `cborReadHead:at:` — `(aBytes, indexSMI)` → 3-elt Array `{majorSMI, argInteger, nextSMI}`
    for majors 0–6; **or SMI `-1`** sentinel if the head/arg runs past `aBytes` length.
  - id 100 `cborIntFromMajor:arg:ifOutOfRange:` — `(majorSMI, argInteger, block)` → Integer;
    evaluates `block` for `−2^64`.
  - id 101 `cborReadFloat:at:` — `(aBytes, indexSMI)` (points at a `0xf9/0xfa/0xfb` byte) →
    2-elt Array `{Float, nextSMI}`; SMI `-1` sentinel if the payload runs past length.
  - id 102 `utf8Valid:` — `aBytes` → `#true`/`#false` symbol.
  - **Leaves return *internal RawBytes* fragments** (readable via `basicAt:`/`basicSize`); the
    nsl walk copies their bytes into its accumulator. Only the final `Cbor encode:` result is
    wrapped as an algebra `Bytes` (`Bytes overRaw: frozenRaw`).
- nsl surface (Task 5/6): `Cbor encode: aMessage` → `Bytes`; `Cbor decode: aBytes ifInvalid: aBlock` → message | `aBlock value: errString`.

---

## Task 1: `.oimg` large-SMI-constant sign-corruption fix (independent)

**Files:**
- Modify: `tools/oimg_serialize.py` (`_smi()`, ~L222-226 and the `RefWord.smi` note ~L31)
- Modify: `tools/ns_compile_oevm.py:813` (the `SMI:`/`LARGEINT:` threshold)
- Test: `nsl-tests/06_limit_tests/07_largeint_smi_band.ns` (NEW)

**Interfaces:** Consumes nothing. Produces nothing the codec depends on — pure de-risk so CBOR
fixtures may use literals in `[2^61, 2^62)`.

- [ ] **Step 1: Write the failing both-path test.** Create
  `nsl-tests/06_limit_tests/07_largeint_smi_band.ns`:

```newspeak
(* A literal in [2^61, 2^62): must print identically on Path A (solidified) and
   Path B (.oimg). Pre-fix, Path B corrupts the sign (findings 2026-07-04). *)
| x |
x:: 2305843009213693952.                 "2^61"
Transcript showCr: x printString.
Transcript showCr: (x + x) printString.  "2^62"
```
Expected output file `07_largeint_smi_band.expected`:
```
2305843009213693952
4611686018427387904
```

- [ ] **Step 2: Run both paths, verify Path B FAILS (wrong sign).**

```bash
cd /home/david/workspace/nsl-tuvm/tools
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB python3 - <<'PY'
from pathlib import Path; import run_tests_oevm as R
p=Path('/home/david/workspace/nsl-tests/06_limit_tests/07_largeint_smi_band.ns')
print('A',R.run_test_file(p)); print('B',R.run_test_file_oimg(p))
PY
```
Expected: `A` PASS, `B` FAIL with `-2305843009213693952` (sign corrupted).

- [ ] **Step 3: Fix `tools/oimg_serialize.py` `_smi()`.** Bound the SMI magnitude by the
  ref-word's true capacity (2 tag bits ⇒ `< 2^(oop*8-3)`), keeping the hard guard:

```python
def _smi(self, v):
    # RefWord.smi(v) = (v << 2) | TAG_SMI in an oop-width word => 2 tag bits =>
    # signed capacity |v| < 2^(oop*8-3). The old bound 2^(oop*8-2) was one bit
    # too lenient and silently overflowed onto the sign bit (findings 2026-07-04).
    limit = 1 << (self.oop * 8 - 3)
    if not (-limit <= v < limit):
        raise ValueError(f"SMI constant {v} exceeds oimg ref-word capacity {limit}")
    return (v << 2) | TAG_SMI
```

- [ ] **Step 4: Fix `tools/ns_compile_oevm.py:813`** — lower the `SMI:` threshold to the
  ref-word's true capacity so wider literals emit `LARGEINT:` (which round-trips):

```python
# oop width of the target image (already known to the compiler); 2 tag bits.
smi_limit = 1 << (self.oop_bytes * 8 - 3)
if -smi_limit <= ival < smi_limit:
    emit(f"SMI:{ival}")
else:
    emit(f"LARGEINT:{ival}")
```
(If `self.oop_bytes` is not already available here, derive it the same way the serializer
does — do NOT hard-code `1<<62`.)

- [ ] **Step 5: Run both paths, verify PASS (A==B).** Same command as Step 2.
  Expected: `A` PASS, `B` PASS, both printing the positive values.

- [ ] **Step 6: Restore byproducts + commit.**

```bash
git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
cd /home/david/workspace/nsl-tests && git add 06_limit_tests/07_largeint_smi_band.* && \
  git commit -m "test(rung3): large-SMI-constant band round-trips on both paths"
cd /home/david/workspace/tuvm && git add tools/oimg_serialize.py tools/ns_compile_oevm.py && \
  git commit -m "fix(oimg): correct SMI ref-word tag-capacity bound (tuvm#… large-SMI corruption)"
```
(Note: `tools/` live in the `tuvm` repo. Confirm the path at implementation time; the ledger
recipe uses `/home/david/workspace/nsl-tuvm/tools` for the harness — verify which repo owns
`oimg_serialize.py`/`ns_compile_oevm.py` and commit there.)

---

## Task 2: C leaf `cborEncodeInt:` (prim id 97)

**Files:**
- Modify: `tuvm/src/dispatch.c` (enum `PRIM_pf_cborEncodeInt` after `PRIM_pf_pumpEvents`;
  `PRIM_NAMES` add `"cborEncodeInt:"`)
- Modify: `tuvm/src/prim_ids.h` (`#define PRIM_pf_cborEncodeInt 97`)
- Modify: `tuvm/src/platform.c` (case in `platform_dispatch`)
- Create: `tuvm/tests/test_cbor.c`; register in `run_c_tests` (ENG_COMMON, beside `test_pulse`)

**Interfaces:** Consumes `IS_SMI`/`UNTAG_SMI`, `struct LargeInteger{int64_t sign;uint64_t mag}`,
`LargeIntegerVtable`, `RawBytesVtable`, `struct String{size_t length;uint8_t bytes[]}`,
`gc_allocate`. Produces id 97 (used by Task 5 encode walk).

- [ ] **Step 1: Write the failing C test.** In `tuvm/tests/test_cbor.c`, a helper that builds
  a RawBytes int and asserts its hex against `cbor2`-authored goldens:

```c
/* golden pairs from python: cbor2.dumps(v).hex() */
static void test_cbor_encode_int(void) {
    struct { const char *hex; int is_large; int sign; uint64_t mag; long smi; } cases[] = {
        {"00",0,0,0, 0},          {"17",0,0,0, 23},        {"1818",0,0,0, 24},
        {"18ff",0,0,0, 255},      {"190100",0,0,0, 256},   {"19ffff",0,0,0, 65535},
        {"1a00010000",0,0,0, 65536}, {"1affffffff",0,0,0, 4294967295L},
        {"1b0000000100000000",1,+1,4294967296ULL,0},
        {"1bffffffffffffffff",1,+1,18446744073709551615ULL,0}, /* +(2^64-1) */
        {"20",0,0,0, -1},         {"3863",0,0,0, -100},
        {"3bfffffffffffffffe",1,-1,18446744073709551615ULL,0}, /* -(2^64-1) => arg 2^64-2 */
    };
    for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        oop v = cases[i].is_large
              ? largeint_alloc(cases[i].sign, cases[i].mag)
              : TAG_SMI(cases[i].smi);
        oop out=NULL; int ok = platform_dispatch(PRIM_pf_cborEncodeInt, 1, &v, &out);
        assert(ok && out && obj_vtable(out)==RawBytesVtable);
        struct String *b=(struct String*)out;
        char got[40]={0}; for(size_t k=0;k<b->length;k++) sprintf(got+2*k,"%02x",b->bytes[k]);
        assert(strcmp(got, cases[i].hex)==0 && "int encode mismatch");
    }
    printf("test_cbor_encode_int OK\n");
}
```
Register `test_cbor_encode_int` in `test_cbor.c`'s `main`/runner and add `test_cbor` to
`run_c_tests` ENG_COMMON.

- [ ] **Step 2: Run, verify it fails to link** (`PRIM_pf_cborEncodeInt` undefined).
  Run: `cd /home/david/workspace/tuvm && ./run_c_tests.sh test_cbor` — Expected: compile/link
  error `PRIM_pf_cborEncodeInt undeclared`.

- [ ] **Step 3: Register the prim.** `dispatch.c`: append `PRIM_pf_cborEncodeInt,` to the enum
  (immediately after `PRIM_pf_pumpEvents`) and `"cborEncodeInt:",` as the last `PRIM_NAMES`
  entry. `prim_ids.h`: `#define PRIM_pf_cborEncodeInt 97`.

- [ ] **Step 4: Implement the case** in `platform.c` `platform_dispatch`:

```c
case PRIM_pf_cborEncodeInt: {
    /* Encode an Integer (SMI or LargeInteger) as CBOR major 0 (>=0) or major 1
       (<0), shortest form. Range is the ratified ±(2^64-1): major 0 arg = mag,
       major 1 arg = mag-1 (value = -1-arg). Both args fit uint64. */
    oop x = args[0];
    int major; uint64_t arg;
    if (IS_SMI(x)) {
        intptr_t v = UNTAG_SMI(x);
        if (v >= 0) { major = 0; arg = (uint64_t)v; }
        else        { major = 1; arg = (uint64_t)(-(v + 1)); }   /* -1-arg = v */
    } else if (LargeIntegerVtable && obj_vtable(x) == LargeIntegerVtable) {
        struct LargeInteger *li = (struct LargeInteger *)x;
        if (li->sign >= 0) { major = 0; arg = li->mag; }
        else               { major = 1; arg = li->mag - 1ULL; }  /* mag>=1 */
    } else {
        fprintf(stderr, "VM Error: cborEncodeInt: non-integer receiver\n"); abort();
    }
    uint8_t buf[9]; size_t n;
    unsigned mt = (unsigned)major << 5;
    if (arg < 24)              { buf[0]=(uint8_t)(mt|arg); n=1; }
    else if (arg < 0x100ULL)   { buf[0]=(uint8_t)(mt|24); buf[1]=(uint8_t)arg; n=2; }
    else if (arg < 0x10000ULL) { buf[0]=(uint8_t)(mt|25); buf[1]=(uint8_t)(arg>>8); buf[2]=(uint8_t)arg; n=3; }
    else if (arg < 0x100000000ULL) { buf[0]=(uint8_t)(mt|26);
        for(int s=3;s>=0;s--) buf[4-s]=(uint8_t)(arg>>(8*s)); n=5; }
    else { buf[0]=(uint8_t)(mt|27);
        for(int s=7;s>=0;s--) buf[8-s]=(uint8_t)(arg>>(8*s)); n=9; }
    oop b = gc_allocate(RawBytesVtable, sizeof(struct String) + n);
    ((struct String*)b)->length = n; memcpy(((struct String*)b)->bytes, buf, n);
    *out = b; return 1;
}
```

- [ ] **Step 5: Run, verify PASS.** Run: `./run_c_tests.sh test_cbor` — Expected:
  `test_cbor_encode_int OK`.

- [ ] **Step 6: Commit.**

```bash
git -C /home/david/workspace/tuvm add src/dispatch.c src/prim_ids.h src/platform.c tests/test_cbor.c run_c_tests.sh
git -C /home/david/workspace/tuvm commit -m "feat(cbor): cborEncodeInt: leaf (prim 97) — major 0/1 shortest form, SMI+LargeInteger"
```

---

## Task 3: C leaf `cborEncodeFloat:` (prim id 98)

**Files:** `tuvm/src/dispatch.c` (enum + name), `prim_ids.h` (`98`), `platform.c` (case),
`tuvm/tests/test_cbor.c` (add `test_cbor_encode_float`).

**Interfaces:** Consumes `struct Float{double value}`, `FloatVtable`, `RawBytesVtable`.
Produces id 98 (Task 5).

- [ ] **Step 1: Failing test** (goldens from `cbor2.dumps(float(x))`):

```c
static void test_cbor_encode_float(void) {
    struct { const char *hex; double v; } cases[] = {
        {"f90000", 0.0}, {"f93c00", 1.0}, {"f93e00", 1.5},   /* f16-exact */
        {"fa47c35000", 100000.0},                            /* f32-exact, not f16 */
        {"fb3ff199999999999a", 1.1},                         /* f64-only */
        {"f97c00", INFINITY}, {"f9fc00", -INFINITY},
    };
    for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        oop f = float_alloc(FloatVtable); ((struct Float*)f)->value = cases[i].v;
        oop out=NULL; int ok = platform_dispatch(PRIM_pf_cborEncodeFloat,1,&f,&out);
        assert(ok && obj_vtable(out)==RawBytesVtable);
        struct String *b=(struct String*)out; char got[40]={0};
        for(size_t k=0;k<b->length;k++) sprintf(got+2*k,"%02x",b->bytes[k]);
        assert(strcmp(got,cases[i].hex)==0 && "float encode mismatch");
    }
    printf("test_cbor_encode_float OK\n");
}
```

- [ ] **Step 2: Run, verify link failure** (`PRIM_pf_cborEncodeFloat` undefined).
  Run: `./run_c_tests.sh test_cbor`.

- [ ] **Step 3: Register prim** (enum after `cborEncodeInt`, name `"cborEncodeFloat:"`,
  `#define PRIM_pf_cborEncodeFloat 98`).

- [ ] **Step 4: Implement** the case in `platform.c` — shortest of f16/f32/f64 that
  round-trips exactly (try f16, then f32, else f64):

```c
case PRIM_pf_cborEncodeFloat: {
    double d = ((struct Float*)args[0])->value;
    uint8_t buf[9]; size_t n;
    float f = (float)d;
    /* try half: convert double->half, half->double, accept if bit-exact */
    uint16_t h; int half_ok = double_to_half(d, &h) && half_to_double(h) == d;
    if (half_ok) { buf[0]=0xf9; buf[1]=(uint8_t)(h>>8); buf[2]=(uint8_t)h; n=3; }
    else if ((double)f == d) { buf[0]=0xfa; uint32_t u; memcpy(&u,&f,4);
        for(int s=3;s>=0;s--) buf[4-s]=(uint8_t)(u>>(8*s)); n=5; }
    else { buf[0]=0xfb; uint64_t u; memcpy(&u,&d,8);
        for(int s=7;s>=0;s--) buf[8-s]=(uint8_t)(u>>(8*s)); n=9; }
    oop b = gc_allocate(RawBytesVtable, sizeof(struct String)+n);
    ((struct String*)b)->length=n; memcpy(((struct String*)b)->bytes,buf,n);
    *out=b; return 1;
}
```
Add file-local static helpers `double_to_half`/`half_to_double` (standard IEEE-754 binary16
conversion incl. inf/NaN/subnormal; NaN never reaches here — `isValue` rejects it). Keep them
`static` in `platform.c`.

- [ ] **Step 5: Run, verify PASS.** `./run_c_tests.sh test_cbor` → `test_cbor_encode_float OK`.

- [ ] **Step 6: Commit.**
```bash
git -C /home/david/workspace/tuvm commit -am "feat(cbor): cborEncodeFloat: leaf (prim 98) — shortest f16/f32/f64"
```

---

## Task 4: C decode leaves — `cborReadHead:at:` (99), `cborIntFromMajor:arg:ifOutOfRange:` (100), `cborReadFloat:at:` (101), `utf8Valid:` (102)

**Files:** `dispatch.c` (4 enum entries + 4 names), `prim_ids.h` (99–102), `platform.c`
(4 cases), `tuvm/tests/test_cbor.c` (decode tests).

**Interfaces:** Consumes `indexable_new(intptr_t)` (kernel Array; slot0=`TAG_SMI(n)`,
`instance_slot`/`instance_set_slot`), `largeint_alloc`, `oevm_int_demote`, `TAG_SMI`,
`float_alloc`. Produces ids 99–102 (Task 6 decode walk).

- [ ] **Step 1: Failing test** — head round-trip, int rebuild + `−2^64` reject, float read,
  utf8:

```c
static oop mkbytes(const uint8_t*p,size_t n){ oop b=gc_allocate(RawBytesVtable,sizeof(struct String)+n);
  ((struct String*)b)->length=n; if(n)memcpy(((struct String*)b)->bytes,p,n); return b; }
static long smi(oop x){ return (long)UNTAG_SMI(x); }

static void test_cbor_decode_leaves(void) {
    /* head: 0x1a 00 01 00 00  => major 0, arg 65536, next 5 */
    uint8_t h[]={0x1a,0,1,0,0}; oop hb=mkbytes(h,5);
    oop a[2]={hb,TAG_SMI(0)}, out=NULL;
    assert(platform_dispatch(PRIM_pf_cborReadHead,2,a,&out) && out!=TAG_SMI(-1));
    assert(smi(instance_slot((struct ObjectInstance*)out,1))==0);           /* major */
    assert(oevm_int_to_double(instance_slot((struct ObjectInstance*)out,2))==65536.0);
    assert(smi(instance_slot((struct ObjectInstance*)out,3))==5);           /* next */

    /* intFromMajor: major 1 arg (2^64-1) => -2^64 => out of range (sentinel) */
    oop big = largeint_alloc(+1, 18446744073709551615ULL);
    oop blkflag = false_symbol;   /* stand-in "block" — see note */
    oop a2[3]={TAG_SMI(1), big, blkflag}, o2=NULL;
    int r = platform_dispatch(PRIM_pf_cborIntFromMajor,3,a2,&o2);
    assert(r && o2==CBOR_OOR_SENTINEL);   /* leaf signals OOR; nsl maps to block */

    /* utf8Valid: */
    uint8_t good[]={0xc3,0xa9}; uint8_t bad[]={0xc3,0x28};
    oop g=mkbytes(good,2),bd=mkbytes(bad,2),og=NULL,ob=NULL;
    platform_dispatch(PRIM_pf_utf8Valid,1,&g,&og); platform_dispatch(PRIM_pf_utf8Valid,1,&bd,&ob);
    assert(og==true_symbol && ob==false_symbol);
    printf("test_cbor_decode_leaves OK\n");
}
```
**Note on the OOR signal:** the prim ABI returns one oop; `cborIntFromMajor:` cannot itself
call an nsl block. So the leaf returns a distinguished sentinel (`CBOR_OOR_SENTINEL`, e.g.
`TAG_SMI` of a reserved value or a dedicated interned symbol `#__cbor_oor`) and the **nsl**
`cborIntFromMajor:arg:ifOutOfRange:` wrapper checks for it and evaluates the block. Simplest:
define the leaf as `cborIntFromMajor:arg:` returning the Integer or the sentinel; the nsl walk
does `r == Cbor oorSentinel ifTrue: [ ^ self failWith: 'integer out of range' ]`. Update the
Interfaces block accordingly (drop the `ifOutOfRange:` from the C leaf; keep it nsl-side).

- [ ] **Step 2: Run, verify link failure.** `./run_c_tests.sh test_cbor`.

- [ ] **Step 3: Register the 4 prims** (enum order: `cborReadHead:at:`, `cborIntFromMajor:arg:`,
  `cborReadFloat:at:`, `utf8Valid:`; ids 99,100,101,102; `prim_ids.h` mirrors). Define
  `CBOR_OOR_SENTINEL` (a reserved interned symbol allocated at boot, or `TAG_SMI(INTPTR_MIN>>1)`
  — pick one and document).

- [ ] **Step 4: Implement the 4 cases** in `platform.c`:

```c
case PRIM_pf_cborReadHead: {
    /* majors 0-6 head parse. args: (aBytes, indexSMI). Returns Array{major,arg,next}
       or TAG_SMI(-1) if the head or its argument runs past the buffer. */
    struct String *b=(struct String*)args[0]; size_t len=b->length;
    size_t i=(size_t)UNTAG_SMI(args[1]);
    if (i>=len) { *out=TAG_SMI(-1); return 1; }
    unsigned ib=b->bytes[i]; unsigned major=ib>>5, ai=ib&0x1f; size_t j=i+1;
    uint64_t arg;
    if (ai<24) arg=ai;
    else if (ai==24){ if(j+1>len){*out=TAG_SMI(-1);return 1;} arg=b->bytes[j]; j+=1; }
    else if (ai==25){ if(j+2>len){*out=TAG_SMI(-1);return 1;} arg=((uint64_t)b->bytes[j]<<8)|b->bytes[j+1]; j+=2; }
    else if (ai==26){ if(j+4>len){*out=TAG_SMI(-1);return 1;} arg=0; for(int k=0;k<4;k++)arg=(arg<<8)|b->bytes[j+k]; j+=4; }
    else if (ai==27){ if(j+8>len){*out=TAG_SMI(-1);return 1;} arg=0; for(int k=0;k<8;k++)arg=(arg<<8)|b->bytes[j+k]; j+=8; }
    else { *out=TAG_SMI(-1); return 1; }   /* ai 28-31: reserved/indefinite -> reject */
    oop argv = (arg <= (uint64_t)INTPTR_MAX>>1) ? TAG_SMI((intptr_t)arg)
                                                : oevm_int_demote(largeint_alloc(+1,arg));
    oop r = indexable_new(3);
    instance_set_slot((struct ObjectInstance*)r,1,TAG_SMI((intptr_t)major));
    instance_set_slot((struct ObjectInstance*)r,2,argv);
    instance_set_slot((struct ObjectInstance*)r,3,TAG_SMI((intptr_t)j));
    *out=r; return 1;
}
case PRIM_pf_cborIntFromMajor: {
    /* args: (majorSMI, argInteger). Returns Integer, or CBOR_OOR_SENTINEL for -2^64. */
    intptr_t major=UNTAG_SMI(args[0]); oop av=args[1];
    uint64_t arg = IS_SMI(av) ? (uint64_t)UNTAG_SMI(av)
                              : ((struct LargeInteger*)av)->mag;  /* readHead built +sign */
    if (major==0) { *out = (arg? oevm_int_demote(largeint_alloc(+1,arg)) : TAG_SMI(0)); return 1; }
    /* major 1: value = -1-arg; arg==2^64-1 => -2^64 out of range */
    if (arg==18446744073709551615ULL) { *out=CBOR_OOR_SENTINEL; return 1; }
    *out = oevm_int_demote(largeint_alloc(-1, arg+1ULL)); return 1;
}
case PRIM_pf_cborReadFloat: {
    /* args: (aBytes, indexSMI at a 0xf9/0xfa/0xfb byte). Returns Array{Float,next} or TAG_SMI(-1). */
    struct String *b=(struct String*)args[0]; size_t len=b->length, i=(size_t)UNTAG_SMI(args[1]);
    if (i>=len) { *out=TAG_SMI(-1); return 1; }
    unsigned t=b->bytes[i]; size_t j=i+1; double d;
    if (t==0xf9){ if(j+2>len){*out=TAG_SMI(-1);return 1;} uint16_t h=(uint16_t)((b->bytes[j]<<8)|b->bytes[j+1]); d=half_to_double(h); j+=2; }
    else if (t==0xfa){ if(j+4>len){*out=TAG_SMI(-1);return 1;} uint32_t u=0; for(int k=0;k<4;k++)u=(u<<8)|b->bytes[j+k]; float f; memcpy(&f,&u,4); d=f; j+=4; }
    else if (t==0xfb){ if(j+8>len){*out=TAG_SMI(-1);return 1;} uint64_t u=0; for(int k=0;k<8;k++)u=(u<<8)|b->bytes[j+k]; memcpy(&d,&u,8); j+=8; }
    else { *out=TAG_SMI(-1); return 1; }
    oop f=float_alloc(FloatVtable); ((struct Float*)f)->value=d;
    oop r=indexable_new(2);
    instance_set_slot((struct ObjectInstance*)r,1,f);
    instance_set_slot((struct ObjectInstance*)r,2,TAG_SMI((intptr_t)j));
    *out=r; return 1;
}
case PRIM_pf_utf8Valid: {
    struct String *b=(struct String*)args[0]; const uint8_t *p=b->bytes; size_t n=b->length,i=0;
    while (i<n){ unsigned c=p[i];
        if (c<0x80) i++;
        else if ((c>>5)==0x6){ if(i+1>=n||(p[i+1]&0xc0)!=0x80||c<0xc2){*out=false_symbol;return 1;} i+=2; }
        else if ((c>>4)==0xe){ if(i+2>=n||(p[i+1]&0xc0)!=0x80||(p[i+2]&0xc0)!=0x80){*out=false_symbol;return 1;}
            if(c==0xe0&&p[i+1]<0xa0){*out=false_symbol;return 1;} if(c==0xed&&p[i+1]>0x9f){*out=false_symbol;return 1;} i+=3; }
        else if ((c>>3)==0x1e){ if(i+3>=n||(p[i+1]&0xc0)!=0x80||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80){*out=false_symbol;return 1;}
            if(c==0xf0&&p[i+1]<0x90){*out=false_symbol;return 1;} if(c>0xf4||(c==0xf4&&p[i+1]>0x8f)){*out=false_symbol;return 1;} i+=4; }
        else { *out=false_symbol; return 1; } }
    *out=true_symbol; return 1;
}
```
(If `instance_set_slot` is spelled differently in this tree, use the existing slot-writer —
grep `instance_slot`/`slots[` in `dispatch.c`/`platform.c` for the idiom; `PRIM_pf_string`
writes `((struct String*)s)->bytes[..]` and array prims write `inst->slots[i]`.)

- [ ] **Step 5: Run, verify PASS.** `./run_c_tests.sh test_cbor` → `test_cbor_decode_leaves OK`.

- [ ] **Step 6: Commit.**
```bash
git -C /home/david/workspace/tuvm commit -am "feat(cbor): decode leaves cborReadHead:/IntFromMajor:/ReadFloat:/utf8Valid: (prims 99-102)"
```

---

## Task 5: nsl `Cbor` class — encode walk (+ both-path encode tests)

**Files:**
- Modify: `nsl-tuvm/lib/kernel.ns` (new `Cbor` class, module sibling of `List`/`Map`/`Record`;
  reaches Kernel helpers via `outer`)
- Create: `nsl-tests/14_cbor/02_golden_hex.ns`, `nsl-tests/14_cbor/03_map_order.ns`

**Interfaces:** Consumes leaves 97/98 (`platform cborEncodeInt:`, `platform cborEncodeFloat:`),
`outer isValueObject:`, `outer valueCompare:with:`, `platform valueRank:`, `OrderedCollection`,
`Bytes` (`Bytes new:`/`overRaw:`/`freeze`, `u8At:put:`/`byteSize` — confirm write selector in
`kernel.ns` ~L447/L1151), `List`/`Map`/`Record` accessors (`items`/`do:`, `keys`/`vals`,
`tag`/`fields`), `Symbol` text. Produces `Cbor encode:` (Task 6/7 use it).

- [ ] **Step 1: Write the failing both-path golden test.** `nsl-tests/14_cbor/02_golden_hex.ns`:

```newspeak
(* Golden from python cbor2:
   cbor2.dumps({ 'n': 42, 's': 'hi' }) is a map; but our Map keys are Symbols =>
   tag 39. Author the golden with a matching cbor2 script (see 14_cbor/README). *)
| m enc |
m:: Map fromPairs: (List
      with: (List with: #n with: 42)
      with: (List with: #s with: 'hi')).
enc:: Cbor encode: m.
Transcript showCr: enc printHex.   "expect the cbor2-authored golden hex"
```
Author the expected hex by running `cbor2` with tag-39 symbol keys (documented in a new
`nsl-tests/14_cbor/README.md` fixture note). Freeze the `.expected` to that hex.

- [ ] **Step 2: Run both paths, verify FAIL** (`Cbor` DNU). Recipe:
```bash
cd /home/david/workspace/nsl-tuvm/tools
OEVM_DIR=/home/david/workspace/tuvm OEVM_PATH=AB python3 - <<'PY'
from pathlib import Path; import run_tests_oevm as R
p=Path('/home/david/workspace/nsl-tests/14_cbor/02_golden_hex.ns')
print('A',R.run_test_file(p)); print('B',R.run_test_file_oimg(p))
PY
```
Expected: both FAIL, `Cbor` does not understand `encode:`.

- [ ] **Step 3: Implement the `Cbor` encode walk** in `kernel.ns` (sibling of `Record`).
  Structure (fill bodies against the confirmed helpers):

```newspeak
class Cbor = () (
  (* --- encode --- *)
  public encode: v = ( | acc | acc:: OrderedCollection new. self encode: v into: acc depth: 1.
    ^ self accToBytes: acc )

  encode: v into: acc depth: d = (
    d > 32 ifTrue: [ ^ self failEncode: 'message depth exceeded' ].
    v == nil ifTrue: [ ^ acc add: 16rF6 ].
    v == true ifTrue: [ ^ acc add: 16rF5 ].
    v == false ifTrue: [ ^ acc add: 16rF4 ].
    | rank | rank:: outer rankOf: v.
    rank = 2 ifTrue: [ ^ self encodeInteger: v into: acc ].     "Integer"
    rank = 3 ifTrue: [ ^ self encodeFloat: v into: acc ].       "Float"
    rank = 4 ifTrue: [ ^ self encodeString: v into: acc ].      "String -> major 3"
    rank = 5 ifTrue: [ ^ self encodeBytes: v into: acc ].       "Bytes  -> major 2"
    rank = 6 ifTrue: [ ^ self encodeSymbol: v into: acc ].      "Symbol -> tag 39 + text"
    rank = 7 ifTrue: [ ^ self encodeList: v into: acc depth: d ].
    rank = 8 ifTrue: [ ^ self encodeMap: v into: acc depth: d ].
    rank = 9 ifTrue: [ ^ self encodeRecord: v into: acc depth: d ].
    ^ self failEncode: 'not a value' )
  (* encodeInteger:/encodeFloat: append the C-leaf RawBytes fragment byte-by-byte:
     frag:: platform cborEncodeInt: v. 1 to: frag basicSize do: [:i| acc add: (frag basicAt: i)]. *)
  (* head emission (major m, length n small SMI) via integer arithmetic, NOT bitops:
     emitHead: m len: n into: acc — n<24: acc add: (m*32)+n; n<256: acc add:(m*32)+24; acc add: n;
     n<65536: (m*32)+25 then n//256, n\\256; etc. *)
  (* encodeMap: RFC bytewise order — for each entry encode key-bytes and val-bytes into
     temp OrderedCollections, collect (keyBytes,valBytes) pairs, SORT pairs by keyBytes
     bytewise (outer helper or a small compareBytes:with:), emit major-5 head (size), then
     concatenated sorted pairs. *)
  ...
)
```
Use `outer rankOf:` (kernel.ns:1066 — `platform valueRank:` then nsl `valueRank`) so builtins
and heap values classify uniformly. Verify the `Bytes`/`String` read selectors (`byteSize`,
`u8At:`, `basicAt:`) against kernel.ns before finalizing.

- [ ] **Step 4: Run both paths, verify golden PASS (A==B).** Same recipe as Step 2. Expected:
  both print the golden hex.

- [ ] **Step 5: Add + run the map-order test** `03_map_order.ns` — a `Map` with keys whose
  bytewise order ≠ `valueCompare:` order (e.g. `#z` and `#aa`); assert encoded bytes = the
  bytewise-sorted golden. Run both paths → A==B PASS.

- [ ] **Step 6: Restore byproducts + commit.**
```bash
git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
git -C /home/david/workspace/nsl-tuvm commit -am "feat(cbor): Cbor encode walk — deterministic bytewise, C int/float leaves"
git -C /home/david/workspace/nsl-tests add 14_cbor && git -C /home/david/workspace/nsl-tests commit -m "test(cbor): encode golden-hex + bytewise map-order (both-path)"
```

---

## Task 6: nsl `Cbor` class — decode walk (+ round-trip & rejection tests)

**Files:** Modify `nsl-tuvm/lib/kernel.ns` (`Cbor` decode); Create `nsl-tests/14_cbor/01_roundtrip.ns`,
`nsl-tests/14_cbor/04_reject.ns`.

**Interfaces:** Consumes leaves 99/100/101/102 + 97/98-round-trip, `List with:`/`Map fromPairs:`/
`Record tag:fields:`, symbol interning (build a Symbol from decoded text — confirm the kernel
selector, e.g. `String asSymbol` / `Symbol intern:`), Milestone-E `on:do:` for making decode
total. Produces `Cbor decode:ifInvalid:`.

- [ ] **Step 1: Failing round-trip test** `01_roundtrip.ns`:

```newspeak
| orig enc back |
orig:: Record tag: #reading fields: (Map fromPairs: (List
        with: (List with: #id  with: 4294967296)          "> host SMI => LargeInteger path"
        with: (List with: #name with: 'sensor')
        with: (List with: #ok  with: true)
        with: (List with: #xs  with: (List with: 1 with: 2 with: 3)))).
enc:: Cbor encode: orig.
back:: Cbor decode: enc ifInvalid: [ :e | Transcript showCr: 'ERR ', e. ^ nil ].
Transcript showCr: (orig = back) printString.    "expect true"
```
Expected `.expected`: `true`.

- [ ] **Step 2: Run both paths, verify FAIL** (`Cbor` DNU `decode:ifInvalid:`). Recipe as Task 5.

- [ ] **Step 3: Implement the decode walk** — total via `on:do:` wrapping a recursive parser
  that uses `platform cborReadHead:at:` and raises a tagged error on any violation:

```newspeak
public decode: aBytes ifInvalid: aBlock = (
  ^ [ | r | r:: self parse: aBytes at: 1 depth: 1.
      (r at: 2) = (aBytes byteSize + 1) ifFalse: [ self failDecode: 'not a value' ].
      r at: 1 ]
    on: [ :e | (e tag == #cborInvalid) ] do: [ :e | aBlock value: (e at: #text ifAbsent: [ 'not a value' ]) ] )

parse: b at: i depth: d = ( (* returns 2-elt Array {value, nextIndex} *)
  d > 32 ifTrue: [ self failDecode: 'message depth exceeded' ].
  | first | first:: b u8At: i.
  first = 16rF6 ifTrue: [ ^ Array with: nil  with: i + 1 ].
  first = 16rF5 ifTrue: [ ^ Array with: true with: i + 1 ].
  first = 16rF4 ifTrue: [ ^ Array with: false with: i + 1 ].
  (first = 16rF9 or: [ first = 16rFA or: [ first = 16rFB ]]) ifTrue: [
     | fr | fr:: platform cborReadFloat: b at: i. fr = -1 ifTrue: [ self failDecode: 'not a value' ]. ^ fr ].
  first = 16rF7 ifTrue: [ self failDecode: 'not a value' ].   "undefined"
  (* majors 0-6 via head *)
  | h major arg nxt | h:: platform cborReadHead: b at: i.
  h = -1 ifTrue: [ self failDecode: 'not a value' ].
  major:: h at: 1. arg:: h at: 2. nxt:: h at: 3.
  major = 0 ifTrue: [ ^ Array with: (self intFrom: 0 arg: arg) with: nxt ].
  major = 1 ifTrue: [ ^ Array with: (self intFrom: 1 arg: arg) with: nxt ].
  major = 2 ifTrue: [ ^ self bytesFrom: b at: nxt len: arg ].
  major = 3 ifTrue: [ ^ self stringFrom: b at: nxt len: arg ].
  major = 4 ifTrue: [ ^ self listFrom: b at: nxt count: arg depth: d ].
  major = 5 ifTrue: [ ^ self mapFrom: b at: nxt count: arg depth: d ].
  major = 6 ifTrue: [ ^ self tagFrom: b at: nxt tag: arg depth: d ].
  self failDecode: 'not a value' )

intFrom: m arg: arg = ( | r | r:: platform cborIntFromMajor: m arg: arg.
  r == Cbor oorSentinel ifTrue: [ self failDecode: 'integer out of range' ]. ^ r )

failDecode: s = ( ^ self error: s tag: #cborInvalid )
```
- `tagFrom:` : tag 39 → decode a major-3 text, intern to Symbol; tag 27 → decode a 2-array
  `[tagSym, fieldsMap]`, `Record tag:fields:`; tag 2/3 → `self failDecode: 'integer out of range'`;
  else → `not a value`.
- `stringFrom:` : slice `len` bytes; `platform utf8Valid:` the slice → false ⇒
  `self failDecode: 'invalid utf-8'`; build a `String`.
- `mapFrom:` : parse `count` key/value pairs; **before** constructing, check each new key against
  already-collected keys via algebra `=` → duplicate ⇒ `self failDecode: 'not a value'`; build
  via `Map fromPairs:`.
- `listFrom:` : parse `count` elements, `List fromCollection:`.
- Depth `d+1` passed into container element parses.

- [ ] **Step 4: Run both paths, verify round-trip PASS (A==B).** Recipe as Task 5. Expected:
  both print `true`.

- [ ] **Step 5: Add + run rejection corpus** `04_reject.ns` — decode each of: an indefinite-length
  array (`0x9f…`), an unknown tag (`0xd8 0x40 …`, tag 64), a depth-33 nesting, a duplicate-key
  map, a truncated buffer, and a `−2^64` int (`0x3b ffffffffffffffff`); assert the `ifInvalid:`
  block runs with the expected error text for each. Non-tautological: assert the text, e.g.:
```newspeak
Transcript showCr: (Cbor decode: (Bytes fromHex: '3bffffffffffffffff') ifInvalid: [ :e | e ]).
"expect: integer out of range"
```
Run both paths → A==B PASS.

- [ ] **Step 6: Restore byproducts + commit.**
```bash
git -C /home/david/workspace/tuvm checkout -- src/solidified_image.c src/solidified_image.stmap
git -C /home/david/workspace/nsl-tuvm commit -am "feat(cbor): Cbor decode walk — total, closed, tag 27/39, dup-key & range rejection"
git -C /home/david/workspace/nsl-tests add 14_cbor && git -C /home/david/workspace/nsl-tests commit -m "test(cbor): round-trip + rejection corpus (both-path)"
```

---

## Task 7: Integration, doc reconciliation, gates, close (controller-owned)

**Files:** `tuvm/review/lib/cbor.md`, `tuvm/review/lib/errors.md`,
`tuvm/review/program-sequence.md`; `nsl-tests/14_cbor/05_int_band.ns`; footprint bank;
GitHub tracking issue.

**Interfaces:** Consumes everything above. Produces the merge-ready branch + reconciled docs.

- [ ] **Step 1: Add `05_int_band.ns`** — a round-trip whose integer is a literal in `[2^61,2^62)`
  (now safe post-Task 1). Run both paths → A==B PASS.

- [ ] **Step 2: Reconcile `tuvm/review/lib/cbor.md`.** Replace the mapping-row rule
  `value > 2^63−1 → error` with the ratified `±(2^64−1)` rule; add `−2^64`/bignum →
  `integer out of range`; note the surface is nsl `Cbor encode:`/`decode:ifInvalid:` (not a
  `platform` prim) and the map order is RFC-bytewise; flip Status to "implemented (rung 3)".

- [ ] **Step 3: Reconcile `tuvm/review/lib/errors.md`.** Correct the stale
  `integer overflow past 2^63−1` line to the ratified trap `LargeInteger overflow (magnitude
  exceeds 2^64-1)`; add the decode-side `integer out of range` row.

- [ ] **Step 4: Full both-path suite gate (controller-owned).** Run the whole suite both paths;
  confirm `14_cbor/01..05` PASS and no regression; run the four named gates (grant,
  debugger-integration, portability-32, boot-gc-asan). Record counts. Restore byproducts.

- [ ] **Step 5: Bank footprint** (host + mcu ROM/RAM deltas) per the metrics-spec; note the
  leaf-prim text delta and that `Cbor` adds nsl image bytes only.

- [ ] **Step 6: Mark `program-sequence.md` rung-3 DONE**; write a short rung-close note; file the
  **wrapping-arithmetic tracking issue** (or annotate tuvm#4) for the deferred L7 option 3.

- [ ] **Step 7: Commit + close.**
```bash
git -C /home/david/workspace/tuvm commit -am "docs(rung3): reconcile cbor.md/errors.md to ratified ±(2^64-1); mark rung-3 done"
git -C /home/david/workspace/nsl-tests add 14_cbor/05_int_band.* && git -C /home/david/workspace/nsl-tests commit -m "test(cbor): int-band round-trip (both-path)"
```
Merge/push ONLY on David's word (all 3 repos, per the milestone pattern).

---

## Self-review notes (author)

- **Spec coverage:** §1 scope → Tasks 1–7; §2 architecture (B) → Tasks 5/6 (nsl walk) + 2/3/4
  (leaves); §3 seam → Tasks 2–4 (leaves) + 5/6 (surface); §4 mapping/int reconciliation →
  Tasks 2/4 (leaves) + 5/6 (walk) + 7 (docs); §4.2 bytewise → Task 5 Step 5; §5 total/closed →
  Task 6 Steps 3/5; §6 prims → Tasks 2–4; §7 .oimg → Task 1; §8 deferred → Task 7 Step 6;
  §9 tests → each task's tests + Task 7 gate. Covered.
- **Open confirmations for implementers (not placeholders — verify against the live tree):**
  (a) which repo owns `oimg_serialize.py`/`ns_compile_oevm.py` (Task 1); (b) the `Bytes` mutable
  write selector + `byteSize`/`u8At:` read names (Task 5); (c) the slot-writer spelling
  (`instance_set_slot` vs `->slots[i]`, Task 4); (d) the symbol-intern selector for decode
  (Task 6); (e) `Bytes fromHex:`/`printHex` test helpers exist or add them to the 14_cbor README.
  Each is a named, located lookup, not an undefined interface.
