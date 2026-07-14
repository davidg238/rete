# Decoupled Image + Execute-In-Place Format — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move oevm from Model A (image `#include`d into the VM binary) to Model B: a standalone VM that memory-maps a separately-built `.oimg` program image and executes it in place.

**Architecture:** The VM firmware owns the *kernel* objects (Integer/Float/MethodContext/Fiber vtables, their methods, core symbols) and constructs them at boot into a `builtin_registry[]`. A host serializer walks the existing `solidify.py` object graph and emits an `.oimg` blob containing only the *app* graph in the target's native struct layout, where every reference is a tagged word (app byte-offset / builtin index / immediate SMI / singleton). The VM maps the blob read-only, resolves references at the field-read boundary, and runs — no decode/relocate step.

**Tech Stack:** C99 VM (`src/vm.c`, `src/dispatch.c`, `src/gc.c`); Python build tooling (`ns-oevm/tools/solidify.py`, `ns_compile_oevm.py`, `run_tests_oevm.py`); `gcc`; design spec at `docs/design/2026-06-17-clustered-image-xip-design.md`.

## Global Constraints

- **Format invariants:** little-endian; all blob objects **8-byte aligned** (covers `double` + 64-bit pointer alignment on both ABIs); references are word-sized (4 bytes on 32-bit targets, 8 on 64-bit). App-object references are **file-relative byte offsets** (`blob_base` = mmap base = file start); header `blob_size` = **total file size** (so `blob_end = blob_base + blob_size`).
- **NULL encoding:** a NULL pointer/ref field is stored as ref-word `0`; app-offset payload `0` is the reserved NULL sentinel (the header occupies file offset 0, so no object is ever at offset 0). `oimg_resolve` and the `is_ptr` accessors must special-case a stored `0` → `NULL`.
- **Reference tag (low 2 bits):** `00`=app-object (payload = byte offset into blob), `01`=builtin (payload = index into `builtin_registry[]`), `10`=small immediate SMI (payload = sign-extended value), `11`=singleton (payload = id: 0=nil, 1=true, 2=false).
- **SMI literal limit:** must fit signed `(wordbits−2)` bits (30-bit on 32-bit targets); serializer errors on overflow (bignum is out of scope).
- **Per-target builds:** one `.oimg` per target ABI; no single blob need serve both word sizes.
- **Blob is immutable:** blob objects are never written; runtime instances are `gc_allocate`d in RAM as today.
- **Model A retained:** the solidify-to-C fused build stays available as an opt-in appliance build; it must remain byte-for-byte unchanged in output through this work.
- **Two test paths, both gated:** Path A (existing solidify-to-C, full 36/42 suite) and Path B (new `.oimg`-loaded) must both pass.
- **Repos:** VM = `/home/david/workspace/oevm`; tooling + tests = `/home/david/workspace/ns-oevm` (tools) and `/home/david/workspace/ns-tests`. The harness finds the VM via `$OEVM_DIR` (defaults to the sibling `oevm`).
- **Regression gate command:** `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py` (expect `Passed: 36 / Failed: 5 / Skipped: 1`).
- **Magic / version:** header magic = ASCII `"OIMG"`; `format_version` starts at 1; `BUILTIN_REGISTRY_VERSION` starts at 1 and bumps whenever `builtins.def` changes.

---

## File Structure

**New (VM, `oevm/src/`):**
- `oimg.h` — format constants, header struct, ref-word tag macros, `oimg_resolve()` + field accessors (`obj_vtable`, `vtable_delegate`, …), `is_in_blob()`, `blob_base`/`blob_end` globals.
- `oimg_loader.c` — `oimg_load(path)` (mmap, header/version check, set `blob_base`), `oimg_root()`, `oimg_bootstrap()`.
- `kernel_boot.c` — `kernel_boot()` constructs kernel objects in RAM, fills `builtin_registry[]`; defines `builtin_registry[]` + `builtin_registry_count`.
- `builtins.def` — X-macro list of firmware-owned objects (singletons, kernel vtables, core symbols), the single source of truth. Lives in `oevm/src/` (consumed by both C and Python).

**New (VM, `oevm/`):**
- `oevm_image_main.c` — standalone `main(argc, argv)` for the loaded-image VM (replaces `oevm_main.c` in the default build).

**New (tooling, `ns-oevm/tools/`):**
- `gen_builtins.py` — reads `oevm/src/builtins.def`, emits the serializer-side `name→index` map module `builtins_registry.py`.
- `oimg_serialize.py` — `OimgSerializer(layout, registry)` with `generate_oimg(graph, entry)` producing the blob bytes; ref-word codec.

**New (VM, `oevm/tools/`):**
- `dump_layout.c` — prints `layout.json` (`sizeof`/`offsetof` per serialized struct) for the current target ABI.

**New (tests, `oevm/tests/`):**
- `test_oimg.c` — C unit tests for ref-word codec, `is_in_blob`, accessors, and a hand-built blob round-trip.

**Modified:**
- `oevm/src/oevm.h` — include `oimg.h`; no struct changes.
- `oevm/src/dispatch.c`, `oevm/src/vm.c` — route field reads through accessors.
- `oevm/src/gc.c` — skip the blob region during collection.
- `ns-oevm/tools/solidify.py` — expose graph for serialization (no behavior change to `generate_c`).
- `ns-oevm/tools/ns_compile_oevm.py` — add `--format=oimg` path emitting `.oimg`.
- `ns-oevm/tools/run_tests_oevm.py` — add Path B (loaded-image) execution; keep Path A.
- `oevm/tests/test_layout.c` — extend with 32-bit ABI assertions.

---

## Phase 0 — Foundations

### Task 1: Builtin registry source + generator

**Files:**
- Create: `oevm/src/builtins.def`
- Create: `ns-oevm/tools/gen_builtins.py`
- Create (generated): `ns-oevm/tools/builtins_registry.py`
- Test: `ns-oevm/tools/test_gen_builtins.py`

**Interfaces:**
- Produces (C, via `builtins.def` X-macros): `BUILTIN(index, kind, name)` entries; `BUILTIN_REGISTRY_VERSION`; `BUILTIN_COUNT`.
- Produces (Python): `builtins_registry.REGISTRY` = `dict[str,int]`, `builtins_registry.VERSION` = `int`.

The registry order is frozen; new entries append. `kind` ∈ {`SINGLETON`, `VTABLE`, `SYMBOL`}. Singletons occupy indices 0–2 (nil, true, false) but are referenced via the `singleton` tag, not the builtin tag; they appear here only so the registry is the single source of truth.

- [ ] **Step 1: Write `builtins.def`**

```c
/* builtins.def — single source of truth for firmware-owned objects an image
 * may reference. Order is FROZEN; append only. Bump BUILTIN_REGISTRY_VERSION
 * (defined in kernel_boot.c) whenever this list changes.
 *
 * BUILTIN(idx, kind, name)
 *   kind: SINGLETON | VTABLE | SYMBOL
 */
BUILTIN(0,  SINGLETON, "nil")
BUILTIN(1,  SINGLETON, "true")
BUILTIN(2,  SINGLETON, "false")
BUILTIN(3,  VTABLE,    "IntegerVtable")
BUILTIN(4,  VTABLE,    "FloatVtable")
BUILTIN(5,  VTABLE,    "MethodContextVtable")
BUILTIN(6,  VTABLE,    "FiberVtable")
BUILTIN(7,  VTABLE,    "SymbolVtable")
BUILTIN(8,  VTABLE,    "VtableVtable")
BUILTIN(9,  VTABLE,    "PrimitiveClosureVtable")
BUILTIN(10, VTABLE,    "BytecodeClosureVtable")
BUILTIN(11, VTABLE,    "BoxedBindingVtable")
BUILTIN(12, VTABLE,    "RunnableFiberVtable")
BUILTIN(13, VTABLE,    "SuspendedFiberVtable")
BUILTIN(14, VTABLE,    "DeadFiberVtable")
```

