# Source-Map Emission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `ns_compile_oevm.py` emit a `.stmap` source map (`.ns` line ↔ bytecode offset, per method) alongside every image it compiles, in the exact JSON format the `jast-gw` debug viewer already consumes — without changing any generated image.

**Architecture:** The Ohm parser already attaches a `span` (`{startOffset, endOffset, startLine, startCol}`) to every AST node (`grammar/ns-parse.js:16`). Today the compiler drops it. We thread it through: each `MethodCompiler` accumulates a `source_map` list of `(bytecode_offset, line)` pairs recorded at statement boundaries (mirroring `st-berry/transpiler/st_funcctx.py:166`); the `OevmCompiler` collects these per method into a dict; a new pure-function module `tools/stmap.py` serializes the dict to the st-berry `.stmap` JSON contract; and `main()` writes it as a sidecar next to the image for both the Path A (`.c`) and Path B (`.oimg`) backends. Recording is pure bookkeeping (appends to a Python list, emits no instruction or constant), so generated images stay byte-identical.

**Tech Stack:** Python 3 (`tools/ns_compile_oevm.py`, new `tools/stmap.py`), pytest (`tools/test_*.py` idiom: subprocess-drive the compiler on an `ns-tests` fixture, assert on output), Node Ohm parser (`grammar/ns-parse.js`, invoked as a subprocess).

## Global Constraints

- **Byte-identical images.** Source-map work MUST NOT change any generated `.c` or `.oimg`. Prove it with `tools/snapshot_images.py` (snapshot before/after, `diff -r`) plus the conformance gate.
- **Conformance unchanged.** `python3 tools/run_tests_oevm.py` must stay `Passed: 47, Failed: 0, Skipped: 1` on both paths.
- **`.stmap` format is fixed by the consumer.** Match the st-berry contract exactly (`st-berry/transpiler/st_serializer.py:155`):
  ```json
  { "source": "<basename.ns>", "functions": { "<Vtable>>selector>": [[offset, line], ...] } }
  ```
  `functions` values are arrays of two-element `[offset, line]` arrays. JSON `indent=2`.
- **Qualified method name** is `f"{vtable}>>{selector}"` for methods and `f"{blk.name}>>{blk.selector}"` for non-inlined blocks.
- **v1 scope:** instance methods, class-side methods, and non-inlined blocks. Synthetic methods with no source (accessors, default `init`) produce empty maps and are skipped — a debugger never steps into them. Statement-granularity (not per-instruction) maps, matching st-berry.

---

## File Structure

- `tools/stmap.py` *(new)* — pure serializer: `build_stmap(source_filename, source_maps) -> str`. One responsibility: turn the collected `{name: [(offset, line)]}` dict into `.stmap` JSON. No compiler imports, fully unit-testable.
- `tools/ns_compile_oevm.py` *(modify)* — add `source_map` accumulation to `MethodCompiler`, a `record_line` hook in `compile_statement`, a `source_maps` dict + `register_source_map` on `OevmCompiler`, registration calls at the three real-source sites, and the sidecar write in `main()`.
- `tools/test_stmap.py` *(new)* — unit tests for `build_stmap`.
- `tools/test_source_map.py` *(new)* — integration tests: in-process collection (Task 2) and the end-to-end `.stmap` sidecar file (Task 3).

---

## Task 1: `build_stmap` serializer

**Files:**
- Create: `tools/stmap.py`
- Test: `tools/test_stmap.py`

**Interfaces:**
- Produces: `build_stmap(source_filename: str, source_maps: dict[str, list[tuple[int, int]]]) -> str` — returns a JSON string `{"source": source_filename, "functions": {name: [[offset, line], ...]}}`. Tuples are emitted as two-element JSON arrays. Insertion order of `source_maps` is preserved.

- [ ] **Step 1: Write the failing test**

Create `tools/test_stmap.py`:

