# #297 Cbor native walk — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `Cbor>>encode:` runs as one C walk (~45× faster on the 425 B frame) dispatched through a `cborWalk` NATIVE binding on each value class, and falls back to today's nsl walk on any doubt.

**Architecture:** builtins.def gains a `NATIVE` row kind (a firmware-static `PrimitiveClosure` whose
name string also lists the ivars C reads) plus a `SYMBOL` row `cborWalk`. The compiler accepts the method form
`sel = native <name>`, binds it as a registry reference, and refuses at bake time when the class's slot layout
does not match the row. `platform cborEncode: v` walks in C (two passes, two GC allocations) and answers
nil to mean "refused". `Cbor>>encode:` then runs `encodeOracle:`, which is today's nsl walk under a new name.

**Tech Stack:** C99 (oevm), Python 3 (nsl compiler, oimg serializer), Ohm grammar (JS), nsl.

**Spec:** `docs/design/2026-10-04-297-cbor-native-walk-design.md` (D1–D5). Spike C (the A′ arm, the
starting point for Task 1's walk): `git show 297-cbor-spike:nodus/oevm/src/cbor_spike.inc`.

> **Plan-embedded code is unrun code** (memory: ~1 defect per task on #165/#190). The snippets
> carry the design. They have never been compiled. Each task's tests are what prove them.

## Global Constraints

- `BUILTIN_REGISTRY_VERSION` 5 → 6 (`nodus/oevm/src/oimg_loader.c:25`) and `gen_builtins.VERSION` 5 → 6, in the **same commit**; then `rm -rf nodus/nsl/tools/__pycache__` before any proving run (memory: a stale `.pyc` served the wrong VERSION).
- No `.oimg` format change: the binding's method is a `TAG_BUILTIN` registry word.
- C refuses with **one** sentinel (nil) and never produces an error text. Error texts and their order stay in nsl only.
- No tunables: no `.bss` scratch, no C heap, no size caps. Depth bound 32 (the language's own bound).
- C accepts **both** `true` objects (`true_symbol` and `oimg_singleton[1]`, likewise false): #330 is not fixed here.
- Symbols encode as tag 39 and Records as tag 27 over `[tag, fields]` (the design corrects #297's text).
- Map keys sort RFC 8949 §4.2 bytewise over the encoded keys, using a stable insertion sort (same as `sortPairsBytewise:`).
- `cborWalk` cannot be sent from nsl. Doing so aborts with `VM Error:` and names the receiver's class.
- Path B (`.oimg`) only in the nsl suite (standing rule). Path A gets the oracle only. See Review Focus 5.
- No new `.c` file: the walk lives in `src/platform.c` beside the leaf prims and reuses `double_to_half`/`half_to_double`. That avoids #63's nine source lists.
- Never run two nsl suites at once, and never run `porta_sync_host.sh` beside one. The suite takes about 20 min: background it with `-u` to a file, never under `timeout`.

## Review Focus

1. **A native name colliding with an app symbol.** If the generated `REGISTRY` held the key `cborList`, `_symbol_ref('cborList')` would resolve an app's `#cborList` to the native closure. Natives therefore go in a separate `NATIVES` dict. Task 1 Step 1 tests this.
2. **A subclass of List/Map/Record/Bytes with extra ivars.** It inherits the binding. C reads slots 1..k, so this is correct only if inherited slots keep their indices. The bake check compares the *prefix* of the class layout for subclasses and the exact layout for the declaring class. Task 2 Step 1 tests both.
3. **A Map whose keys are themselves Maps or Lists.** The scratch area must nest with stack discipline (a key's inner map sorts inside the outer map's region). The oracle gate in Task 3 includes `{ {#b->1, #a->2} -> 1 }`.
4. **A value that survives construction but fails encode** (a depth-32 tree with a leaf: leaves sit at d=33). C must refuse, and the oracle must raise `message depth exceeded`. Task 3 has its own `expect-runtime-fail` test.
5. **Path A images.** `Symbol_cborWalk` stays NULL under Model A (it is set only by `kernel_boot`), so `cborEncode:` refuses and the oracle runs: correct bytes, old speed. Task 1 Step 6 asserts that a NULL selector refuses.

---

### Task 1: VM — NATIVE rows, the C walk, `platform cborEncode:`

**Files:**
- Modify: `nodus/oevm/src/builtins.def` (rows 19–23)
- Modify: `nodus/nsl/tools/gen_builtins.py` (NATIVE parse, `NATIVES`, VERSION 6); regenerate `nodus/nsl/tools/builtins_registry.py`
- Modify: `nodus/nsl/tools/test_gen_builtins.py`
- Modify: `nodus/oevm/src/oimg_loader.c:25` (version 6)
- Modify: `nodus/oevm/src/oevm.h` (`struct CborNative`, the four externs, `Symbol_cborWalk`)
- Modify: `nodus/oevm/src/kernel_boot.c` (`KSym_cborWalk`, registry rows 19–23, header comment)
- Modify: `nodus/oevm/src/platform.c` (natives + walk + `PRIM_pf_cborEncode` case)
- Modify: `nodus/oevm/src/prim_names.def`, `src/dispatch.c` enum + parity guard, `src/prim_ids.h` (the prim triple)
- Modify: `nodus/oevm/src/vm.c` (`oop Symbol_cborWalk = NULL;` beside `Symbol_value`, `vm.c:337`); `src/gc.c:832` marks it like `Symbol_value`
- Test: `nodus/oevm/tests/test_cbor.c` (already a `C_UNIT_TESTS` row, ENG_B); `tests/test_kernel_boot.c` if it asserts row count

**Interfaces:**
- Produces (for Task 2): builtins.def rows `BUILTIN(19, NATIVE, "cborList items depth")`, `(20, NATIVE, "cborMap keys vals depth")`, `(21, NATIVE, "cborRecord tag fields depth")`, `(22, NATIVE, "cborBytes raw")`, `(23, SYMBOL, "cborWalk")`. Python: `builtins_registry.NATIVES = {'cborList': (19, ['items','depth']), …}`; `REGISTRY` gains `'cborWalk': 23` and **no** native names.
- Produces (for Task 2's Path A): C objects `Native_cborList`, `Native_cborMap`, `Native_cborRecord`, `Native_cborBytes` (`const struct CborNative`, declared in `oevm.h`).
- Produces (for Task 3): prim selector `cborEncode:` (id 160, next after `PRIM_pf_boardName` 159). It answers a RawBytes on success and nil when refused.

- [ ] **Step 1: Failing test — registry.** In `test_gen_builtins.py` add:

```python
def test_native_rows_are_not_symbols(tmp_path):
    out = tmp_path / "reg.py"
    gen_builtins.main(out)
    ns = {}
    exec(out.read_text(), ns)
    assert ns["VERSION"] == 6
    assert ns["NATIVES"]["cborList"] == (19, ["items", "depth"])
    assert ns["NATIVES"]["cborBytes"] == (22, ["raw"])
    assert ns["REGISTRY"]["cborWalk"] == 23
    for name in ("cborList", "cborMap", "cborRecord", "cborBytes"):
        assert name not in ns["REGISTRY"]          # Review Focus 1
```

Run: `cd nodus/nsl/tools && python3 -m pytest test_gen_builtins.py -q` → FAIL (no rows yet).

- [ ] **Step 2: Rows + generator.** Append the five rows above to `builtins.def`. Update its header comment: `kind: SINGLETON | VTABLE | SYMBOL | NATIVE`. A NATIVE name string is `"<native> <ivar>…"`, and the ivars are the slots C reads in order, starting at slot 1. Version 5→6. In `gen_builtins.py`, split NATIVE rows from the rest:

```python
VERSION = 6
...
    reg, natives = [], []
    for idx, kind, name in entries:
        if kind == "NATIVE":
            head, *ivars = name.split()
            natives.append(f"    {head!r}: ({idx}, {ivars!r})")
        else:
            reg.append(f"    {name!r}: {idx}")
    text = (f"# GENERATED by gen_builtins.py — do not edit.\nVERSION = {VERSION}\n"
            f"REGISTRY = {{\n" + ",\n".join(reg) + "\n}\n"
            f"NATIVES = {{\n" + ",\n".join(natives) + "\n}\n")
```

Set `BUILTIN_REGISTRY_VERSION = 6`. Run `python3 nodus/nsl/tools/gen_builtins.py && rm -rf nodus/nsl/tools/__pycache__`, then the Step 1 test → PASS.

- [ ] **Step 3: Failing C test — the walk's leaves and its refusals.** Append to `tests/test_cbor.c`. Containers need an image, so they are Task 3's job. This test covers leaves and the sentinel:

```c
static oop enc(oop v) { oop out = NULL; oop a[1] = { v };
    assert(platform_dispatch(PRIM_pf_cborEncode, 1, a, &out) == 1); return out; }
static void expect_hex(oop b, const char *hex) {
    struct String *s = (struct String *)b; char buf[64] = {0};
    assert(b != NULL);
    for (size_t i = 0; i < s->length; i++) sprintf(buf + 2 * i, "%02x", s->bytes[i]);
    if (strcmp(buf, hex)) { fprintf(stderr, "got %s want %s\n", buf, hex); exit(1); } }
...
    expect_hex(enc(NULL), "f6");
    expect_hex(enc(TAG_SMI(23)), "17");
    expect_hex(enc(TAG_SMI(-1)), "20");
    expect_hex(enc(TAG_SMI(1000)), "1903e8");
    assert(enc(float_new(NAN)) == NULL);                 /* NaN: refused, oracle raises */
    { oop plain = gc_allocate(RawBytesVtable, sizeof(struct String));   /* no cborWalk binding */
      ((struct String *)plain)->length = 0;
      assert(enc(plain) == NULL); }                       /* bare RawBytes is not a value */
    { extern oop Symbol_cborWalk; oop saved = Symbol_cborWalk; Symbol_cborWalk = NULL;
      expect_hex(enc(TAG_SMI(5)), "05");                  /* leaves need no selector */
      Symbol_cborWalk = saved; }                          /* Review Focus 5 (container half: Task 3) */
```

(Use whatever Float constructor `test_cbor.c` already uses for its `cborEncodeFloat:` cases. Grep the file first.) Run the C unit gate: `cd nodus/nsl && python3 -c "import run_tests_oevm as r; r.run_c_unit_tests(only=['test_cbor'])"`. If that entry point does not exist, build with the header's own command line. → FAIL to compile (`PRIM_pf_cborEncode` undeclared).

- [ ] **Step 4: The prim triple.** Append `PRIM_NAME("cborEncode:")` to `prim_names.def`. Append `PRIM_pf_cborEncode,` to dispatch.c's enum after `PRIM_pf_boardName`, and advance the parity guard to `PRIM_pf_cborEncode`. Add `#define PRIM_pf_cborEncode 160` to `prim_ids.h`. First confirm that `cborEncode:` is not an existing nsl selector: `rg -n "cborEncode:" --glob '*.nsl' nodus | grep -v "cborEncodeInt\|cborEncodeFloat"` must come back empty. (A Symbol carries its prim id, so any send with that name would be intercepted, #226.)

- [ ] **Step 5: Natives + walk in `platform.c`.** Put this beside `PRIM_pf_cborEncodeFloat`. In `oevm.h`:

```c
/* #297: a NATIVE registry row (builtins.def). The closure comes first so a binding's
   method oop IS the struct; `walk` is reached from it. */
struct CbW;
struct CborNative { struct Closure closure; int (*walk)(struct CbW *, oop, int); };
extern const struct CborNative Native_cborList, Native_cborMap, Native_cborRecord, Native_cborBytes;
extern oop Symbol_cborWalk;   /* NULL under Model A: cborEncode: then refuses containers */
```

In `platform.c`, start from the spike's `CbW`/`cbw_byte`/`cbw_bytes`/`cbw_head`/`cb_leaf` (with `allow_raw` dropped: a bare RawBytes refuses) and its `cb2_*` walkers, with four changes:
  1. **Dispatch** (replaces `a2_meth` compare):

```c
static int cb_enc(CbW *w, oop v, int d) {
    if (d > 32) return -1;
    int r = cb_leaf(w, v);
    if (r) return r > 0 ? 0 : -1;
    if (Symbol_cborWalk == NULL) return -1;
    oop m = NULL, dv = NULL;
    lookup_method_cached(obj_vtable(v), Symbol_cborWalk, &m, &dv);
    if (m == NULL || obj_vtable(m) != PrimitiveClosureVtable
        || closure_entry((struct Closure *)m) != (void *)cbor_native_unsendable) return -1;
    return ((const struct CborNative *)m)->walk(w, v, d);
}
```

  2. **Unsendable entry** (each native's `closure.entry_point`):

```c
static oop cbor_native_unsendable(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    const char *cls = oimg_vtable_name(obj_vtable(receiver));
    fprintf(stderr, "VM Error: cborWalk is a native binding (#297) and is not sendable from nsl"
                    " (receiver %s)\n", cls ? cls : "?");
    abort();
}
#define CB_NATIVE(N, FN) const struct CborNative N = { \
    .closure = { .base = { (oop)&Vtable_PrimitiveClosureVtable }, .entry_point = (void *)cbor_native_unsendable }, \
    .walk = FN }
CB_NATIVE(Native_cborList, cb_list); CB_NATIVE(Native_cborMap, cb_map);
CB_NATIVE(Native_cborRecord, cb_record); CB_NATIVE(Native_cborBytes, cb_bytes);
```

  (Check how `oimg_vtable_name` behaves for a RAM vtable and that `extern const struct Vtable Vtable_PrimitiveClosureVtable` is declared in both link sets. `solidified_image.c` and `kernel_boot.c` each define it.)
  3. **Slot constants cite the row**: `/* builtins.def row 19 "cborList items depth": slot 1 = items */ #define CB_LIST_ITEMS 1`. Do the same for map keys 1 / vals 2, record tag 1 / fields 2, bytes raw 1.
  4. **D4 memory: no static scratch.** Pass 1 (`w->p == NULL`) counts output bytes. In each map it also adds `n` to `w->entries` and each key's encoded length to `w->keybytes` (add both fields to `CbW`, plus `uint8_t *scr; size_t sp, scap;`). Then:

```c
static oop cb_encode(oop v) {
    CbW c = {0};
    if (cb_enc(&c, v, 1)) return NULL;
    size_t esz = sizeof(struct CbEnt);   /* { uint32_t off, len; oop val; } */
    size_t scap = c.entries * esz + c.keybytes + (c.maps + 1) * esz;   /* per-map + base alignment slack */
    oop out = gc_allocate(RawBytesVtable, sizeof(struct String) + c.n);
    oop scr = scap > esz ? gc_allocate(RawBytesVtable, sizeof(struct String) + scap) : NULL;
    /* both allocations before pass 2: no GC mid-walk; v rooted by the prim's args;
       `out` must survive the second allocation — root it the way other two-allocation
       prims in platform.c do (grep for an existing pattern) */
    ((struct String *)out)->length = c.n;
    CbW w = { .p = ((struct String *)out)->bytes, .cap = c.n };
    if (scr) { ((struct String *)scr)->length = scap; w.scr = align_up(((struct String *)scr)->bytes, esz); w.scap = scap - esz; }
    if (cb_enc(&w, v, 1) || w.n != c.n) return NULL;
    return out;
}
```

  In pass 2, `cb_map` carves its entry array, then its key bytes, from `w.scr` at `w.sp`, and restores `sp` on exit (stack discipline, Review Focus 3). Entries go first so they stay aligned. Each map sub-area is `n*esz` aligned up to `esz`. That is why the slack is per map (`c.maps` is counted in pass 1 alongside `entries`/`keybytes`). Declare the writer as `typedef struct CbW { … } CbW;`, matching the `struct CbW` forward declaration in `oevm.h`. Replace the spike's `return -3` paths with an `assert(w->sp + need <= w->scap)`: pass 1 makes overflow impossible, and the assert is what proves it.

  Then `case PRIM_pf_cborEncode: *out = cb_encode(args[0]); return 1;`.

- [ ] **Step 6: Registry + boot.** Add `KSym_cborWalk` (`.prim_id = 0`, `.length = 8`, `.name = "cborWalk"`) to `kernel_boot.c`. Assign `Symbol_cborWalk = (oop)&KSym_cborWalk;` in step (a') and `builtin_registry[19..22] = (oop)&Native_cbor…; builtin_registry[23] = (oop)&KSym_cborWalk;` in step (c). Update the header comment's index map. Define `oop Symbol_cborWalk = NULL;` in `vm.c` and mark it in `gc.c` beside `Symbol_value`.

- [ ] **Step 7: Run.** Run `test_cbor` → PASS. Then run the full C unit table plus `bash nodus/oevm/tests/test_portability_32.sh` (the `-m32` build is the only test of the 31-bit SMI paths in `cbw_head`), plus `bash nodus/oevm/tests/hanoi_ab_host.sh` and `http_roundtrip_host.sh` (memory: the suite does not run them). All green.

- [ ] **Step 8: Commit.**

```bash
git add nodus/oevm/src nodus/oevm/tests/test_cbor.c nodus/nsl/tools/gen_builtins.py \
        nodus/nsl/tools/builtins_registry.py nodus/nsl/tools/test_gen_builtins.py
git commit -m "vm(#297): NATIVE registry rows + the C cbor walk behind platform cborEncode:"
```

### Task 2: Compiler — `sel = native <name>`, binding emission, the bake shape check

**Files:**
- Modify: `nodus/nsl/grammar/ns.ohm:31` (`MethodDef`), `nodus/nsl/grammar/ns-parse.js:155` (handlers)
- Modify: `nodus/nsl/tools/ns_compile_oevm.py` (`compile_class` method loop ~:1873; a check after `finalize_slot_layouts`)
- Modify: `nodus/nsl/tools/solidify.py` (`add_native_binding`, `generate_c` bindings loop :237)
- Modify: `nodus/nsl/tools/oimg_serialize.py` (binding emission :705–708; `method_name_map` :497; the symbol-collection loop :530)
- Test: `nodus/nsl/tools/test_compile_diagnostics.py` (refusals) and `test_compile_oimg.py` (emission). Follow each file's existing case style.

**Interfaces:**
- Consumes: `builtins_registry.NATIVES` and `REGISTRY['cborWalk']` (Task 1).
- Produces: AST method node `{type:'method', selector, params, native: '<name>', body: []}`. A binding tuple `(selector, ("NATIVE", name))` in `SolidifyCompiler.vtables[vt]['bindings']`. A bake error that contains both the native name and the class qname.

- [ ] **Step 1: Failing tests.** Add four compile cases:
  - (a) A class `List = ( | items depth | ) ( cborWalk = native cborList )` compiles. In the `.oimg`, the binding's method word decodes to `RefWord.builtin(19)` and its symbol word to `RefWord.builtin(23)`.
  - (b) The same class with `| depth items |` refuses. The message contains `cborList` and the class name (D5's falsification, made permanent).
  - (c) `cborWalk = native nosuch` refuses, naming `nosuch`.
  - (d) A subclass `class L2 = List ( | extra | ) ()` of (a) compiles. Its layout `outer items depth extra` has the row's ivars as a prefix (Review Focus 2). A class declaring `cborWalk = native cborList` whose own layout is `outer items` (too short) refuses.
  - Also (e): an app method using the symbol `#cborList` still compiles to an *app* symbol (`_symbol_ref` must not hit NATIVES; Review Focus 1).

  Run them → FAIL (parse error on `native`).

- [ ] **Step 2: Grammar + AST.**

```
  MethodDef
    = visibility? MethodPattern "=" nativeKw identifier            -- native
    | visibility? MethodPattern "=" "(" Temps? Statements ")"     -- body
  nativeKw = "native" ~identifierPart
```

In `ns-parse.js`, rename `MethodDef` to `MethodDef_body` (unchanged) and add:

```js
  MethodDef_native(vis, pattern, _eq, _kw, name) {
    const sig = pattern.toAST();
    return { type: 'method', selector: sig.selector, params: sig.params,
             visibility: vis.numChildren ? vis.children[0].sourceString : null,
             temps: [], body: [], native: name.sourceString, span: span(this) };
  },
```

Check that `native` is not already an identifier anywhere in `.nsl` sources (`rg -nw native --glob '*.nsl' nodus`): the keyword rule would break a slot or method of that name.

- [ ] **Step 3: Compiler.** At the top of the method loop in `compile_class`:

```python
            if m.get("native"):
                self.bind_native(vt, qname, m)
                continue
```

```python
    def bind_native(self, vt, qname, m):
        from builtins_registry import NATIVES
        name = m["native"]
        if name not in NATIVES:
            raise CompileError(f"{qname}>>{m['selector']}: unknown native '{name}' (builtins.def NATIVE rows)")
        if m["params"]:
            raise CompileError(f"{qname}>>{m['selector']}: native '{name}' binds a unary selector")
        self.solidify.add_native_binding(vt, m["selector"], name)
        self.native_bindings.append((qname, name))   # checked after finalize_slot_layouts
```

After `finalize_slot_layouts()` runs, check every class that *has* a native binding, whether declared or inherited (walk `class_parent`):

```python
        for qname, name in self.native_bindings_including_inherited():
            want = ["outer", *NATIVES[name][1]]
            got = self.class_slots.get(qname, [])
            if got[:len(want)] != want:
                raise CompileError(f"native '{name}' reads slots {want} but class {qname} "
                                   f"has {got} — a shape edit is a builtins.def edit (bump the registry version)")
```

(Use the compiler's existing error type and exit path: copy what `test_compile_diagnostics.py` asserts. Print `self.class_slots['Kernel.List']` once to confirm that slot 0 is named `outer`. If it is not, adjust `want` and say so in the commit.)

- [ ] **Step 4: Emission.** `solidify.py`:

```python
    def add_native_binding(self, vtable_name, selector, native):
        self.vtables[vtable_name]['bindings'].append((selector, ("NATIVE", native)))
```

and in `generate_c`'s bindings loop: `method_ref = f"(oop)&Native_{mid[1]}" if isinstance(mid, tuple) else f"(oop)&{mid}_closure"`. In `oimg_serialize.py` at :708: `ref = RefWord.builtin(NATIVES[mid[1]][0]) if isinstance(mid, tuple) else self._aref(f"cl:{mid}")`. Skip tuple ids wherever `method_id` is looked up as `cl:`/`cm:` (`method_name_map` :497 and the reserve/offset loops; grep `bindings"]` for every site). Run the Step 1 tests → PASS. Then run `python3 -m pytest nodus/nsl/tools -q` → all green.

- [ ] **Step 5: Commit.** `git commit -m "nsl(#297): sel = native <name> — registry-bound, slot shape checked at bake"`

### Task 3: Kernel — the four bindings, `encode:` → C with oracle fallback, the oracle gate

**Files:**
- Modify: `nodus/nsl/lib/kernel.nsl`. List (:330), Map (:391), Record (:527) and Bytes (:616) each get one line. `Cbor` (:732) gets an `encode:` split and its class comment updated.
- Create: `nodus/tests/14_cbor/07_native_oracle.nsl`, `nodus/tests/14_cbor/08_native_fallback.nsl`, `nodus/tests/14_cbor/09_native_depth.nsl`
- Modify: `nodus/tests/14_cbor/README.md` (one paragraph: two encoders, one gate)

**Interfaces:**
- Consumes: `platform cborEncode:` (Task 1), `native` form (Task 2).
- Produces: `public encodeOracle: v` on `Cbor` (the old `encode:` body, renamed only), for tests and the board bench.

- [ ] **Step 1: Failing tests.** `07_native_oracle.nsl` reuses the spike's payload builders, copied verbatim from `CborPrimArms.nsl` on `297-cbor-spike`: `health`, `metrics`, `logRec`, `batch`, `ints64`, `map8`, `mixed`. It adds `02_golden_hex`'s map and a map-keyed map `{ {#b->1, #a->2} -> 1, #z -> 2 }` (Review Focus 3), a List subclass instance (Review Focus 2) and a 31-deep List tree with a leaf. For each payload it prints `(C encode: p) printHex = (C encodeOracle: p) printHex`. Expected: one `true` per payload, followed by `batch`'s byte count (`425`). The count anchors the payload, so a silently-shrunk frame fails.

`08_native_fallback.nsl` defines a class with `valueRank = ( ^ 7 )`, `do:` over a 2-element Array `{1. 2}`, and no `cborWalk`. Expect `8201` (the oracle's bytes) printed from `encode:`. That proves the refusal path end to end.

`09_native_depth.nsl` encodes `nest: 32` from `08_messages/10_depth_bound.nsl` (leaf at d=33) and expects `expect-runtime-fail: message depth exceeded` (Review Focus 4).

Run each: `cd nodus/nsl && python3 tools/run-one.py ../tests/14_cbor/07_native_oracle.nsl B` → FAIL (`encodeOracle:` not understood).

- [ ] **Step 2: Kernel.** Add `cborWalk = native cborList` to List, and likewise `cborMap` / `cborRecord` / `cborBytes`, each with a one-line comment pointing to builtins.def. In `Cbor`:

```
    (* #297: one C walk (platform cborEncode:), dispatched through each value
       class's `cborWalk` NATIVE binding. C answers nil on ANY doubt — no binding,
       depth, NaN, an unrecognised object — and the oracle below then produces the
       exact bytes or the exact error, so error texts and their order live only here. *)
    public encode: v = ( | raw |
      raw:: platform cborEncode: v.
      raw == nil ifTrue: [ ^ self encodeOracle: v ].
      ^ Bytes overRaw: raw )

    public encodeOracle: v = ( | acc |
      acc:: ByteBuilder new.
      self encode: v into: acc depth: 1.
      ^ acc frozenBytes )
```

(Check that `Bytes` resolves from inside `Cbor`, the way `freeze` at :580 reaches it. If it does not, use `outer Bytes`.)

- [ ] **Step 3: Run** the three new tests plus all of `14_cbor/` and `22_spacesync/09_batch_size.nsl` with `run-one.py … B` → PASS.

- [ ] **Step 4: Falsify both guards (D5). Restore each one before moving on.**
  - In `cb_map`, flip the comparison (`> 0` → `< 0`) and rebuild → `07_native_oracle` must print `false` for `map8`/`mixed`/`batch`. Restore.
  - Swap `| items depth |` to `| depth items |` in kernel List → the bake must refuse, naming `cborList` and `Kernel.List`. Restore. (Path-A bakes clobber `src/solidified_image.c`: check `git status` and run `git checkout nodus/oevm/src/solidified_image.c` if it changed.)
  - Record both outputs (one line each) for the commit body.

- [ ] **Step 5: Commit.** `git commit -m "kernel(#297): Cbor encode: is the C walk; encodeOracle: is the fallback and the gate"`

### Task 4: Gates

- [ ] **Step 1:** Run `free -g` (≥ 2 GB free), then run the suite in the background to a file:
  `cd nodus/nsl && python3 -u tools/run_tests_oevm.py > /tmp/claude-1000/suite-297.log 2>&1` (with `run_in_background`). Read **both** the `Passed: N / Failed: 0` line and the aux-gate line. A SKIP is a finding. Expected count: 288 + 3 = 291.
- [ ] **Step 2:** After it finishes, run `cd porta && go test ./...` and then, alone, `cd nodus/oevm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`.
- [ ] **Step 3:** Update the CLAUDE.md gate count line (`291 passed … (date)`). Commit `test(#297): gates green — suite N/0, porta, PORTA SYNC OK`.

### Task 5: Board — price and stack (one flash, `c6spare`, never `c6bench`)

**Files:** Modify `nodus/oevm/zephyr/CborBenchNodeC6.nsl`.

- [ ] **Step 1:** Make the bench time **both** arms in one binary: `cbor encode:` (C) and `cbor encodeOracle:` (nsl). That is now a within-binary delta, so delete the "CROSS-BINARY, DELIBERATELY" paragraph and the two-checkout recipe, and say why in their place. Fix #212 in the same edit. Replace both `#bid -> 1` sites (:52, :85) with `#bid -> 1073741823` (2^30−1, the band ceiling: the 5-byte worst case a real node sends). State in the header that this deliberately re-baselines, citing #212.
- [ ] **Step 2:** Resolve `c6spare` via the collector env files, then flash and read: `cd nodus/oevm/zephyr && ./run.sh --board esp32c6 --node CborBenchNodeC6.nsl --flash`. Record ms/encode for both arms on each payload and the printed byte counts. A board is required here: the host suite never compiles `zephyr/src/*.c`.
- [ ] **Step 3:** Read the VM stack high-water. Use the board's existing stack report (grep `stack` in `zephyr/src/` for the hook the bench prints, or add one `platform` print if none exists). The C recursion is ≤32 levels on an 8 KB main stack, and D4 says this is measured rather than assumed.
- [ ] **Step 4:** Run the `bridge-health` scenario on `c6spare` (`nodus/oevm/tests/target/run.sh bridge-health`; read `tests/target/CLAUDE.md` first for the `CONF_EXTRA`/radio rule). Read the `pushEncode` span from it. Compare it with the earlier ~53 ms.
- [ ] **Step 5:** Write `docs/results/2026-10-0X-297-cbor-native-walk-c6.md` (table: payload × arm × ms, stack high-water, `pushEncode` before/after, build id). Commit it, then post a ≤5-line comment on #297 and #212 citing the doc (`refs #297`).

### Task 6: Promotion + close-out

- [ ] `nodus/oevm/docs/vm.md`: a NATIVE row section covering what the kind is, the name-string ivar list, the bake shape check, and the unsendable entry.
- [ ] `nodus/nsl/docs/NEWSPEAK_DIALECT.md` + `nodus/nsl/docs/compiler.md`: the `sel = native <name>` form and its one user.
- [ ] `kernel.nsl` `Cbor` class comment: the C/oracle split (done in Task 3; verify that it reads as current).
- [ ] For David (review/ is not edited here): the program-sequence 2026-07-17 OEOM addendum has its first production user. List this in the close-out comment.
- [ ] Re-file the deferrals as their own issues if they are not already tracked: decode's `rawBytesOf:` (profile first) and #161 fix (1) re-priced against the new encode.
- [ ] Run `tools/docmap.sh`. Commit `docs(#297): NATIVE rows and native <name> promoted to the mainline`. Close #297 only once the board doc has landed.