(Core symbols are appended in Task 11 once `kernel_boot` can intern them; the file is created here with the singleton+vtable rows so Task 1's generator and tests have stable input.)

- [ ] **Step 2: Write the failing test**

```python
# ns-oevm/tools/test_gen_builtins.py
import subprocess, sys, importlib.util
from pathlib import Path

TOOLS = Path(__file__).resolve().parent

def load_registry():
    spec = importlib.util.spec_from_file_location("builtins_registry", TOOLS / "builtins_registry.py")
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod

def test_generator_produces_stable_indices():
    subprocess.run([sys.executable, str(TOOLS / "gen_builtins.py")], check=True)
    reg = load_registry()
    assert reg.REGISTRY["IntegerVtable"] == 3
    assert reg.REGISTRY["FloatVtable"] == 4
    assert reg.REGISTRY["nil"] == 0
    assert reg.VERSION == 1
    assert len(reg.REGISTRY) == 15
```

- [ ] **Step 3: Run it to confirm it fails**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_gen_builtins.py -q`
Expected: FAIL (`gen_builtins.py` / `builtins_registry.py` not found).

- [ ] **Step 4: Write `gen_builtins.py`**

```python
# ns-oevm/tools/gen_builtins.py
import re, sys
from pathlib import Path

DEF = Path(__file__).resolve().parent.parent.parent / "oevm" / "src" / "builtins.def"
OUT = Path(__file__).resolve().parent / "builtins_registry.py"
VERSION = 1  # keep in lockstep with BUILTIN_REGISTRY_VERSION in kernel_boot.c

def parse():
    entries = []
    line_re = re.compile(r'BUILTIN\(\s*(\d+)\s*,\s*(\w+)\s*,\s*"([^"]*)"\s*\)')
    for line in DEF.read_text().splitlines():
        m = line_re.search(line)
        if m:
            entries.append((int(m.group(1)), m.group(2), m.group(3)))
    return entries

def main():
    entries = parse()
    for i, (idx, _kind, _name) in enumerate(entries):
        if idx != i:
            sys.exit(f"builtins.def indices must be dense and ordered: row {i} has index {idx}")
    body = ",\n".join(f'    {name!r}: {idx}' for idx, _k, name in entries)
    OUT.write_text(f"# GENERATED by gen_builtins.py — do not edit.\nVERSION = {VERSION}\nREGISTRY = {{\n{body}\n}}\n")
    print(f"wrote {OUT} ({len(entries)} entries)")

if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Run the test to confirm it passes**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_gen_builtins.py -q`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/oevm && git add src/builtins.def
cd /home/david/workspace/ns-oevm && git add tools/gen_builtins.py tools/builtins_registry.py tools/test_gen_builtins.py
git -C /home/david/workspace/oevm commit -m "feat(oimg): builtin registry source (builtins.def)"
git -C /home/david/workspace/ns-oevm commit -m "feat(oimg): builtin registry generator + serializer-side map"
```

---

### Task 2: Layout dumper

**Files:**
- Create: `oevm/tools/dump_layout.c`
- Test: `oevm/tools/test_dump_layout.sh`

**Interfaces:**
- Produces: a `layout.json` on stdout with `{"oop_size": N, "structs": {"Symbol": {"size": S, "fields": {"name": {"offset": O, "is_ref": false, "is_ptr": true}, ...}}, ...}}`.
- `is_ref` = field holds an `oop` (a tagged ref-word in the blob). `is_ptr` = field holds an internal C pointer to other blob bytes (`Symbol.name`, `CompiledMethod.instructions`/`constants`, `Vtable.bindings`, `Closure.entry_point`). Plain scalars have both false.

- [ ] **Step 1: Write the failing test**

```bash
# oevm/tools/test_dump_layout.sh
set -e
cd "$(dirname "$0")/.."
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dump_layout
/tmp/dump_layout > /tmp/layout.json
python3 - <<'PY'
import json
d = json.load(open("/tmp/layout.json"))
assert d["oop_size"] in (4, 8), d["oop_size"]
sym = d["structs"]["Symbol"]
assert sym["fields"]["name"]["is_ptr"] is True
assert sym["fields"]["base.vtable"]["is_ref"] is True
vt = d["structs"]["Vtable"]
assert vt["fields"]["delegate"]["is_ref"] is True
assert vt["fields"]["bindings"]["is_ptr"] is True
print("layout OK")
PY
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `bash /home/david/workspace/oevm/tools/test_dump_layout.sh`
Expected: FAIL (`dump_layout.c` not found).

- [ ] **Step 3: Write `dump_layout.c`**

```c
/* oevm/tools/dump_layout.c — emit native struct layout for the target ABI. */
#include <stdio.h>
#include <stddef.h>
#include "oevm.h"

/* F(struct, "field-path", is_ref, is_ptr) */
#define FIELDS \
  S(Symbol) \
    F(Symbol, "base.vtable", 1, 0, offsetof(struct Symbol, base.vtable)) \
    F(Symbol, "name",        0, 1, offsetof(struct Symbol, name)) \
    F(Symbol, "length",      0, 0, offsetof(struct Symbol, length)) \
  S(Vtable) \
    F(Vtable, "base.vtable", 1, 0, offsetof(struct Vtable, base.vtable)) \
    F(Vtable, "delegate",    1, 0, offsetof(struct Vtable, delegate)) \
    F(Vtable, "bindings",    0, 1, offsetof(struct Vtable, bindings)) \
    F(Vtable, "capacity",    0, 0, offsetof(struct Vtable, capacity)) \
    F(Vtable, "size",        0, 0, offsetof(struct Vtable, size)) \
    F(Vtable, "is_nested",   0, 0, offsetof(struct Vtable, is_nested)) \
  S(Binding) \
    F(Binding, "symbol",     1, 0, offsetof(struct Binding, symbol)) \
    F(Binding, "method",     1, 0, offsetof(struct Binding, method)) \
  S(CompiledMethod) \
    F(CompiledMethod, "instructions",     0, 1, offsetof(struct CompiledMethod, instructions)) \
    F(CompiledMethod, "instruction_count",0, 0, offsetof(struct CompiledMethod, instruction_count)) \
    F(CompiledMethod, "num_registers",    0, 0, offsetof(struct CompiledMethod, num_registers)) \
    F(CompiledMethod, "num_arguments",    0, 0, offsetof(struct CompiledMethod, num_arguments)) \
    F(CompiledMethod, "constants",        0, 1, offsetof(struct CompiledMethod, constants)) \
    F(CompiledMethod, "constant_count",   0, 0, offsetof(struct CompiledMethod, constant_count)) \
    F(CompiledMethod, "access_modifier",  0, 0, offsetof(struct CompiledMethod, access_modifier)) \
  S(Closure) \
    F(Closure, "base.vtable",     1, 0, offsetof(struct Closure, base.vtable)) \
    F(Closure, "home_ctx",        1, 0, offsetof(struct Closure, home_ctx)) \
    F(Closure, "home_generation", 0, 0, offsetof(struct Closure, home_generation)) \
    F(Closure, "entry_point",     0, 1, offsetof(struct Closure, entry_point)) \
  S(Float) \
    F(Float, "base.vtable", 1, 0, offsetof(struct Float, base.vtable)) \
    F(Float, "value",       0, 0, offsetof(struct Float, value)) \
  S(ObjectInstance) \
    F(ObjectInstance, "base.vtable", 1, 0, offsetof(struct ObjectInstance, base.vtable)) \
  S(BoxedBinding) \
    F(BoxedBinding, "base.vtable", 1, 0, offsetof(struct BoxedBinding, base.vtable)) \
    F(BoxedBinding, "symbol",      1, 0, offsetof(struct BoxedBinding, symbol)) \
    F(BoxedBinding, "method",      1, 0, offsetof(struct BoxedBinding, method))

int main(void) {
    printf("{\n  \"oop_size\": %zu,\n  \"structs\": {\n", sizeof(oop));
    int first_struct = 1, first_field = 1;
    #define S(name) \
        if (!first_struct) printf("\n    }},\n"); first_struct = 0; first_field = 1; \
        printf("    \"%s\": {\"size\": %zu, \"fields\": {", #name, sizeof(struct name));
    #define F(st, path, is_ref, is_ptr, off) \
        printf("%s\n      \"%s\": {\"offset\": %zu, \"is_ref\": %s, \"is_ptr\": %s}", \
               first_field ? "" : ",", path, (size_t)(off), \
               (is_ref) ? "true" : "false", (is_ptr) ? "true" : "false"); first_field = 0;
    FIELDS
    #undef S
    #undef F
    printf("\n    }}\n  }\n}\n");
    return 0;
}
```

Note: `ObjectInstance.slots` is a flexible array (no fixed offset); the serializer appends `nslots` ref-words after `sizeof(struct ObjectInstance)`. Same for `Symbol` string bytes and `CompiledMethod` arrays — these are trailing blobs the serializer places and points at via `is_ptr` fields.

- [ ] **Step 4: Run the test to confirm it passes**

Run: `bash /home/david/workspace/oevm/tools/test_dump_layout.sh`
Expected: prints `layout OK`.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm && git add tools/dump_layout.c tools/test_dump_layout.sh
git commit -m "feat(oimg): layout dumper for target ABI"
```

---

## Phase 1 — Host serializer

### Task 3: Ref-word codec + header writer

**Files:**
- Create: `ns-oevm/tools/oimg_serialize.py`
- Test: `ns-oevm/tools/test_oimg_serialize.py`

**Interfaces:**
- Produces: `class RefWord` with `@staticmethod app_offset(off)`, `builtin(idx)`, `smi(v)`, `singleton(id)`, each returning an `int` ref-word; `decode(word) -> (tag, payload)`.
- Produces: `OimgWriter(oop_size, version, registry_version)` with `.set_word_size(n)`, `.header_bytes(blob_size, cluster_count, root_ref)`, helpers `.u16/.u32/.word(v)` returning little-endian `bytes`.

- [ ] **Step 1: Write the failing test**

```python
# ns-oevm/tools/test_oimg_serialize.py
from oimg_serialize import RefWord

def test_refword_roundtrip():
    assert RefWord.decode(RefWord.app_offset(0x40)) == (0b00, 0x40)
    assert RefWord.decode(RefWord.builtin(4))      == (0b01, 4)
    assert RefWord.decode(RefWord.smi(-3))         == (0b10, -3)
    assert RefWord.decode(RefWord.singleton(0))    == (0b11, 0)

def test_app_offset_must_be_aligned():
    import pytest
    with pytest.raises(ValueError):
        RefWord.app_offset(0x41)  # not 4-aligned
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd /home/david/workspace/ns-oevm/tools && python3 -m pytest test_oimg_serialize.py -q`
Expected: FAIL (no module).

- [ ] **Step 3: Implement the codec**

```python
# ns-oevm/tools/oimg_serialize.py
import struct

TAG_APP, TAG_BUILTIN, TAG_SMI, TAG_SINGLETON = 0b00, 0b01, 0b10, 0b11

class RefWord:
    @staticmethod
    def app_offset(off):
        if off & 0b11:
            raise ValueError(f"app offset {off} not 4-aligned")
        return (off << 2) | TAG_APP
    @staticmethod
    def builtin(idx):  return (idx << 2) | TAG_BUILTIN
    @staticmethod
    def smi(v):        return ((v << 2) & 0xFFFFFFFFFFFFFFFF) | TAG_SMI if v < 0 else (v << 2) | TAG_SMI
    @staticmethod
    def singleton(i):  return (i << 2) | TAG_SINGLETON
    @staticmethod
    def decode(word):
        tag = word & 0b11
        payload = word >> 2
        if tag == TAG_SMI:
            # interpret as signed within the (oop-2)-bit field handled by caller width;
            # tests use small values so reconstruct sign from the python int directly:
            return (tag, payload if word >= 0 else -((-(word) ) >> 2))
        return (tag, payload)

class OimgWriter:
    MAGIC = b"OIMG"
    def __init__(self, oop_size, version, registry_version):
        self.oop_size = oop_size
        self.version = version
        self.registry_version = registry_version
    def word(self, v):
        return v.to_bytes(self.oop_size, "little", signed=False)
    def u16(self, v): return struct.pack("<H", v)
    def u32(self, v): return struct.pack("<I", v)
    def header_bytes(self, blob_size, cluster_count, root_ref):
        h  = self.MAGIC
        h += self.u16(self.version) + self.u16(0)                 # flags=0
        h += self.u32(self.registry_version)
        h += self.u32(blob_size) + self.u32(cluster_count)
        # pad root_ref (word-sized) to its natural alignment
        if len(h) % self.oop_size:
            h += b"\x00" * (self.oop_size - (len(h) % self.oop_size))
        h += self.word(root_ref)
        return h
```

(Implemented final form: `RefWord` is width-agnostic — `smi(v)` may yield a negative Python int; `decode(word) = (word & 3, word >> 2)` keeps SMI sign via Python's arithmetic right shift. `OimgWriter.word(v)` masks to two's-complement at `oop_size` width so negative SMI ref-words serialize unsigned. See `oimg_serialize.py` as built.)

- [ ] **Step 4: Run the test to confirm it passes**

Run: `cd /home/david/workspace/ns-oevm/tools && python3 -m pytest test_oimg_serialize.py -q`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/ns-oevm && git add tools/oimg_serialize.py tools/test_oimg_serialize.py
git commit -m "feat(oimg): ref-word codec + header writer"
```

---

### Task 4: Serialize the object graph

**Files:**
- Modify: `ns-oevm/tools/oimg_serialize.py`
- Test: `ns-oevm/tools/test_oimg_graph.py`

**Interfaces:**
- Consumes: `layout` (dict from `layout.json`), `registry` (`dict[str,int]` from `builtins_registry.REGISTRY`).
- Consumes from the graph (mirrors `solidify.py` fields): `symbols: dict[name->var]`, `vtables: dict[name->{delegate, bindings:[(sel,method_id)], meta, is_nested}]`, `methods: [{id, num_registers, num_arguments, constants, instructions, access_modifier}]`. Closures are 1:1 with methods (`<id>_closure` wraps `<id>_compiled`), as in `solidify.py generate_c`.
- Produces: `OimgSerializer(layout, registry).generate(graph, entry_vtable_name) -> bytes`.

**Serialization algorithm (two passes over a flat object list):**
1. **Collect** every app object into an ordered list with a kind tag: one Symbol per `graph.symbols` entry whose name is NOT in `registry`; one Vtable + its trailing Binding array per `graph.vtables`; one CompiledMethod (+ trailing instruction and constant arrays) and one Closure per `graph.methods`; one Float per distinct float literal in any method's constants; one ObjectInstance per literal instance (none in current tests — support the kind, emit zero).
2. **Assign offsets:** walk the list in cluster-kind order (SYMBOL, VTABLE+bindings, METHOD+arrays, CLOSURE, FLOAT, INSTANCE, BOXEDBINDING), 4-aligning each record and each trailing array; record `offset[obj_key]`.
3. **Emit records** using `layout` field offsets, writing each field: refs via `encode_ref()`, internal pointers (`name`, `instructions`, `constants`, `bindings`, `entry_point`) via app-offset to their trailing array, scalars packed at their `layout` offset and width.
4. **`encode_ref(value)`:** SMI literal → `RefWord.smi`; the names `"nil"/"true"/"false"` → `RefWord.singleton`; a name in `registry` (symbol or vtable) → `RefWord.builtin`; otherwise an app object → `RefWord.app_offset(offset[key])`. Error if an SMI exceeds the signed `(oop_size*8−2)`-bit range.
5. **Header:** `root_ref = encode_ref(entry_vtable_name)`; `blob_size` = total object-region bytes; `cluster_count` = number of non-empty kinds.

- [ ] **Step 1: Write the failing test (tiny hand-built graph round-trips)**

```python
# ns-oevm/tools/test_oimg_graph.py
import json, struct
from oimg_serialize import OimgSerializer, RefWord
from builtins_registry import REGISTRY, VERSION

LAYOUT = json.loads('''{"oop_size":8,"structs":{
 "Symbol":{"size":24,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "name":{"offset":8,"is_ref":false,"is_ptr":true},"length":{"offset":16,"is_ref":false,"is_ptr":false}}},
 "Vtable":{"size":48,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "delegate":{"offset":8,"is_ref":true,"is_ptr":false},"bindings":{"offset":16,"is_ref":false,"is_ptr":true},
   "capacity":{"offset":24,"is_ref":false,"is_ptr":false},"size":{"offset":32,"is_ref":false,"is_ptr":false},
   "is_nested":{"offset":40,"is_ref":false,"is_ptr":false}}},
 "Binding":{"size":16,"fields":{"symbol":{"offset":0,"is_ref":true,"is_ptr":false},
   "method":{"offset":8,"is_ref":true,"is_ptr":false}}},
 "CompiledMethod":{"size":56,"fields":{"instructions":{"offset":0,"is_ref":false,"is_ptr":true},
   "instruction_count":{"offset":8,"is_ref":false,"is_ptr":false},"num_registers":{"offset":16,"is_ref":false,"is_ptr":false},
   "num_arguments":{"offset":20,"is_ref":false,"is_ptr":false},"constants":{"offset":24,"is_ref":false,"is_ptr":true},
   "constant_count":{"offset":32,"is_ref":false,"is_ptr":false},"access_modifier":{"offset":40,"is_ref":false,"is_ptr":false}}},
 "Closure":{"size":32,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "home_ctx":{"offset":8,"is_ref":true,"is_ptr":false},"home_generation":{"offset":16,"is_ref":false,"is_ptr":false},
   "entry_point":{"offset":24,"is_ref":false,"is_ptr":true}}},
 "Float":{"size":16,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "value":{"offset":8,"is_ref":false,"is_ptr":false}}},
 "ObjectInstance":{"size":8,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false}}},
 "BoxedBinding":{"size":24,"fields":{"base.vtable":{"offset":0,"is_ref":true,"is_ptr":false},
   "symbol":{"offset":8,"is_ref":true,"is_ptr":false},"method":{"offset":16,"is_ref":true,"is_ptr":false}}}
}}''')

class Graph:  # mirrors solidify.py field names
    symbols = {"increment": "Symbol_increment"}
    vtables = {"CounterVtable": {"delegate": None, "bindings": [("increment", "Method_CounterVtable_increment")],
                                 "meta": None, "is_nested": 0}}
    methods = [{"id": "Method_CounterVtable_increment", "num_registers": 4, "num_arguments": 0,
                "constants": ["SMI:1"], "instructions": [0x0000010C, 0x00000200, 0x02010306, 0x0000030D, 0x00000304],
                "access_modifier": 0}]

def test_header_and_root():
    blob = OimgSerializer(LAYOUT, REGISTRY).generate(Graph, "CounterVtable")
    assert blob[:4] == b"OIMG"
    fmt_ver = struct.unpack_from("<H", blob, 4)[0]; assert fmt_ver == 1
    reg_ver = struct.unpack_from("<I", blob, 8)[0]; assert reg_ver == VERSION
    blob_size = struct.unpack_from("<I", blob, 12)[0]
    cluster_count = struct.unpack_from("<I", blob, 16)[0]
    assert cluster_count == 5  # SYMBOL, VTABLE, METHOD, CLOSURE, (FLOAT=0 omitted) ... actual non-empty kinds
    assert blob_size > 0

def test_increment_method_instructions_present():
    blob = OimgSerializer(LAYOUT, REGISTRY).generate(Graph, "CounterVtable")
    # the 5 instruction words appear verbatim somewhere in the blob
    needle = b"".join(struct.pack("<I", w) for w in Graph.methods[0]["instructions"])
    assert needle in blob
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd /home/david/workspace/ns-oevm/tools && python3 -m pytest test_oimg_graph.py -q`
Expected: FAIL (`OimgSerializer` not defined).

- [ ] **Step 3: Implement `OimgSerializer`**

Implement the five-step algorithm above in `oimg_serialize.py`. Concretely:
- Maintain `self.records` (list of `(kind, key, encoder_callable)`) and `self.offset` (dict). Use a helper `align4(n)`.
- Replace `RefWord.decode`/`smi` with a width-aware version stored on the serializer: `self.oop_size = layout["oop_size"]`; `smi(v)` raises `ValueError` if `not (-(1 << (oop_size*8-2)) <= v < (1 << (oop_size*8-2)))`.
- `encode_ref(value)` per Step 4; `value` is either an `int` (SMI), or a `str` name resolved against `registry`/`graph`/singletons/app offsets.
- Build trailing arrays (symbol bytes NUL-padded to 4; instruction `u32[]`; constants `word[]` of encoded refs; binding `Binding[]` records) and point the owning record's `is_ptr` field at them.
- Emit each record by writing `sizeof` zero-bytes then overwriting each field at its `layout` offset/width.

Cluster count in the header counts only non-empty kinds (the `FLOAT`/`INSTANCE`/`BOXEDBINDING` kinds are omitted when empty — adjust the test's `== 5` to match your kind set: SYMBOL, VTABLE, METHOD, CLOSURE present → but trailing arrays are not separate clusters; they live inside their owner's cluster body. So 4 kinds. Set the assertion to the number your implementation emits and keep it consistent.)

- [ ] **Step 4: Run the test to confirm it passes**

Run: `cd /home/david/workspace/ns-oevm/tools && python3 -m pytest test_oimg_graph.py -q`
Expected: PASS (fix the `cluster_count` assertion to your actual non-empty-kind count during Step 3).

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/ns-oevm && git add tools/oimg_serialize.py tools/test_oimg_graph.py
git commit -m "feat(oimg): serialize object graph to native-layout blob"
```

---

### Task 5: Wire `--format=oimg` into the compiler driver

**Files:**
- Modify: `ns-oevm/tools/ns_compile_oevm.py:1336-1500` (the `main()` + `patched_generate_c` region)
- Modify: `ns-oevm/tools/solidify.py` (add a read-only `graph()` accessor; no behavior change)
- Test: `ns-oevm/tools/test_compile_oimg.py`

**Interfaces:**
- Produces: `python3 ns_compile_oevm.py <src.ns> <out> --format=oimg --layout=<layout.json>` writes a valid `.oimg` to `<out>`; default `--format=c` is unchanged.
- Consumes: the entry-class vtable name, already determined in `main()` (it emits `Instantiate entry class <Name>` in the C path). Capture that name into a variable `entry_vtable_name` and pass it to the serializer.

- [ ] **Step 1: Write the failing test**

```python
# ns-oevm/tools/test_compile_oimg.py
import subprocess, sys, struct, tempfile, os
from pathlib import Path
TOOLS = Path(__file__).resolve().parent
OEVM = TOOLS.parent.parent / "oevm"

def test_compile_counter_to_oimg(tmp_path):
    # build layout.json for host ABI
    subprocess.run(["gcc","-std=c99","-Isrc","tools/dump_layout.c","-o",str(tmp_path/"dl")], cwd=OEVM, check=True)
    layout = tmp_path/"layout.json"
    layout.write_bytes(subprocess.run([str(tmp_path/"dl")], capture_output=True, check=True).stdout)
    src = TOOLS.parent.parent / "ns-tests" / "04_somns_basic" / "01_method_call.ns"  # known-good minimal class+send
    out = tmp_path/"counter.oimg"
    r = subprocess.run([sys.executable, str(TOOLS/"ns_compile_oevm.py"), str(src), str(out),
                        "--format=oimg", f"--layout={layout}"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert out.read_bytes()[:4] == b"OIMG"
```

(Use whatever minimal `.ns` the existing parser accepts; copy a known-good tiny test from `ns-tests/04_somns_basic` if the inline source does not parse.)

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd /home/david/workspace/ns-oevm/tools && python3 -m pytest test_compile_oimg.py -q`
Expected: FAIL (`--format` unrecognized → C file written, magic mismatch).

- [ ] **Step 3: Add argument parsing + the oimg branch**

In `ns_compile_oevm.py main()`: parse `--format` (default `c`) and `--layout` from `sys.argv` (filter them out before the positional `src`/`out`). After the graph is built and `entry_vtable_name` known:

```python
if fmt == "oimg":
    import json
    from oimg_serialize import OimgSerializer
    from builtins_registry import REGISTRY
    layout = json.load(open(layout_path))
    class _G:  # adapt solidify graph to the serializer's expected attributes
        symbols = compiler.solidify.symbols
        vtables = compiler.solidify.vtables
        methods = compiler.solidify.methods
    blob = OimgSerializer(layout, REGISTRY).generate(_G, entry_vtable_name)
    open(out_c, "wb").write(blob)
    print(f"OIMG written to {out_c}")
    return
```

Keep the existing C path as the `else`.

- [ ] **Step 4: Run the test to confirm it passes**

Run: `cd /home/david/workspace/ns-oevm/tools && python3 -m pytest test_compile_oimg.py -q`
Expected: PASS.

- [ ] **Step 5: Verify the C path is unchanged**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `Passed: 36 / Failed: 5 / Skipped: 1` (unchanged).

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/ns-oevm && git add tools/ns_compile_oevm.py tools/solidify.py tools/test_compile_oimg.py
git commit -m "feat(oimg): --format=oimg path in compiler driver"
```

---

## Phase 2 — VM execute-in-place runtime

### Task 6: `oimg.h` — globals, `is_in_blob`, ref-word resolve

**Files:**
- Create: `oevm/src/oimg.h`
- Create: `oevm/src/oimg_loader.c` (just the globals + `is_in_blob` for now)
- Modify: `oevm/src/oevm.h` (add `#include "oimg.h"` near the top, after the typedefs of `oop`)
- Test: `oevm/tests/test_oimg.c`

**Interfaces:**
- Produces: globals `extern const uint8_t *oimg_blob_base; extern const uint8_t *oimg_blob_end;`
- Produces: `static inline int is_in_blob(const void *p)` — `p >= base && p < end`.
- Produces: tag macros `OIMG_TAG(w)`, `OIMG_PAYLOAD(w)`; `oop oimg_resolve(uintptr_t word)` — app→`(oop)(base+payload)`, builtin→`builtin_registry[payload]`, smi→`TAG_SMI(sign_extend(payload))`, singleton→`oimg_singleton[payload]`.
- Produces: `extern oop builtin_registry[]; extern oop oimg_singleton[3];` (defined in `kernel_boot.c`, Task 11; for this task stub them in `oimg_loader.c`).

- [ ] **Step 1: Write the failing test**

```c
/* oevm/tests/test_oimg.c */
#include "../src/oevm.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t fake[64] = {0};
    oimg_blob_base = fake; oimg_blob_end = fake + sizeof(fake);
    assert(is_in_blob(fake + 8));
    assert(!is_in_blob(fake - 1));
    assert(!is_in_blob(fake + 64));
    /* smi ref-word resolves to a tagged SMI */
    uintptr_t w = ((uintptr_t)7 << 2) | 0b10;
    assert(IS_SMI(oimg_resolve(w)) && UNTAG_SMI(oimg_resolve(w)) == 7);
    /* app ref-word resolves to base+offset */
    uintptr_t a = ((uintptr_t)8 << 2) | 0b00;
    assert((const uint8_t*)oimg_resolve(a) == fake + 8);
    printf("test_oimg basic PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_oimg.c src/oimg_loader.c -o /tmp/t_oimg`
Expected: FAIL (compile error — `oimg.h` / symbols missing).

- [ ] **Step 3: Write `oimg.h` + loader globals**

```c
/* oevm/src/oimg.h */
#ifndef OIMG_H
#define OIMG_H
#include <stdint.h>
#include <stddef.h>

extern const uint8_t *oimg_blob_base;
extern const uint8_t *oimg_blob_end;
extern oop builtin_registry[];
extern oop oimg_singleton[3];

static inline int is_in_blob(const void *p) {
    return (const uint8_t *)p >= oimg_blob_base && (const uint8_t *)p < oimg_blob_end;
}

#define OIMG_TAG(w)     ((w) & 0x3u)
#define OIMG_PAYLOAD(w) ((intptr_t)(w) >> 2)   /* arithmetic shift: sign-extends SMI */
#define OIMG_TAG_APP 0u
#define OIMG_TAG_BUILTIN 1u
#define OIMG_TAG_SMI 2u
#define OIMG_TAG_SINGLETON 3u

static inline oop oimg_resolve(uintptr_t word) {
    switch (OIMG_TAG(word)) {
        /* app_offset encodes (file_offset << 2) | 0 (Task 3); file_offset = word >> 2.
         * word == 0 is the reserved NULL sentinel (no object lives at offset 0). */
        case OIMG_TAG_APP:       return word ? (oop)(oimg_blob_base + (word >> 2)) : NULL;
        case OIMG_TAG_BUILTIN:   return builtin_registry[word >> 2];
        case OIMG_TAG_SMI:       return TAG_SMI(OIMG_PAYLOAD(word));
        default:                 return oimg_singleton[word >> 2];
    }
}
#endif
```

Note (from Phase 1): offsets are **file-relative**, so `oimg_blob_base` is the
mmap base (file start), not the object-region start — Task 10 sets it accordingly.
The `is_ptr` accessors in Task 7 must likewise return `NULL` when the stored word
is `0` (e.g. a vtable with no `bindings`, a closure's NULL `home_ctx`).

In `oimg_loader.c`:

```c
/* oevm/src/oimg_loader.c */
#include "oevm.h"
const uint8_t *oimg_blob_base = NULL;
const uint8_t *oimg_blob_end  = NULL;
/* Stubs until kernel_boot.c (Task 11) provides the real registry. */
oop builtin_registry[64] = {0};
oop oimg_singleton[3] = {0};
```

Add `#include "oimg.h"` in `oevm.h` after the `oop` typedef and `TAG_SMI`/`IS_SMI` macros (so `oimg_resolve` sees them).

- [ ] **Step 4: Run the test to confirm it passes**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_oimg.c src/oimg_loader.c -o /tmp/t_oimg && /tmp/t_oimg`
Expected: prints `test_oimg basic PASS`.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm && git add src/oimg.h src/oimg_loader.c src/oevm.h tests/test_oimg.c
git commit -m "feat(oimg): blob globals, is_in_blob, ref-word resolve"
```

---

### Task 7: Field-read accessors

**Files:**
- Modify: `oevm/src/oimg.h` (add accessors)
- Modify: `oevm/tests/test_oimg.c` (hand-built blob exercising accessors)

**Interfaces (each is identity for RAM objects, resolves for blob objects):**
- `oop obj_vtable(oop o)` — `is_in_blob(o) ? oimg_resolve((uintptr_t)((struct Object*)o)->vtable) : ((struct Object*)o)->vtable`
- `oop vtable_delegate(oop vt)`, `struct Binding *vtable_bindings(oop vt)` (resolves the `bindings` pointer via `base+offset`), `oop binding_symbol(struct Binding *b, oop owner_vt)`, `oop binding_method(struct Binding *b, oop owner_vt)` — note Binding refs resolve relative to the blob when `is_in_blob(b)`.
- `const uint32_t *cm_instructions(struct CompiledMethod *m)`, `oop cm_const_at(struct CompiledMethod *m, size_t i)` (resolves both the `constants` pointer and the ref-word at `[i]`).
- `void *closure_entry(struct Closure *c)`, `const char *symbol_name(struct Symbol *s)`, `oop instance_slot(struct ObjectInstance *o, size_t i)`.

Rule for `is_ptr` fields (internal C pointers): when the owning object is in the blob, the stored value is a byte offset → return `blob_base + offset`; otherwise return the raw pointer.

- [ ] **Step 1: Add a failing accessor test**

```c
/* append to tests/test_oimg.c main() before the final return */
{
    /* Hand-lay a Symbol whose vtable field is a builtin ref (index 7=SymbolVtable)
       and whose name is an app-offset to trailing bytes. */
    static uint8_t blob[64] = {0};
    oimg_blob_base = blob; oimg_blob_end = blob + sizeof(blob);
    struct Symbol *s = (struct Symbol *)blob;
    /* base.vtable = builtin(7) */
    *(uintptr_t*)((uint8_t*)s + offsetof(struct Symbol, base.vtable)) = ((uintptr_t)7 << 2) | OIMG_TAG_BUILTIN;
    /* name = app-offset to byte 32 */
    size_t name_off = 32; const char *txt = "hi";
    blob[32] = 'h'; blob[33] = 'i'; blob[34] = 0;
    *(uintptr_t*)((uint8_t*)s + offsetof(struct Symbol, name)) = ((uintptr_t)name_off << 2) | OIMG_TAG_APP;
    extern oop builtin_registry[]; oop marker = (oop)0xABCD; builtin_registry[7] = marker;
    assert(obj_vtable((oop)s) == marker);
    assert(symbol_name(s)[0] == 'h' && symbol_name(s)[1] == 'i');
    (void)txt;
    printf("test_oimg accessors PASS\n");
}
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_oimg.c src/oimg_loader.c -o /tmp/t_oimg`
Expected: FAIL (`obj_vtable`/`symbol_name` undefined).

- [ ] **Step 3: Add accessors to `oimg.h`**

```c
static inline oop obj_vtable(oop o) {
    if (IS_SMI(o) || o == NULL) return NULL;
    struct Object *ob = (struct Object *)o;
    return is_in_blob(o) ? oimg_resolve((uintptr_t)ob->vtable) : ob->vtable;
}
static inline const char *symbol_name(struct Symbol *s) {
    return is_in_blob(s) ? (const char *)(oimg_blob_base + ((uintptr_t)s->name >> 2))
                         : s->name;
}
static inline oop vtable_delegate(oop vt) {
    struct Vtable *v = (struct Vtable *)vt;
    return is_in_blob(vt) ? oimg_resolve((uintptr_t)v->delegate) : v->delegate;
}
static inline struct Binding *vtable_bindings(oop vt) {
    struct Vtable *v = (struct Vtable *)vt;
    return is_in_blob(vt) ? (struct Binding *)(oimg_blob_base + ((uintptr_t)v->bindings >> 2))
                          : v->bindings;
}
static inline oop binding_symbol(struct Binding *b) {
    return is_in_blob(b) ? oimg_resolve((uintptr_t)b->symbol) : b->symbol;
}
static inline oop binding_method(struct Binding *b) {
    return is_in_blob(b) ? oimg_resolve((uintptr_t)b->method) : b->method;
}
static inline const uint32_t *cm_instructions(struct CompiledMethod *m) {
    return is_in_blob(m) ? (const uint32_t *)(oimg_blob_base + ((uintptr_t)m->instructions >> 2))
                         : m->instructions;
}
static inline oop cm_const_at(struct CompiledMethod *m, size_t i) {
    if (!is_in_blob(m)) return m->constants[i];
    const uintptr_t *c = (const uintptr_t *)(oimg_blob_base + ((uintptr_t)m->constants >> 2));
    return oimg_resolve(c[i]);
}
static inline void *closure_entry(struct Closure *c) {
    return is_in_blob(c) ? (void *)(oimg_blob_base + ((uintptr_t)c->entry_point >> 2))
                         : c->entry_point;
}
static inline oop instance_slot(struct ObjectInstance *o, size_t i) {
    return is_in_blob(o) ? oimg_resolve((uintptr_t)o->slots[i]) : o->slots[i];
}
```

- [ ] **Step 4: Run the test to confirm it passes**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_oimg.c src/oimg_loader.c -o /tmp/t_oimg && /tmp/t_oimg`
Expected: prints both `PASS` lines.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm && git add src/oimg.h tests/test_oimg.c
git commit -m "feat(oimg): field-read accessors (identity for RAM objects)"
```

---

### Task 8: Route VM field reads through accessors

**Files:**
- Modify: `oevm/src/dispatch.c` (all object-field reads)
- Modify: `oevm/src/vm.c` (LD_CONST/LD_SLOT/ST_SLOT/SEND vtable reads, bytecode fetch via `cm_instructions`)

**Interfaces:** none new; behavior identical for RAM objects, so the Model A suite is the gate.

This is a mechanical, behavior-preserving sweep. The transformation rule:

| Direct read | Replace with |
|---|---|
| `X->vtable` (X a heap oop) | `obj_vtable((oop)X)` |
| `receiver->vtable == FloatVtable` (and `== SymbolVtable`, etc.) | `obj_vtable(receiver) == FloatVtable` |
| `vt->delegate` | `vtable_delegate((oop)vt)` |
| `vt->bindings` | `vtable_bindings((oop)vt)` |
| `vt->bindings[i].symbol` / `.method` | `binding_symbol(&vtable_bindings((oop)vt)[i])` / `binding_method(...)` |
| `cl->entry_point` (read) | `closure_entry(cl)` |
| `sym->name` | `symbol_name(sym)` |
| `m->instructions` (bytecode fetch) | `cm_instructions(m)` |
| `m->constants[i]` | `cm_const_at(m, i)` |
| `inst->slots[i]` (read) | `instance_slot(inst, i)` |

Leave WRITES untouched: instances are always RAM (`gc_allocate`), so `inst->slots[i] = v` and `cl->entry_point = ...` (the `primitive_compiled_method_set_instruction_at` path, dispatch.c:396) operate on RAM objects only. The immutability guard (Task 15) asserts this.

- [ ] **Step 1: Enumerate the sites**

Run: `cd /home/david/workspace/oevm && grep -nE -e '->vtable' -e '->delegate' -e '->bindings' -e '->instructions' -e '->constants\[' -e '->name' -e 'entry_point' -e '->slots\[' src/dispatch.c src/vm.c`
Record the count; these are the sites to convert (the survey at plan-writing time found ~40 in dispatch.c).

- [ ] **Step 2: Convert `dispatch.c`**

Apply the table to every read site. Worked examples (real lines):
- `dispatch.c:35` `if (obj != NULL && obj->vtable == SymbolVtable)` → `if (obj != NULL && obj_vtable(obj) == SymbolVtable)`
- `dispatch.c:51-52` `if (vt->bindings[i].symbol == selector) return vt->bindings[i].method;` → `struct Binding *bs = vtable_bindings((oop)vt); if (binding_symbol(&bs[i]) == selector) return binding_method(&bs[i]);`
- `dispatch.c:55` `current = vt->delegate;` → `current = vtable_delegate((oop)vt);`
- `dispatch.c:123` `oop (*func)(...) = (...)cl->entry_point;` → `... = (...)closure_entry(cl);`
- `dispatch.c:131` `sym ? sym->name : "?"` → `sym ? symbol_name(sym) : "?"`; and the trailing `receiver->vtable` → `obj_vtable(receiver)`.
- `dispatch.c:488` etc. (`receiver->vtable == FloatVtable`) → `obj_vtable(receiver) == FloatVtable`.

- [ ] **Step 3: Convert `vm.c`**

Convert the interpreter loop: bytecode pointer comes from `cm_instructions(method)`; `LD_CONST` reads `cm_const_at(method, k)`; `LD_SLOT` reads `instance_slot(...)`; `SEND` receiver-vtable read uses `obj_vtable(...)`. Locate via the Step-1 grep on `vm.c`.

- [ ] **Step 4: Build and run the full Model A suite (the gate)**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `Passed: 36 / Failed: 5 / Skipped: 1` — **unchanged**. If any previously-passing test regresses, an accessor was applied to a non-heap value or a write site; revert that site.

- [ ] **Step 5: Run the C unit tests**

Run: `cd /home/david/workspace/oevm && for t in tests/test_oevm.c tests/test_access.c tests/test_mirrors.c; do gcc -std=c99 -Isrc "$t" src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c -lm -o /tmp/ut 2>/dev/null && /tmp/ut; done`
Expected: each existing unit test still passes (link now includes `oimg_loader.c`).

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/oevm && git add src/dispatch.c src/vm.c
git commit -m "refactor(oimg): route VM field reads through XIP accessors (no-op for RAM)"
```

---

### Task 9: GC skips the blob

**Files:**
- Modify: `oevm/src/gc.c`
- Test: extend `oevm/tests/test_oimg.c`

**Interfaces:** none new. The collector must (a) never free/move a blob object, (b) not recurse into blob objects (their refs reach only blob/builtin/smi — all permanent).

- [ ] **Step 1: Find the trace + sweep entry points**

Run: `cd /home/david/workspace/oevm && grep -nE 'mark|trace|sweep|free|gc_allocate|UPDATE_LIMITS|flash_rom' src/gc.c`
Identify the per-object mark routine and the sweep loop.

- [ ] **Step 2: Add the blob guard**

At the top of the mark/trace-one-object routine, before dereferencing fields for children:

```c
if (is_in_blob(obj)) return;   /* blob objects are immutable ROM: never marked-for-sweep, never traced */
```

And in the sweep loop, ensure blob objects are never candidates (they are not in the GC's allocation list because they were never `gc_allocate`d — confirm the sweep iterates only the RAM allocation list; if it scans an address range, exclude `[oimg_blob_base, oimg_blob_end)`).

When a RAM object's field points into the blob, tracing stops at the guard — correct, because the blob sub-graph is permanent and self-contained.

- [ ] **Step 3: Add a test (RAM object referencing a blob object survives GC)**

Construct a RAM `ObjectInstance` whose slot holds a pointer into a fake blob; run a collection; assert no crash and the RAM object's slot still points into the blob. (Reuse the `tests/test_oimg.c` harness; call the GC's collect entry point directly.)

- [ ] **Step 4: Run unit + full suite**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_oimg.c src/oimg_loader.c src/gc.c -lm -o /tmp/t_oimg && /tmp/t_oimg`
Then: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: unit PASS; suite `36/5/1` unchanged.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm && git add src/gc.c tests/test_oimg.c
git commit -m "feat(oimg): GC skips immutable blob region"
```

---

### Task 11: Kernel boot — construct builtins, fill registry

(Task 11 precedes Task 10 in build order because the loader's bootstrap calls into the kernel. Implement kernel_boot first.)

**Files:**
- Create: `oevm/src/kernel_boot.c`
- Modify: `oevm/src/builtins.def` (append the core SYMBOL rows now)
- Modify: `oevm/src/oimg_loader.c` (remove the `builtin_registry`/`oimg_singleton` stubs — kernel_boot now owns them)
- Test: `oevm/tests/test_kernel_boot.c`

**Interfaces:**
- Produces: `void kernel_boot(void)` — constructs the kernel vtables (reusing the existing `init_solidified_vtables` logic for the root meta-vtables, but in firmware, not per-image), interns the core symbols, and fills `builtin_registry[]` + `oimg_singleton[]` in `builtins.def` index order.
- Produces: `oop builtin_registry[BUILTIN_COUNT]; oop oimg_singleton[3]; const uint32_t BUILTIN_REGISTRY_VERSION = 1;`

The kernel objects are exactly those currently emitted into every `solidified_image.c`: `Vtable_VtableVtable`, `Vtable_SymbolVtable`, `Vtable_BytecodeClosureVtable`, `Vtable_PrimitiveClosureVtable`, `Vtable_BoxedBindingVtable`, the three Fiber vtables, `IntegerVtable`, `FloatVtable`, `MethodContextVtable` (+ its `unwindTo:value:` method — keep that compiled method in firmware), `FiberVtable`. Lift these definitions out of `solidify.py`'s generated tail into `kernel_boot.c` as static C (they are app-independent).

- [ ] **Step 1: Append core symbols to `builtins.def`**

Append (continuing the index sequence; bump `BUILTIN_REGISTRY_VERSION`/`gen_builtins.VERSION` to 2):

```c
BUILTIN(15, SYMBOL, "=")
BUILTIN(16, SYMBOL, "==")
BUILTIN(17, SYMBOL, "new")
BUILTIN(18, SYMBOL, "printString")
BUILTIN(19, SYMBOL, "value")
BUILTIN(20, SYMBOL, "unwindTo:value:")
/* … add every symbol kernel methods reference; mirror the StaticSymbol_* set
   that ns_compile_oevm.py emits for an empty program (see its init_reflection_symbols
   region ~lines 1459-1491). */
```

Regenerate: `cd /home/david/workspace/ns-oevm && python3 tools/gen_builtins.py`.

- [ ] **Step 2: Write the failing test**

```c
/* oevm/tests/test_kernel_boot.c */
#include "../src/oevm.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    kernel_boot();
    assert(builtin_registry[3] == IntegerVtable);
    assert(builtin_registry[4] == FloatVtable);
    assert(oimg_singleton[0] != NULL); /* nil */
    /* core symbol interned and registered */
    extern oop find_builtin_symbol(const char *);  /* optional helper, or compare names */
    printf("kernel_boot PASS\n");
    return 0;
}
```

- [ ] **Step 3: Run it to confirm it fails**

Run: `cd /home/david/workspace/oevm && gcc -std=c99 -Isrc tests/test_kernel_boot.c src/kernel_boot.c src/oimg_loader.c src/dispatch.c src/vm.c src/gc.c -lm -o /tmp/t_kb`
Expected: FAIL (`kernel_boot` undefined).

- [ ] **Step 4: Implement `kernel_boot.c`**

Move the kernel vtable/method/symbol C definitions (currently in `solidify.py generate_c` lines ~191-212 and `ns_compile_oevm.py` patched tail) into `kernel_boot.c`. Define `builtin_registry[]`/`oimg_singleton[]`/`BUILTIN_REGISTRY_VERSION`. `kernel_boot()` assigns the global vtable pointers (as `init_solidified_vtables` did), interns the core symbols (allocate `struct Symbol` in firmware static storage), and writes each into `builtin_registry[idx]` per `builtins.def` order. Delete the stub arrays from `oimg_loader.c`.

- [ ] **Step 5: Run the test to confirm it passes**

Run: the Step-3 command, then `/tmp/t_kb`.
Expected: prints `kernel_boot PASS`.

- [ ] **Step 6: Commit**

```bash
cd /home/david/workspace/oevm && git add src/kernel_boot.c src/builtins.def src/oimg_loader.c tests/test_kernel_boot.c
cd /home/david/workspace/ns-oevm && git add tools/builtins_registry.py tools/gen_builtins.py
git -C /home/david/workspace/oevm commit -m "feat(oimg): kernel_boot constructs builtins, fills registry"
git -C /home/david/workspace/ns-oevm commit -m "chore(oimg): core symbols in registry (v2)"
```

---

### Task 10: Loader + standalone main

**Files:**
- Modify: `oevm/src/oimg_loader.c` (add `oimg_load`, `oimg_root`, `oimg_bootstrap`)
- Create: `oevm/oevm_image_main.c`
- Test: `oevm/tests/test_load_run.sh`

**Interfaces:**
- Produces: `int oimg_load(const char *path)` — `mmap` the file read-only, validate magic + `format_version` + `builtin_registry_version == BUILTIN_REGISTRY_VERSION`, set `oimg_blob_base`/`oimg_blob_end` to the object region (after the header+directory), return 0 / non-zero on error.
- Produces: `oop oimg_root(void)` — decode the header `root_ref` via `oimg_resolve`.
- Produces: `oop oimg_bootstrap(oop entry_vtable)` — replicate the current convention (`run_solidified_entry_point`): `gc_allocate` an `ObjectInstance` of the entry vtable, send the initializer, then `send(inst, main:args:, …, {nil,nil})`, return the result.

**Gap found in Phase 1 — extend the root descriptor.** The init selector is *program-derived* (`init_<params>:` from the entry class params; `ns_compile_oevm.py:1480-1492`), not a fixed `init:manifest:`. The header currently carries only `root_ref`, which is insufficient. Before this task, **extend the header** with: `init_sel_ref` (ref-word to the initializer selector symbol — an app symbol already in the blob), `init_argc` (u16), and `main_sel_ref` (ref-word to `main:args:`). The serializer (Task 4/5) must emit these — `ns_compile` knows the entry class, its params (→ init selector + argc), and `main:args:`. `oimg_bootstrap` then resolves `init_sel_ref`/`main_sel_ref` and sends with `init_argc` nil args. The entry-instance slot count also comes from the entry vtable's class slots — carry it as `entry_nslots` (u16) too, since `gc_allocate` needs `sizeof(ObjectInstance) + entry_nslots*sizeof(oop)`. Update Task 6's header struct + the Task 3/Task 4 header writer accordingly when starting Phase 2.

The header stores `blob_size` for the *object region*; `oimg_blob_base` = mmap addr + header size + directory size; `oimg_blob_end` = base + blob_size. App-offset refs are relative to `oimg_blob_base` (the serializer must assign offsets from the object-region start — confirm consistency with Task 4 Step 2).

- [ ] **Step 1: Write the failing end-to-end test**

```bash
# oevm/tests/test_load_run.sh
set -e
cd "$(dirname "$0")/.."
NS=../ns-oevm
# layout for host
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl && /tmp/dl > /tmp/layout.json
# a known tiny test that prints a value (passing conformance test; expects "42")
SRC=../ns-tests/04_somns_basic/01_method_call.ns
python3 $NS/tools/ns_compile_oevm.py "$SRC" /tmp/app.oimg --format=oimg --layout=/tmp/layout.json
# build the image VM
gcc -Wall -std=c99 src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm
/tmp/oevm_img /tmp/app.oimg
```

- [ ] **Step 2: Run it to confirm it fails**

Run: `bash /home/david/workspace/oevm/tests/test_load_run.sh`
Expected: FAIL (`oevm_image_main.c` / loader funcs missing).

- [ ] **Step 3: Implement loader + main**

`oimg_loader.c`: add `oimg_load`/`oimg_root`/`oimg_bootstrap` per interfaces (use `mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0)`).

```c
/* oevm/oevm_image_main.c */
#include "src/oevm.h"
#include <stdio.h>
int oimg_load(const char *path);
oop oimg_root(void);
oop oimg_bootstrap(oop entry_vtable);
extern void (*gc_additional_roots_hook)(void);
static void roots_cb(void) {}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <image.oimg>\n", argv[0]); return 2; }
    gc_additional_roots_hook = roots_cb;
    kernel_boot();
    if (oimg_load(argv[1]) != 0) { fprintf(stderr, "failed to load %s\n", argv[1]); return 1; }
    oimg_bootstrap(oimg_root());
    return 0;
}
```

- [ ] **Step 4: Run the test to confirm it passes**

Run: `bash /home/david/workspace/oevm/tests/test_load_run.sh`
Expected: prints the program's expected output (same as Model A for that test).

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm && git add src/oimg_loader.c oevm_image_main.c tests/test_load_run.sh
git commit -m "feat(oimg): mmap loader + standalone image VM entry"
```