```python
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stmap import build_stmap


def test_build_stmap_shape():
    out = build_stmap("foo.ns", {"Bar>>baz": [(0, 3), (2, 4)]})
    doc = json.loads(out)
    assert doc == {
        "source": "foo.ns",
        "functions": {"Bar>>baz": [[0, 3], [2, 4]]},
    }


def test_build_stmap_empty():
    doc = json.loads(build_stmap("x.ns", {}))
    assert doc == {"source": "x.ns", "functions": {}}


def test_build_stmap_multiple_functions_preserve_order():
    sm = {"A>>m": [(0, 1)], "B>>n": [(0, 5), (1, 6)]}
    doc = json.loads(build_stmap("x.ns", sm))
    assert list(doc["functions"].keys()) == ["A>>m", "B>>n"]
    assert doc["functions"]["B>>n"] == [[0, 5], [1, 6]]
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_stmap.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'stmap'`.

- [ ] **Step 3: Write minimal implementation**

Create `tools/stmap.py`:

```python
"""Source-map (.stmap) serializer for ns-oevm.

Emits the same JSON contract st-berry produces (st_serializer.serialize_source_map),
so the jast-gw debug viewer consumes ns-oevm and st-berry maps identically:

    { "source": "<basename.ns>",
      "functions": { "<Vtable>>selector>": [[offset, line], ...] } }
"""
import json


def build_stmap(source_filename, source_maps):
    """Serialize collected per-method line maps to a .stmap JSON string.

    source_maps: dict {qualified_name: [(bytecode_offset, line), ...]}.
    Returns a JSON string; tuples become two-element [offset, line] arrays.
    """
    functions = {
        name: [[offset, line] for (offset, line) in pairs]
        for name, pairs in source_maps.items()
    }
    return json.dumps(
        {"source": source_filename, "functions": functions},
        indent=2,
    )
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_stmap.py -v`
Expected: PASS (3 passed).

- [ ] **Step 5: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add tools/stmap.py tools/test_stmap.py
git commit -m "feat(stmap): pure .stmap serializer matching the st-berry contract"
```

---

## Task 2: Record spans and collect per-method maps in the compiler

This task makes the compiler accumulate `(offset, line)` pairs in memory. It writes no file and emits no instruction, so it is a pure no-op for generated images.

**Files:**
- Modify: `tools/ns_compile_oevm.py`
  - `MethodCompiler.__init__` (around line 81, after `self.instructions = []`)
  - `MethodCompiler.compile_statement` (line 394)
  - new `MethodCompiler.record_line` (add next to `compile_statement`)
  - `OevmCompiler.__init__` (line 922)
  - new `OevmCompiler.register_source_map`
  - `OevmCompiler.compile` block loop (around line 1255) — register block maps
  - `OevmCompiler.compile_class` instance-method loop (after `finalize_jumps()` at line 1329) and class-method loop (after line 1356) — register method maps
- Test: `tools/test_source_map.py`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `MethodCompiler.source_map: list[tuple[int, int]]` — `(offset, line)` pairs.
  - `MethodCompiler.record_line(node)` — appends `(len(self.instructions), node["span"]["startLine"])` when `node` has a span; no-op otherwise.
  - `OevmCompiler.source_maps: dict[str, list[tuple[int, int]]]` — qualified name → pairs.
  - `OevmCompiler.register_source_map(name, source_map)` — stores `source_map` under `name` only when non-empty.

- [ ] **Step 1: Write the failing test**

Create `tools/test_source_map.py`:

```python
import json
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
GRAMMAR = TOOLS.parent / "grammar" / "ns-parse.js"
sys.path.insert(0, str(TOOLS))