---

## Phase 3 — Integration, decoupling, conformance

### Task 12: Decouple the default build

**Files:**
- Modify: `oevm/src/oevm_main.c` (guard the solidified path behind `OEVM_APPLIANCE`)

**Interfaces:** the default build no longer compiles `solidified_image.c`; Model A is `gcc -DOEVM_APPLIANCE … oevm_main.c solidified_image.c`.

- [ ] **Step 1: Guard Model A**

Wrap `oevm_main.c`'s body so the `extern run_solidified_entry_point()` path compiles only under `-DOEVM_APPLIANCE`. Document both build lines in `oevm/docs/deployment-model.md` ("Suggested path" item 5: Model A is the appliance build).

- [ ] **Step 2: Verify both builds compile**

Run (Model B): `cd /home/david/workspace/oevm && gcc -Wall -std=c99 src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img -lm`
Run (Model A appliance): generate an image C first (`python3 ../ns-oevm/tools/ns_compile_oevm.py <a passing test> src/solidified_image.c`), then `gcc -DOEVM_APPLIANCE -std=c99 src/vm.c src/gc.c src/dispatch.c src/solidified_image.c src/oevm_main.c -lm -o /tmp/oevm_app && /tmp/oevm_app`
Expected: both build; appliance run matches Model A output.