def _parse(src_text, tmp_path):
    f = tmp_path / "frag.ns"
    f.write_text(src_text)
    r = subprocess.run(["node", str(GRAMMAR), str(f)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


# A two-method class (packageUsing: manifest form — the proven fixture idiom).
# `^ 42` sits on line 2, `^ self answer` on line 3 (1-based source lines).
FIXTURE = """class Probe packageUsing: manifest = () (
public answer = ( ^ 42 )
public ask = ( ^ self answer )
) : ( )
"""


def test_compiler_collects_per_method_source_maps(tmp_path):
    import ns_compile_oevm as C
    ast = _parse(FIXTURE, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()

    sm = compiler.source_maps
    assert sm, "expected a non-empty source_maps dict"
    # Every entry is a list of (offset, line) with sane values.
    for name, pairs in sm.items():
        assert ">>" in name
        assert pairs, f"{name} should have at least one pair"
        for offset, line in pairs:
            assert isinstance(offset, int) and offset >= 0
            assert isinstance(line, int) and line >= 1
        offsets = [o for o, _ in pairs]
        assert offsets == sorted(offsets), f"{name} offsets must be non-decreasing"

    # The two source statements land on lines 2 and 3 (0-based file -> 1-based
    # lines: `^ 42` is line 2, `^ self answer` is line 3).
    all_lines = {line for pairs in sm.values() for _, line in pairs}
    assert {2, 3} <= all_lines
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_source_map.py::test_compiler_collects_per_method_source_maps -v`
Expected: FAIL — `AttributeError: 'OevmCompiler' object has no attribute 'source_maps'`.

- [ ] **Step 3: Add `source_map` to `MethodCompiler.__init__`**

In `tools/ns_compile_oevm.py`, immediately after `self.instructions = []` (line 81):

```python
        self.instructions = []
        # Source map: (bytecode_offset, source_line) recorded at statement
        # boundaries, mirroring st-berry/transpiler/st_funcctx.py. Pure
        # bookkeeping — never affects emitted bytecode.
        self.source_map = []
```

- [ ] **Step 4: Add `record_line` and hook `compile_statement`**

Add a `record_line` method directly above `compile_statement` (line 394), then call it as the first line of `compile_statement`:

```python
    def record_line(self, node):
        # Record the source line for the next instruction at the current offset.
        # Guarded: synthetic nodes without a span (e.g. generated accessors) are
        # simply not mapped.
        span = node.get("span") if isinstance(node, dict) else None
        if span and "startLine" in span:
            self.source_map.append((len(self.instructions), span["startLine"]))

    def compile_statement(self, stmt):
        self.record_line(stmt)
        if stmt["type"] == "return":
```

(Leave the rest of `compile_statement` unchanged.)

- [ ] **Step 5: Add `source_maps` + `register_source_map` to `OevmCompiler`**

In `OevmCompiler.__init__` (line 922), after `self.solidify = SolidifyCompiler()`:

```python
        self.solidify = SolidifyCompiler()
        # Per-method source maps, keyed "Vtable>>selector". Collected during
        # compile(); serialized to a .stmap sidecar by main().
        self.source_maps = {}
```

Add this method to `OevmCompiler` (e.g. directly below `__init__`):

```python
    def register_source_map(self, name, source_map):
        # Store only non-empty maps; synthetic methods (accessors, default init)
        # have no source statements and are intentionally omitted.
        if source_map:
            self.source_maps[name] = source_map
```

- [ ] **Step 6: Register maps at the three real-source sites**

In `OevmCompiler.compile`, inside the `for blk in self.block_methods:` loop (around line 1255), after the existing `self.solidify.add_method(...)` for the block:

```python
            self.register_source_map(f"{blk.name}>>{blk.selector}", blk.source_map)
```

In `OevmCompiler.compile_class`, in the instance-method loop, after `self.solidify.add_method(...)` at line 1342 (the `vtable_name=vt, selector=selector` add):

```python
            self.register_source_map(f"{vt}>>{selector}", compiler.source_map)
```

In the class-side-method loop, after its `self.solidify.add_method(...)` (following line 1358):

```python
            self.register_source_map(f"{meta_vt}>>{selector}", compiler.source_map)
```

- [ ] **Step 7: Run test to verify it passes**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_source_map.py::test_compiler_collects_per_method_source_maps -v`
Expected: PASS.

- [ ] **Step 8: Prove the image is byte-identical (no-op)**

```bash
cd /home/david/workspace/ns-oevm
rm -rf /tmp/sm_before /tmp/sm_after
git stash                       # park the Task 2 edits
python3 tools/snapshot_images.py /tmp/sm_before
git stash pop                   # restore the edits
python3 tools/snapshot_images.py /tmp/sm_after
diff -r /tmp/sm_before /tmp/sm_after && echo "NO-OP CONFIRMED"
```
Expected: no diff, then `NO-OP CONFIRMED`. (Task 2 writes no `.stmap` yet, so the directories match exactly.)

- [ ] **Step 9: Conformance unchanged**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4`
Expected: `Passed: 47, Failed: 0, Skipped: 1`.

- [ ] **Step 10: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add tools/ns_compile_oevm.py tools/test_source_map.py
git commit -m "feat(stmap): collect per-method (offset, line) maps during compile (no-op for images)"
```

---

## Task 3: Write the `.stmap` sidecar from `main()`

**Files:**
- Modify: `tools/ns_compile_oevm.py` — `main()`, right after `compiler.compile()` (line 1686), before the `if fmt == "oimg":` branch (so both backends get a map). Add `from stmap import build_stmap` near the top-level imports.
- Test: `tools/test_source_map.py` (add an end-to-end test)

**Interfaces:**
- Consumes: `build_stmap` (Task 1); `compiler.source_maps` (Task 2).
- Produces: a sidecar file at `os.path.splitext(out_c)[0] + ".stmap"` containing the `.stmap` JSON, written for both `--format=c` and `--format=oimg`.

- [ ] **Step 1: Write the failing test**

Append to `tools/test_source_map.py`:

```python
NS_TESTS = TOOLS.parent.parent / "ns-tests"


def test_stmap_sidecar_written_path_a(tmp_path):
    src = NS_TESTS / "04_somns_basic" / "01_method_call.ns"
    out = tmp_path / "img.c"
    r = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"), str(src), str(out)],
        capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout

    stmap = tmp_path / "img.stmap"
    assert stmap.exists(), "expected a .stmap sidecar next to the image"
    doc = json.loads(stmap.read_text())
    assert doc["source"] == "01_method_call.ns"
    assert isinstance(doc["functions"], dict) and doc["functions"]
    # Every function maps to a list of [offset, line] pairs.
    for name, pairs in doc["functions"].items():
        assert ">>" in name
        for pair in pairs:
            assert len(pair) == 2
            assert isinstance(pair[0], int) and isinstance(pair[1], int)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_source_map.py::test_stmap_sidecar_written_path_a -v`
Expected: FAIL — `AssertionError: expected a .stmap sidecar next to the image`.

- [ ] **Step 3: Add the `build_stmap` import**

Near the top of `tools/ns_compile_oevm.py`, with the other imports (after `import subprocess`):

```python
from stmap import build_stmap
```

(`tools/` is the module's own directory and is on `sys.path` when the compiler runs as a script, matching how `oimg_serialize` / `builtins_registry` are imported elsewhere in this file.)

- [ ] **Step 4: Write the sidecar in `main()`**

In `main()`, immediately after `compiler.compile()` (line 1686) and before the `if fmt == "oimg":` branch:

```python
    compiler.compile()

    # Emit the source-map sidecar (.stmap) next to the image, for both the
    # Path A (.c) and Path B (.oimg) backends. Format matches st-berry so the
    # jast-gw debug viewer consumes ns-oevm and st-berry maps identically.
    stmap_path = os.path.splitext(out_c)[0] + ".stmap"
    with open(stmap_path, "w") as f:
        f.write(build_stmap(os.path.basename(src_ns), compiler.source_maps))
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/test_source_map.py -v`
Expected: PASS (both tests).

- [ ] **Step 6: Verify the `.oimg` path also emits a sidecar**

```bash
cd /home/david/workspace/ns-oevm
gcc -std=c99 -I../oevm/src ../oevm/tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json
python3 tools/ns_compile_oevm.py ../ns-tests/04_somns_basic/01_method_call.ns /tmp/app.oimg \
    --format=oimg --layout=/tmp/layout.json
test -f /tmp/app.stmap && python3 -c "import json; print(sorted(json.load(open('/tmp/app.stmap'))['functions']))"
```
Expected: prints the list of qualified method names — confirms the sidecar is written on the Path B early-return branch too.

- [ ] **Step 7: Prove images are still byte-identical**

```bash
cd /home/david/workspace/ns-oevm
rm -rf /tmp/sm_before /tmp/sm_after
git stash
python3 tools/snapshot_images.py /tmp/sm_before
git stash pop
python3 tools/snapshot_images.py /tmp/sm_after
diff -r /tmp/sm_before /tmp/sm_after --exclude='*.stmap' && echo "IMAGES UNCHANGED"
```
Expected: no diff among `.c` files, then `IMAGES UNCHANGED`. (The `.stmap` sidecars now exist only in `after/`; excluding them shows the images themselves are untouched.)

- [ ] **Step 8: Conformance unchanged**

Run: `cd /home/david/workspace/ns-oevm && python3 tools/run_tests_oevm.py 2>&1 | tail -4`
Expected: `Passed: 47, Failed: 0, Skipped: 1`.

- [ ] **Step 9: Full python test suite green**

Run: `cd /home/david/workspace/ns-oevm && python3 -m pytest tools/ -q`
Expected: all pass (new `test_stmap.py` + `test_source_map.py` plus the existing `test_*` suite).

- [ ] **Step 10: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add tools/ns_compile_oevm.py tools/test_source_map.py
git commit -m "feat(stmap): write .stmap sidecar next to the image for both backends"
```

---

## Task 4: Documentation

**Files:**
- Modify: `docs/compiler.md` (add a short "Source maps (.stmap)" section)
- Modify: `docs/superpowers/specs/2026-06-19-next-phase-stdlib-and-debug.md` (mark the source-map step done)

**Interfaces:** none.

- [ ] **Step 1: Document the `.stmap` output in `docs/compiler.md`**

Add a section describing: the sidecar path (`<image-base>.stmap`), the JSON contract (`{source, functions: {"Vtable>>selector": [[offset, line]]}}`), that it matches the st-berry format consumed by `jast-gw`, statement-granularity, and v1 scope (instance/class-side methods + non-inlined blocks; synthetic methods omitted). Reference `tools/stmap.py`.

- [ ] **Step 2: Update the roadmap doc**

In `docs/superpowers/specs/2026-06-19-next-phase-stdlib-and-debug.md`, under Item 2's concrete work list, mark step 1 (thread spans) and step 2 (emit `.stmap`) as done, leaving step 3 (wire oevm's `debugger_hook` to the MCP surface) as the next piece.

- [ ] **Step 3: Commit**

```bash
cd /home/david/workspace/ns-oevm
git add docs/compiler.md docs/superpowers/specs/2026-06-19-next-phase-stdlib-and-debug.md
git commit -m "docs(stmap): document source-map emission; mark roadmap step done"
```

---

## Self-Review

**1. Spec coverage** (against the roadmap doc's Item 2 source-map work):
- "Thread the existing source spans through `ns_compile_oevm.py`" → Task 2 (`record_line` + `compile_statement` hook). ✅
- "Emit a `.stmap` (`.ns` line ↔ bytecode offset) in the format jast-gw consumes" → Task 1 (serializer) + Task 3 (sidecar write), format pinned to `st_serializer.serialize_source_map`. ✅
- "Wire oevm's debugger_hook + inspect_stack to the MCP surface" → **explicitly out of scope here** (next plan); noted in Task 4 Step 2. This plan delivers the compiler prerequisite only, which is the stated highest-leverage piece.

**2. Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". Every code step shows full code; every run step gives an exact command and expected output. ✅

**3. Type consistency:** `source_map` is `list[(int, int)]` everywhere; `source_maps` is `dict[str, list[(int,int)]]`; `build_stmap(source_filename, source_maps)` consumes exactly that and emits `[offset, line]` arrays. Qualified-name form `f"{vtable}>>{selector}"` is identical across the three registration sites and asserted (`">>" in name`) in both tests. `record_line(node)` / `register_source_map(name, source_map)` signatures match their call sites. ✅

**Scope note:** Single subsystem (the compiler's map output). One coherent plan; no sub-project split needed.