- [ ] **Step 3: Commit**

```bash
cd /home/david/workspace/oevm && git add src/oevm_main.c docs/deployment-model.md
git commit -m "feat(oimg): decouple default build; Model A behind OEVM_APPLIANCE"
```

---

### Task 13: Path B in the test harness (both gated)

**Files:**
- Modify: `ns-oevm/tools/run_tests_oevm.py`

**Interfaces:** an env toggle `OEVM_PATH=B` (or run both by default) builds `.oimg` + the image VM and compares output, using the same `expect:` metadata.

- [ ] **Step 1: Add a Path-B runner function**

Add `run_test_file_oimg(src_path)` mirroring `run_test_file` but: (1) build host `layout.json` once (module-level cache); (2) compile with `--format=oimg --layout=…` to `/tmp/<name>.oimg`; (3) build the image VM once (cache the binary across tests — sources don't change); (4) run `oevm_img <image>`; (5) compare stdout against `expect`. Reuse `parse_test_metadata`.

- [ ] **Step 2: Drive both paths in `main()`**

For each test, run Path A (existing) and Path B; report a test as PASS only if both agree with `expect`. Print per-path status on failure. Keep the `expect_compile_fail`/`expect_runtime_fail`/`skip_oevm` handling identical.

- [ ] **Step 3: Run the suite**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: Path A `36/5/1`; Path B matches Path A on the 36 passing tests (the 5 known failures may differ — record which fail under B; the gate is that no Path-A pass regresses under B). Investigate any Path-B-only failure with the printf-bisect technique from `conformance-workstreams`.

- [ ] **Step 4: Commit**

```bash
cd /home/david/workspace/ns-oevm && git add tools/run_tests_oevm.py
git commit -m "test(oimg): add loaded-image path B; both paths gated"
```

---

### Task 14: 32-bit portability

**Files:**
- Modify: `oevm/tests/test_layout.c` (32-bit assertions)
- Create: `oevm/tests/test_portability_32.sh`

**Interfaces:** prove an `-m32` build serializes + loads + runs a subset of tests.

- [ ] **Step 1: Ensure `-m32` toolchain**

Run: `gcc -m32 -E -x c /dev/null >/dev/null 2>&1 && echo OK || echo "install gcc-multilib"`
If missing, document `sudo apt-get install gcc-multilib` as a prerequisite (do not install silently).

- [ ] **Step 2: Write the 32-bit portability test**

```bash
# oevm/tests/test_portability_32.sh
set -e
cd "$(dirname "$0")/.."
NS=../ns-oevm
gcc -m32 -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl32 && /tmp/dl32 > /tmp/layout32.json
python3 -c "import json;d=json.load(open('/tmp/layout32.json'));assert d['oop_size']==4,d['oop_size']"
SRC=../ns-tests/04_somns_basic/01_method_call.ns   # same minimal test as Task 10
python3 $NS/tools/ns_compile_oevm.py "$SRC" /tmp/app32.oimg --format=oimg --layout=/tmp/layout32.json
gcc -m32 -Wall -std=c99 src/vm.c src/gc.c src/dispatch.c src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_img32 -lm
/tmp/oevm_img32 /tmp/app32.oimg
echo "32-bit portability OK"
```

- [ ] **Step 3: Run it**

Run: `bash /home/david/workspace/oevm/tests/test_portability_32.sh`
Expected: prints the program output then `32-bit portability OK`. Fix any width assumptions in `oimg_serialize.py` (it must use `layout["oop_size"]` everywhere, never a hardcoded 8) and in the loader.

- [ ] **Step 4: Extend `test_layout.c`**

Add a parametric check that all `oop`-typed fields sit at `index * oop_size` for both word sizes; build it `-m32` and native in the test and assert.

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/oevm && git add tests/test_layout.c tests/test_portability_32.sh
git commit -m "test(oimg): 32-bit ABI portability (serialize+load+run)"
```

---

### Task 15: Immutability guard

**Files:**
- Modify: `oevm/src/dispatch.c` (the slot-write + `set_instruction_at` primitives)

**Interfaces:** a write to a blob object aborts loudly rather than faulting on read-only memory.

- [ ] **Step 1: Add the guard at write sites**

At `ST_SLOT` execution and `primitive_compiled_method_set_instruction_at` (dispatch.c:396), before writing:

```c
if (is_in_blob(target)) {
    fprintf(stderr, "VM Error: attempt to mutate immutable image object %p\n", (void*)target);
    abort();
}
```

- [ ] **Step 2: Verify no current test trips it**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py`
Expected: `36/5/1`; no "mutate immutable image object" message on any passing test (instances are RAM).

- [ ] **Step 3: Commit**

```bash
cd /home/david/workspace/oevm && git add src/dispatch.c
git commit -m "feat(oimg): guard against mutating immutable blob objects"
```

---

## Self-review notes (coverage map)

| Spec section | Task(s) |
|---|---|
| Kernel/app split | 11, 12 |
| Native layout / ABI-matched / layout dumper | 2, 14 |
| Tagged offset-word references | 3, 4, 6 |
| Field-read resolution (incl. internal C pointers) | 7, 8 |
| Builtin registry single source of truth | 1, 11 |
| Host serializer on solidify graph | 4, 5 |
| Kernel boot + standalone loader/main | 10, 11 |
| Execute-in-place mmap (no decode/relocate) | 10 |
| GC skips blob | 9 |
| Immutability constraint + guard | 15 |
| Parallel test paths, both gated | 13 |
| 32-bit portability via per-target builds | 14 |
| Model A retained as appliance build | 12 |

**Out of scope (per spec):** bignum/`integer_overflow`; inline caching + unboxed floats; ESP32/Zephyr + keeper seam; partial-graph messages; literal-mutation RAM promotion.
