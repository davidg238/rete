# The debugger's stack and names — Implementation Plan (rete#323)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** At a stop, `custos debug` shows the call stack as `file:line Class>>sel` per frame and the named args, temps and captured locals of any frame; one method identity (the blob offset) end to end.

**Architecture:** The compiler writes each method's variable layout (`vars`) into `.oimg.map.json`. The VM gains `blobOffsetOf:` and `lexicalSlotsOf:` and its break primitives take a blob offset; the Supervisor drops its registry and answers `dbg:backtrace` and a widened `dbg:inspect`. custos keys everything on the blob offset and adds `bt` and `locals`.

**Tech Stack:** C (oevm VM), nsl (`supervisor.nsl`), Python (the compiler, test harnesses), Go (custos).

**Spec:** `docs/design/2026-10-05-debugger-stack-and-names-design.md` (read it first; §1 carries a correction made while planning).

> Code in this plan has **never run** (memory: plan-embedded code is unrun code — about one
> defect per task on #165/#190). Treat each snippet as a starting point checked against the
> file it lands in; read the surrounding code before pasting.

## Global Constraints

- No backward compatibility: no dual-read, no versioned field, no accepting both a method oop and an offset (root `CLAUDE.md`, design posture).
- `protocol/IMAGE-ARTIFACTS.md` §3: a missing or extra field refuses the map — `vars` is **required** on every method (empty list when none), and the doc and both sides change in one commit.
- The pc unit everywhere is the **instruction index** (`offsetOf:`; the map's `lines` offsets) — never bytes. The method key is the **byte** offset from `oimg_blob_base`.
- Never run two nsl suites concurrently, never `porta_sync_host.sh` beside one; the suite runs ~20 min in the background to a file (`python3 -u … > log 2>&1`), never under `timeout`, never through `tail`.
- Read the suite's aux line: `debugger-integration`, `custos-debug` and `tutorial-debugger` must say PASS, not SKIP.
- Explicit paths on every `git add`; never `commit -a` (the tree has ~50 untracked bake outputs).
- Commits end `refs #323` and carry the attribution line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Board work is `c6spare` only; never flash `c6bench`.

## Review Focus

1. **A stop before the prologue runs** (pc 0, `OP_MK_CONTEXT` not yet executed): the Context register is nil, so `locals` must print captured names as `(not yet set)`, never crash or print a register. Test: Task 4 Step 1, `TestLocalsBeforePrologue`.
2. **A `to:do:` induction variable at the loop's increment pc** (outside the inlined body's range): `locals` must simply omit `i`, not show a stale register under a wrong name. Test: Task 4 Step 1, `TestLocalsOutsideInlinedRange`.
3. **Two inlined scopes reusing one register for different names** (sequential `ifTrue:` blocks each with a temp): only the name whose range covers the pc is shown. Test: Task 1 Step 1, `test_vars_reused_register_has_disjoint_ranges`.
4. **`bt` from a pause in the kernel** (the common case: a waiting program sits in `pumpEvents`): every frame is named, kernel ones as `kernel.nsl:<line>  Kernel>>…`, none as `-1`. Test: Task 4 Step 6 (`test_custos_debug.py` `bt` after `pause`).
5. **A map from another build**: `bt`/`locals` must refuse it exactly as `break` does today (the hash check), never resolve offsets against the wrong map. Covered by the existing `CheckImage` path — Task 3 Step 6 asserts `bt` goes through the same `SourceMap` the session checked.

---

### Task 1: `vars` in the image map (compiler + contract + reader)

**Files:**
- Modify: `nodus/nsl/tools/ns_compile_oevm.py` (`MethodCompiler.__init__` ~L118, `finalize_contexts` prologue shift ~L371, `compile_inlined_block` ~L385–415, the `.map.json` write ~L2367)
- Modify: `nodus/nsl/tools/image_map.py` (`build_image_map`)
- Modify: `protocol/IMAGE-ARTIFACTS.md` §3, §4
- Modify: `protocol/fixtures/image/Fixture.nsl`; regenerate `Fixture.oimg`, `Fixture.oimg.map.json`
- Modify: `custos/internal/imagemap/imagemap.go` (`docMethod`, `Method`, the missing-field check)
- Test: `nodus/nsl/tools/test_image_map.py`, `custos/internal/imagemap/imagemap_test.go`

**Interfaces:**
- Produces (Python): `MethodCompiler.debug_vars() -> list[dict]`, each `{"name": str, "reg": int}` | `{"name": str, "reg": int, "from": int, "to": int}` | `{"name": str, "ctx": int}`.
- Produces (Go): `imagemap.Var{Name string; Reg, Ctx, From, To int}` with `Reg == -1` for a ctx var, `Ctx == -1` for a reg var, `From == 0 && To == -1` meaning whole-method; `imagemap.Method.Vars []Var`.

- [ ] **Step 1: Write the failing writer tests** in `test_image_map.py` (reuse its compile fixture; add a second tiny source compiled the same way for the reuse case):

```python
def _method(doc, name):
    return next(m for m in doc["methods"].values() if m["name"] == name)

def test_vars_every_shape(compiled_map):
    doc = compiled_map  # the fixture's freshly compiled map
    main = _method(doc, "Fixture>>main:args:")
    names = {v["name"]: v for v in main["vars"]}
    assert names["platform2"] == {"name": "platform2", "reg": 1}
    assert names["args"] == {"name": "args", "reg": 2}
    assert set(names["k"]) == {"name", "ctx"}          # captured by the kept block
    i = names["i"]                                      # the inlined to:do: param
    assert set(i) == {"name", "reg", "from", "to"} and i["from"] < i["to"]
    assert "self" not in names

def test_vars_reused_register_has_disjoint_ranges(tmp_path):
    src = tmp_path / "Reuse.nsl"
    src.write_text(
        "class Reuse usingPlatform: platform = () (\n"
        "  public main: p args: a = (\n"
        "    true ifTrue: [ | x | x:: 1. x println ].\n"
        "    true ifTrue: [ | y | y:: 2. y println ].\n"
        "    ^ 0 )\n) : ()\n")
    doc = compile_map(src, tmp_path / "Reuse.oimg")   # same helper the fixture uses
    vs = {v["name"]: v for v in _method(doc, "Reuse>>main:args:")["vars"]}
    assert vs["x"]["reg"] == vs["y"]["reg"]
    assert vs["x"]["to"] <= vs["y"]["from"]

def test_every_method_has_vars(compiled_map):
    assert all(isinstance(m["vars"], list) for m in compiled_map["methods"].values())
```

If `test_image_map.py` has no `compiled_map`/`compile_map` helper by those names, factor the existing compile step (its L20–30) into them first, in this step.

- [ ] **Step 2: Run, expect FAIL** — `cd nodus/nsl && python3 -m pytest tools/test_image_map.py -q` → `KeyError: 'vars'` (and `k`/`i` missing until Step 3's fixture edit).

- [ ] **Step 3: Give the fixture every shape.** In `protocol/fixtures/image/Fixture.nsl`, change `main:args:` to:

```
  public main: platform2 args: args = (
    | c twice k |
    c:: Counter from: 40.
    k:: 1.
    twice:: [ :x | x + x + k ].
    1 to: 2 do: [ :i | c bump ].
    (twice value: c bump) println.
    ^ 0 )
```

- [ ] **Step 4: Record inlined ranges in the compiler.** In `MethodCompiler.__init__`, beside `self.source_map = []`, add `self.inlined_vars = []  # [name, reg, from, to] for inlined-block params/temps (rete#323)`. In `compile_inlined_block`, record each declared name with the instruction index at declaration and close the range when the scope is popped:

```python
        start = len(self.instructions)
        declared = []
        if param_regs is not None:
            for name, reg in zip(block.get("params", []), param_regs):
                self.current_scope.declare(name, reg)
                declared.append((name, reg))
        for t in block.get("temps", []):
            reg = self.alloc_reg()
            self.current_scope.declare(t, reg)
            declared.append((t, reg))
            if init_temps:
                self.emit_ABx(OP_LD_CONST, reg, self.add_constant(None))
        # ... body unchanged ...
        end = len(self.instructions)
        self.inlined_vars.extend([n, r, start, end] for (n, r) in declared)
        self.current_scope = saved_scope
        self.next_free_reg = saved_free
```

In `finalize_contexts`, where the prologue is prepended (`self.source_map = [(off + n, line) …]`), shift the ranges too:

```python
        self.inlined_vars = [[nm, r, a + n, b + n] for (nm, r, a, b) in self.inlined_vars]
```

Add the method:

```python
    def debug_vars(self):
        """The frame's named variables for the image map's `vars` (rete#323,
        protocol/IMAGE-ARTIFACTS.md §3). Captured args/temps are named by Context
        slot only: after the prologue their register is stale."""
        out = []
        for name, reg in self.local_vars.items():
            if name == "self":
                continue
            if name in self.capture_slot:
                out.append({"name": name, "ctx": self.capture_slot[name]})
            else:
                out.append({"name": name, "reg": reg})
        out += [{"name": n, "reg": r, "from": a, "to": b} for (n, r, a, b) in self.inlined_vars]
        return out
```

- [ ] **Step 5: Write `vars` into the map.** `build_image_map` gains a parameter `method_vars` (`{"<name>": [var, …]}`) and writes `"vars": method_vars.get(name, [])` after `"lines"`. At the call site (~L2371) pass `{k: c.debug_vars() for k, c in compiler.method_compilers.items()}`. Check that `method_compilers` keys use the same block naming as `source_maps` (both are registered together at L1822–1823, 1904–1905, 1932–1933, 2154–2155), so `method_vars.get(name)` hits for blocks.

- [ ] **Step 6: Run the writer tests, expect PASS** — `cd nodus/nsl && python3 -m pytest tools/test_image_map.py -q`.

- [ ] **Step 7: Contract.** In `protocol/IMAGE-ARTIFACTS.md` §3, add `vars` to the example method and a bullet after `lines`, worded from the spec §3 (reg alone = whole method; reg+from/to = inlined, `to` exclusive, instruction indices; ctx = captured arg/temp, no range; `self` not listed; a later-declared live entry shadows an earlier one). In §4, add "a captured local and an inlined loop variable" to the fixture's list of shapes.

- [ ] **Step 8: Regenerate the fixture** with the §4 command, from the repo root:

```
python3 nodus/nsl/tools/ns_compile_oevm.py protocol/fixtures/image/Fixture.nsl \
    protocol/fixtures/image/Fixture.oimg --format=oimg \
    --layout=nodus/oevm/zephyr/layout-esp32c6.json
```

Then `rm -f protocol/fixtures/image/*.stmap protocol/fixtures/image/*.methods.json` if the compiler left any (only the three files are tracked).

- [ ] **Step 9: Reader test, failing first.** In `imagemap_test.go` add:

```go
func TestVarsFromFixture(t *testing.T) {
	m, err := Load("../../../protocol/fixtures/image/Fixture.oimg.map.json")
	if err != nil {
		t.Fatal(err)
	}
	var main *Method
	for _, x := range m.Methods {
		if x.Name == "Fixture>>main:args:" {
			x := x
			main = &x
		}
	}
	if main == nil {
		t.Fatal("no Fixture>>main:args:")
	}
	got := map[string]Var{}
	for _, v := range main.Vars {
		got[v.Name] = v
	}
	if v := got["args"]; v.Reg != 2 || v.Ctx != -1 || v.To != -1 {
		t.Errorf("args = %+v, want reg 2 whole-method", v)
	}
	if v := got["k"]; v.Reg != -1 || v.Ctx < 0 {
		t.Errorf("k = %+v, want a ctx slot", v)
	}
	if v := got["i"]; v.Reg < 0 || v.From >= v.To {
		t.Errorf("i = %+v, want a reg with a range", v)
	}
}
```

and extend the existing missing-field test table with a map whose method lacks `vars` (expect refusal). Run `cd custos && go test -count=1 ./internal/imagemap/` → FAIL (unknown field `vars`).

- [ ] **Step 10: Reader.** In `imagemap.go`:

```go
type docVar struct {
	Name *string `json:"name"`
	Reg  *int    `json:"reg"`
	Ctx  *int    `json:"ctx"`
	From *int    `json:"from"`
	To   *int    `json:"to"`
}
```

`docMethod` gains `Vars []docVar \`json:"vars"\``; a nil `Vars` (field absent — check with a `*[]docVar` or a raw presence check, matching how `lines` is checked today) is a missing field. Convert each to `Var`: exactly one of `reg`/`ctx` present, `from`/`to` both or neither, `ctx` never with a range — anything else refuses the map naming the method. Defaults: `Ctx=-1` for reg vars, `Reg=-1` for ctx vars, `From=0, To=-1` when no range.

- [ ] **Step 11: Run every Go gate** — `for m in protocol porta custos; do (cd $m && go test -count=1 ./...); done` → all `ok`. `custos/internal/debug/sourcemap_test.go` checks hand-written facts about the fixture's lines; if an offset moved because `main:args:` grew, update the fact from the regenerated map and say so in the commit body.

- [ ] **Step 12: Commit**

```bash
git add nodus/nsl/tools/ns_compile_oevm.py nodus/nsl/tools/image_map.py nodus/nsl/tools/test_image_map.py \
  protocol/IMAGE-ARTIFACTS.md protocol/fixtures/image/Fixture.nsl protocol/fixtures/image/Fixture.oimg \
  protocol/fixtures/image/Fixture.oimg.map.json custos/internal/imagemap/imagemap.go \
  custos/internal/imagemap/imagemap_test.go custos/internal/debug/sourcemap_test.go
git commit -m "image map: vars per method (registers, inlined ranges, context slots), refs #323"
```

---

### Task 2: VM primitives — `blobOffsetOf:`, `lexicalSlotsOf:`, break by offset

**Files:**
- Modify: `nodus/oevm/src/vm.c` (`primitive_break_add`/`_remove` ~L2038–2060, new prims beside `primitive_method_of` ~L2067)
- Modify: `nodus/oevm/src/oevm.h` (~L445: declarations)
- Modify: `nodus/oevm/src/mirror.c` (~L56–75: dispatch)
- Test: `nodus/oevm/tests/test_debugger.c` (TEST 12 ~L700–720; a new TEST for the two read prims)

This task changes the break primitives' argument, so `supervisor.nsl` is out of step until Task 3. **Do not commit Task 2 alone**: Tasks 2 and 3 land as one commit (the wire grammar changes on both ends together). Run the C unit test here; the suite runs at the end of Task 3.

**Interfaces:**
- Produces (mirror selectors): `blobOffsetOf: ctx` → SMI | nil; `lexicalSlotsOf: aMethodContext` → Array of the frame's Context slots (register `num_arguments + 1`) | nil; `breakAt: <SMI blobOff> offset: <SMI pc>` → the SMI blobOff, or nil (no blob / out of range / misaligned); `unbreakAt: <SMI blobOff> offset: <SMI pc>` → same.

- [ ] **Step 1: Failing C test.** In TEST 12, before the `breakAt:` send, place a fake blob around `cm_bp` and send offsets:

```c
        /* rete#323: the break prims take a BLOB OFFSET. Frame cm_bp inside a fake
           blob so its offset is well defined; restore the globals after. */
        const uint8_t *saved_base = oimg_blob_base, *saved_end = oimg_blob_end;
        oimg_blob_base = (const uint8_t *)cm_bp - 64;
        oimg_blob_end  = (const uint8_t *)cm_bp + 256;
        oop ba[] = { TAG_SMI(64), TAG_SMI(K) };
        oop key = send(mirror_object(), create_symbol("breakAt:offset:"), 2, ba);
        assert(key == TAG_SMI(64));
```

and the `unbreakAt:` send with `{ TAG_SMI(64), TAG_SMI(K) }`; after it, assert the refusals and restore:

```c
        oop bad[] = { TAG_SMI(4096), TAG_SMI(K) };          /* outside the blob */
        assert(send(mirror_object(), create_symbol("breakAt:offset:"), 2, bad) == NULL);
        oop odd[] = { TAG_SMI(65), TAG_SMI(K) };            /* not word-aligned */
        assert(send(mirror_object(), create_symbol("breakAt:offset:"), 2, odd) == NULL);
        oimg_blob_base = NULL; oimg_blob_end = NULL;        /* a Path A image */
        assert(send(mirror_object(), create_symbol("breakAt:offset:"), 2, ba) == NULL);
        oimg_blob_base = saved_base; oimg_blob_end = saved_end;
```

Add a TEST for the read prims (number it after the last existing test): inside `bp_match_hook_c` (which receives the live MethodContext), send `lexicalSlotsOf:` with that context and record the answer: TEST 12's method has no captures, so assert it is NULL. For the positive case compile nothing new — it is covered on the host by Task 4 Step 5 (`dbg:inspect 1` on `scaled:` carries `c0=`/`c1=`), which runs a real frame with a real Context; say so in a comment in the C test. Also assert `lexicalSlotsOf:` answers NULL for `TAG_SMI(3)`. For `blobOffsetOf:`, reuse the TEST 12 hook: inside `bp_match_hook_c` (the hook receives the context), record `send(mirror_object(), create_symbol("blobOffsetOf:"), 1, &ctx)` into a static and assert it equals `TAG_SMI(64)` after the run.

Include `oimg.h` if the test does not already see `oimg_blob_base`/`oimg_blob_end`.

- [ ] **Step 2: Run, expect FAIL** — build and run the C units the way the suite does (`run_tests_oevm.py` runs them as `c-units`; find the exact make target with `rg -n test_debugger nodus/oevm/Makefile nodus/nsl/tools/run_tests_oevm.py`). Expected: `VM Error: Mirror doesNotUnderstand #lexicalSlotsOf:` or the `key == TAG_SMI(64)` assert.

- [ ] **Step 3: Implement** in `vm.c`, replacing `bp_method_key` and the two break prims:

```c
/* rete#323: the debugger names a method by its BLOB OFFSET -- the image map's key
   and the profiler's (`mprof:`). The bp table still keys on the CompiledMethod
   pointer, oimg_blob_base + off. A CompiledMethod carries no vtable, so an in-blob
   offset cannot be checked to BE a method; one that is not arms an entry that never
   fires, and custos only sends offsets read from the map. No blob (Path A), out of
   range, or misaligned: nil, and nothing is armed. */
static oop bp_key_at(oop off) {
    if (!IS_SMI(off) || oimg_blob_base == NULL) return NULL;
    intptr_t n = UNTAG_SMI(off);
    if (n < 0 || n % (intptr_t)sizeof(oop) != 0 ||
        oimg_blob_base + n >= oimg_blob_end) return NULL;
    return (oop)(oimg_blob_base + n);
}

oop primitive_break_add(oop receiver, int argc, oop *args) {
    (void)receiver;
    if (argc < 2 || !IS_SMI(args[1])) {
        fprintf(stderr, "VM Error: breakAt:offset: requires (SMI blob offset, SMI pc)\n");
        abort();
    }
    oop key = bp_key_at(args[0]);
    if (key == NULL) return NULL;
    bp_add(key, (long)UNTAG_SMI(args[1]));
    return args[0];
}

oop primitive_break_remove(oop receiver, int argc, oop *args) {
    (void)receiver;
    if (argc < 2 || !IS_SMI(args[1])) {
        fprintf(stderr, "VM Error: unbreakAt:offset: requires (SMI blob offset, SMI pc)\n");
        abort();
    }
    oop key = bp_key_at(args[0]);
    if (key == NULL) return NULL;
    bp_remove(key, (long)UNTAG_SMI(args[1]));
    return args[0];
}

/* The blob offset of a running context's CompiledMethod; nil with no blob. */
oop primitive_blob_offset_of(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver) || oimg_blob_base == NULL) return NULL;
    struct MethodContext *ctx = (struct MethodContext *)receiver;
    const uint8_t *cm = (const uint8_t *)closure_entry((struct Closure *)ctx->closure);
    if (cm < oimg_blob_base || cm >= oimg_blob_end) return NULL;
    return TAG_SMI((intptr_t)(cm - oimg_blob_base));
}

/* The captured locals of a frame: the slots of the heap Context in its register
   num_arguments + 1 (the compiler's ctx_reg; OP_LD_CTX reads the same register) as a
   fresh Array. nil when that register holds no Context -- a frame with no captures,
   or one whose OP_MK_CONTEXT prologue has not run (frame regs are nil-seeded). */
oop primitive_lexical_slots_of(oop receiver, int argc, oop *args) {
    (void)argc; (void)args;
    if (receiver == NULL || IS_SMI(receiver)) return NULL;
    struct MethodContext *mc = (struct MethodContext *)receiver;
    struct CompiledMethod *cm = (struct CompiledMethod *)closure_entry((struct Closure *)mc->closure);
    int r = cm->num_arguments + 1;
    if (r >= cm->num_registers) return NULL;
    oop c = mc->regs[r];
    if (c == NULL || IS_SMI(c) || obj_vtable(c) != ContextVtable) return NULL;
    uint32_t n = ((struct Context *)c)->num_slots;
    oop arr = indexable_new((intptr_t)n);
    c = mc->regs[r];   /* re-read after the allocation */
    struct ObjectInstance *inst = (struct ObjectInstance *)arr;
    for (uint32_t i = 0; i < n; i++)
        inst->slots[i + 1] = ((struct Context *)c)->slots[i];   /* same 1-based fill as primitive_calls_stats */
    return arr;
}
```

Check before trusting: (a) `primitive_calls_stats` fills `slots[1..n]` for `indexable_new(n)` — confirm that is the indexable layout (slot 0 is the size) by reading `indexable_new`; (b) the Context stays live across `indexable_new` (the GC is non-moving mark-sweep and the Context is reachable from the frame register; keep the re-read, do not cache `slots`); (c0) `mc->regs` is how `primitive_method_context_reg_at` (`vm.c` ~L1723) reads a register — copy its access exactly, including whatever it does for a frame whose regs live on the stack; (c) `vm.c` sees `oimg_blob_base`/`oimg_blob_end` (`#include "oimg.h"`) and `struct Context` (`frame_abi.h`). Delete `bp_method_key` and its comment; `primitive_method_of` stays only if Task 3 still calls `methodOf:` — it will not, so delete it and its `mirror.c` line too, and its `oevm.h` declaration (dead code goes in the same commit) — after `rg -n "methodOf:" nodus --glob "!*solidified_image.c"` shows no other caller (`test_debugger.c` may have one: port it to `blobOffsetOf:`).

Declare both new prims in `oevm.h` beside `primitive_method_of`'s old line, and dispatch them in `mirror.c`'s read-side block:

```c
    if (strcmp(sel, "blobOffsetOf:") == 0)  return primitive_blob_offset_of(args[0], argc - 1, args + 1);
    if (strcmp(sel, "lexicalSlotsOf:") == 0) return primitive_lexical_slots_of(args[0], argc - 1, args + 1);
```

Update the `mirror.c` comment above the break prims to say they take `(SMI blob offset, SMI pc)`. Note the mirror's `argc < 1` guard aborts on a nil reflectee only when argc is 0, so `lexicalSlotsOf: nil` reaches the prim with `args[0] == NULL` — which the prim answers nil.

- [ ] **Step 4: Run the C test, expect PASS.** Same command as Step 2. Do not commit; go to Task 3.

---

### Task 3: The identity switch — Supervisor, custos session, harnesses (one commit with Task 2)

**Files:**
- Modify: `nodus/nsl/lib/supervisor.nsl` (Debugger ~L66–420)
- Modify: `custos/internal/debug/sourcemap.go`, `session.go`, `debug.go` (help text), `session_test.go`, `sourcemap_test.go`
- Modify: `nodus/nsl/tools/test_debugger_integration.py` (delete Path A, port Path B), `nodus/nsl/tools/test_custos_debug.py`
- Modify: `nodus/nsl/tools/run_tests_oevm.py` (only if it names Path A for the debugger gate)
- Delete: `nodus/oevm/zephyr/tools/dbg_session.py`

**Interfaces:**
- Consumes: Task 2's selectors; Task 1's `imagemap.Method.Vars` (not used until Task 4).
- Produces (node lines): `dbg:paused <why> <blobOff> <pc>` (blobOff `-1` with no blob); `dbg:break <blobOff> <pc>` / `dbg:clear <blobOff> <pc>` → `dbg:ok break|clear`, `dbg:error no-blob`, `dbg:error no-bp`, `dbg:error usage`; `dbg:methods` → `dbg:error unknown`.
- Produces (Go): `Breakpoint{Off, PC, Line int; Name string}`; `(*SourceMap).Where(off, pc int) (where, text string, line int)`; `(*SourceMap).Method(off int) (imagemap.Method, bool)`; `(*SourceMap).Methods() []string` (map only).

- [ ] **Step 1: Supervisor — breakpoints by offset.** In `Debugger`'s instance variables replace `ids sels meths tags bpIds bpMethods bpOffs` with `bpBlobs bpOffs` (keep `entry` — `objectVtable` uses it — and `stepMode stepDepth curCtx ready`). Delete `buildRegistry`, `listMethods`, `methodAt:`, `methodIdOfCtx:` and their comments. Then:

```
    (* rete#323: a method is its blob offset (the image map's key). The C table
       keys on oimg_blob_base + off; this record is what onBreakContext: matches. *)
    ensureBp = (
      bpBlobs == nil ifTrue: [
        bpBlobs:: io OrderedCollection new.
        bpOffs:: io OrderedCollection new ].
      ^ self )

    public break: blob at: off = (
      self ensureBp.
      (m breakAt: blob offset: off) == nil ifTrue: [ 'dbg:error no-blob' println. ^ self ].
      bpBlobs add: blob.
      bpOffs add: off.
      'dbg:ok break' println.
      ^ self )

    public clear: blob at: off = (
      | i hit n |
      self ensureBp.
      hit:: 0 - 1.
      i:: 1.
      [ hit < 0 and: [ i <= bpBlobs size ] ] whileTrue: [
        ((bpBlobs at: i) = blob and: [ (bpOffs at: i) = off ]) ifTrue: [ hit:: i ].
        i:: i + 1 ].
      hit < 0 ifTrue: [ 'dbg:error no-bp' println. ^ self ].
      m unbreakAt: blob offset: off.
      n:: bpBlobs size.
      hit to: n - 1 do: [ :j |
        bpBlobs at: j put: (bpBlobs at: j + 1).
        bpOffs at: j put: (bpOffs at: j + 1) ].
      bpBlobs removeLast.
      bpOffs removeLast.
      'dbg:ok clear' println.
      ^ self )

    (* The blob offset of a frame's method, or -1 with no blob (a Path A image). *)
    blobOf: ctx = (
      | b |
      b:: m blobOffsetOf: ctx.
      ^ b == nil ifTrue: [ 0 - 1 ] ifFalse: [ b ] )
```

`doBreak:`/`doClear:` keep their parsing; rename `idTok` to `blobTok` and update the "Tokenize" comment to `<blob> <off>`.

- [ ] **Step 2: Supervisor — stops by offset.** In `onBreakContext:`, the breakpoint path matches on `(self blobOf: ctx)` against `bpBlobs` (replacing `meth:: m methodOf: ctx` and the `bpMethods ==` test), and both `dbg:paused` lines print `self blobOf: ctx` where they printed an id or `methodIdOfCtx:`:

```
        blob:: self blobOf: ctx.
        hit:: false.
        self ensureBp.
        i:: 1.
        [ hit not and: [ i <= bpBlobs size ] ] whileTrue: [
          ((bpBlobs at: i) = blob and: [ (bpOffs at: i) = off ]) ifTrue: [ hit:: true ].
          i:: i + 1 ].
```

then `hit ifFalse: [ …ready / 'dbg:paused pause ' blob off… ] ifTrue: [ 'dbg:paused break ' blob off ]`, and the step path prints `'dbg:paused step '` with `self blobOf: ctx`. Replace the method temporaries `meth id mid` with `blob hit`. Check `hit not` exists in the dialect (nsl grammar gotchas memory); if not, keep the `id < 0` integer-flag shape of the original with `id` holding 0/1.

- [ ] **Step 3: Supervisor — `dbg:backtrace` and the Context slots in `dbg:inspect`.** Add to `commandLoop` beside `dbg:inspect` (both are queries; keep looping):

```
        verb = 'dbg:backtrace' ifTrue: [ self doBacktrace ] ifFalse: [
```

(and one more closing `]` at the end of the chain), with:

```
    (* dbg:backtrace -> ONE line, innermost first: `dbg:frames <blob>@<pc> ...`.
       custos maps each pair through the image map (rete#323). *)
    doBacktrace = (
      | ws s |
      ws:: io WriteStream new.
      ws nextPutAll: 'dbg:frames'.
      s:: curCtx.
      [ s == nil ] whileFalse: [
        ws nextPutAll: ' '.
        ws print: (self blobOf: s).
        ws nextPutAll: '@'.
        ws print: (m offsetOf: s).
        s:: m senderOf: s ].
      ws contents println.
      ^ self )
```

In `doInspect:`, after the `r<i>=` loop and before `ws contents println`, append the frame's captured locals (the primitive reads register `num_arguments + 1` itself, so the Supervisor needs no arity):

```
      slots:: m lexicalSlotsOf: s.
      slots == nil ifFalse: [
        1 to: slots size do: [ :k |
          ws nextPutAll: ' c'.
          ws print: k - 1.
          ws nextPutAll: '='.
          ws nextPutAll: (self describe: (slots at: k)) ] ].
```

and add `slots` to `doInspect:`'s temporaries.

Update the comment above `doInspect:` to the new line shape, and the class comment of `Debugger` (it still describes the registry).

- [ ] **Step 4: custos — the map is the only lookup.** In `sourcemap.go`: delete `ids`, `sels`, `selector`, `regRE`, `LoadRegistry`, `HasRegistry`, and the `methods` map. Keep `functions` but give each its offset:

```go
type function struct {
	off   int    // blob offset: the VM's name for it
	name  string // "<Vtable>>selector"
	pairs []pair
}
```

`Breakpoint` becomes:

```go
// Breakpoint is where a source line lands in the VM's terms.
type Breakpoint struct {
	Off, PC int    // the method's blob offset and the bytecode offset to send
	Line    int    // the line it lands on: the one asked for, or the next with code
	Name    string // the method, "<Vtable>>selector"
}
```

`Resolve`: delete the "a block or class the registry does not list" refusal and the registry lookup; take `f := sm.function(key)` and walk its pairs as today, answering `Breakpoint{Off: f.off, PC: p.off, Line: p.line, Name: f.name}`. `Where(off, pc)`:

```go
// Where turns the (blob offset, pc) of a stop into a sentence and the source line
// ("" when the stop is not in the app's file). line is 0 then.
func (sm *SourceMap) Where(off, pc int) (where, text string, line int) {
	if off < 0 {
		return fmt.Sprintf("in an image with no blob (Path A), pc %d", pc), "", 0
	}
	m, ok := sm.img.Methods[off]
	if !ok {
		return fmt.Sprintf("at blob offset %d, which the map does not name (wrong build?)", off), "", 0
	}
	at := lineAt(m.Lines, pc)
	if at == 0 {
		return fmt.Sprintf("in %s's %s, pc %d", sm.img.Files[m.File], m.Name, pc), "", 0
	}
	if m.File != 0 {
		return fmt.Sprintf("at %s:%d  %s", sm.img.Files[m.File], at, m.Name), "", at
	}
	return fmt.Sprintf("at %s:%d  %s", sm.Source, at, m.Name), sm.LineText(at), at
}
```

with `lineAt` holding today's "pair with the largest offset not past pc, ties to the larger line; before the first, the first" loop, over `[][2]int`, answering 0 for no lines. Note `line` is answered only for the app's file (the session's step logic compares lines of the app; a kernel line must not read as "moved to line N of the app").

`Methods()` lists every `function` (app file only), `fmt.Sprintf("%6d  %-36s %s:%d-%d", f.off, f.name, sm.Source, lo, hi)`, in offset order. Add `func (sm *SourceMap) Method(off int) (imagemap.Method, bool)` for Task 4.

- [ ] **Step 5: custos — the session by offset.** In `session.go`: `stop` holds `boot bool; off, pc int`; `OnLine` parses `pausedRE` into `(off, pc)` and calls `Where(off, pc)`; delete `registry()` and every call; `here()` answers `(off, line, ok)`; `Do`'s break case sends `fmt.Sprintf("dbg:%s %d %d", verb, bp.Off, bp.PC)` and says `-- %s %s:%d  %s%s` with `bp.Name`; `methods` no longer calls `stopFirst()` (no VM round trip); `stepLine` compares `(off, line)`. The `case strings.HasPrefix(word, "dbg:")` wait pattern gains `frames`: `^dbg:(ok|error|stack|frames|paused)`. Update `Help` (drop nothing yet; Task 4 adds `bt`/`locals`).

- [ ] **Step 6: Go tests.** Port `session_test.go` and `sourcemap_test.go`: every `dbg:paused <id>` fixture line becomes `dbg:paused <blobOff>` with the offset read from `protocol/fixtures/image/Fixture.oimg.map.json`; delete the registry-loading cases; the "block is refused" case becomes "a line in `Fixture_block1>>value:` resolves to that block's offset" (check the block's real name in the regenerated map); add `TestWhereKernel` (an offset whose method has `File != 0` answers `in kernel.nsl's …` or `at kernel.nsl:N`, and `line == 0`), `TestWhereUnknownOffset` (an offset not in the map says "wrong build?"), and `TestWhereNoBlob` (`off == -1`). Run `cd custos && go test -count=1 ./...` → `ok`.

- [ ] **Step 7: The Python harnesses.** `test_debugger_integration.py`: delete `run_path_a` and every Path A helper (L56 build, L210–303 drivers) and the docstring's Path A paragraph; the Path B driver stops sending `dbg:methods` and instead reads `countTo:`'s blob offset and its first line's pc from the compiled `.oimg.map.json` (it already builds the `.oimg`; the map is beside it), and asserts `dbg:paused break <thatOffset> <pc>`; the step/over/out assertions compare blob offsets where they compared `id=cid` / `main_id=1`. `test_custos_debug.py`: replace the `break 26` refusal with

```python
        child.sendline("break 26")
        see(r"-- break %s:26  DebugPauseTarget_block2>>value:" % re.escape(src))
        see(r"dbg:ok break")
        child.sendline("clear 26")
        see(r"dbg:ok clear")
```

(check line 26 is still that block's line in `47_debug_pause_target.nsl`). Delete `nodus/oevm/zephyr/tools/dbg_session.py` (spec §5: the board check is `custos debug -port`). `rg -n 'dbg_session|dbg:methods|Path A' nodus/nsl/tools/run_tests_oevm.py nodus/nsl/tools/test_debugger_integration.py` → fix every live hit.

- [ ] **Step 8: Build the hooks image and run the debugger gates.** `cd nodus/nsl && python3 tools/test_debugger_integration.py` and `python3 tools/test_custos_debug.py` → both PASSED. Then the whole suite in the background:

```bash
cd nodus/nsl && free -g && python3 -u tools/run_tests_oevm.py > /tmp/claude-suite-323-t3.log 2>&1
```

Expected: `Passed: N / Failed: 0`, aux line with `debugger-integration: PASS  custos-debug: PASS`. `tutorial-debugger` **will be RED** here (its transcript says "outside the registry" and shows a block refusal); that is Task 5's, and the commit body must say so.

- [ ] **Step 9: Commit Tasks 2 + 3 together**

```bash
git add nodus/oevm/src/vm.c nodus/oevm/src/oevm.h nodus/oevm/src/mirror.c nodus/oevm/tests/test_debugger.c \
  nodus/nsl/lib/supervisor.nsl custos/internal/debug/sourcemap.go custos/internal/debug/session.go \
  custos/internal/debug/debug.go custos/internal/debug/session_test.go custos/internal/debug/sourcemap_test.go \
  nodus/nsl/tools/test_debugger_integration.py nodus/nsl/tools/test_custos_debug.py nodus/nsl/tools/run_tests_oevm.py
git rm nodus/oevm/zephyr/tools/dbg_session.py
git commit -m "debugger: one method identity, the blob offset; registry deleted; dbg:backtrace, refs #323"
```

If `src/solidified_image.c` (tracked) changed because a bake ran, **do not commit it**; `git checkout -- nodus/oevm/src/solidified_image.c` (memory: Path-A bakes clobber the tracked fixture).

---

### Task 4: custos `bt` and `locals`, and the debug target grows the shapes

**Files:**
- Modify: `custos/internal/debug/session.go` (`Do`, `Help`, `words`), new `custos/internal/debug/frames.go`
- Test: `custos/internal/debug/frames_test.go`, `nodus/nsl/tools/test_custos_debug.py`, `nodus/nsl/tools/test_debugger_integration.py`
- Modify: `nodus/tests/04_somns_basic/46_debug_target.nsl`

**Interfaces:**
- Consumes: `imagemap.Var`, `(*SourceMap).Method(off)`, `(*SourceMap).Where(off, pc)`.
- Produces: `ParseFrames(line string) ([][2]int, error)`; `ParseInspect(line string) (regs, ctx map[int]string, err error)`; `LiveVars(vars []imagemap.Var, pc int) []imagemap.Var`.

- [ ] **Step 1: Failing unit tests** in `frames_test.go`:

```go
func TestParseFrames(t *testing.T) {
	got, err := ParseFrames("dbg:frames 3832@9 4100@2 -1@0")
	if err != nil || len(got) != 3 || got[0] != [2]int{3832, 9} || got[2] != [2]int{-1, 0} {
		t.Fatalf("got %v, %v", got, err)
	}
}

func TestParseInspect(t *testing.T) {
	regs, ctx, err := ParseInspect("dbg:stack off=9 r0=an Object r1=5 r2=[a context] c0=7 c1=nil")
	if err != nil || regs[1] != "5" || regs[0] != "an Object" || ctx[0] != "7" || ctx[1] != "nil" {
		t.Fatalf("regs %v ctx %v err %v", regs, ctx, err)
	}
}

func TestLocalsOutsideInlinedRange(t *testing.T) {
	vs := []imagemap.Var{{Name: "n", Reg: 1, Ctx: -1, To: -1}, {Name: "i", Reg: 4, Ctx: -1, From: 5, To: 20}}
	if got := LiveVars(vs, 22); len(got) != 1 || got[0].Name != "n" {
		t.Fatalf("pc 22: %v", got)
	}
	if got := LiveVars(vs, 5); len(got) != 2 {
		t.Fatalf("pc 5: %v", got)
	}
}

func TestLiveVarsShadowing(t *testing.T) {
	vs := []imagemap.Var{{Name: "x", Reg: 3, Ctx: -1, To: -1}, {Name: "x", Reg: 6, Ctx: -1, From: 2, To: 9}}
	got := LiveVars(vs, 4)
	if len(got) != 1 || got[0].Reg != 6 {
		t.Fatalf("got %v, want the inner x", got)
	}
}

func TestLocalsBeforePrologue(t *testing.T) {
	// A captured var whose Context is not built yet: no c<k> field on the line.
	s, out := newTestSession(t) // the helper session_test.go already uses; reuse it
	s.showLocals(map[int]string{0: "an Object"}, map[int]string{}, []imagemap.Var{{Name: "k", Reg: -1, Ctx: 0, To: -1}})
	if !strings.Contains(out.String(), "k = (not yet set)") {
		t.Fatalf("got %q", out.String())
	}
}
```

If `session_test.go` has no reusable session constructor, write `newTestSession` there in this step (a `Session` over a `writerLine` to a buffer and the fixture's `SourceMap`).

Run `cd custos && go test -count=1 ./internal/debug/` → FAIL (undefined).

- [ ] **Step 2: Implement `frames.go`**

```go
// Copyright (c) 2026 Ekorau LLC

package debug

// frames.go — the stack and the names at a stop (rete#323): `bt` from
// `dbg:frames`, `locals` from `dbg:stack` joined to the image map's `vars`.

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"

	"github.com/davidg238/rete/custos/internal/imagemap"
)

// ParseFrames reads `dbg:frames <off>@<pc> ...`, innermost first.
func ParseFrames(line string) ([][2]int, error) {
	rest, ok := strings.CutPrefix(line, "dbg:frames")
	if !ok {
		return nil, fmt.Errorf("not a dbg:frames line: %q", line)
	}
	var out [][2]int
	for _, f := range strings.Fields(rest) {
		a, b, ok := strings.Cut(f, "@")
		off, err1 := strconv.Atoi(a)
		pc, err2 := strconv.Atoi(b)
		if !ok || err1 != nil || err2 != nil {
			return nil, fmt.Errorf("bad frame %q in %q", f, line)
		}
		out = append(out, [2]int{off, pc})
	}
	return out, nil
}

// field is one ` r<n>=` or ` c<n>=` of a dbg:stack line. A value runs to the next
// field: printStrings contain spaces (`an Object`).
var fieldRE = regexp.MustCompile(` ([rc])(\d+)=`)

// ParseInspect reads `dbg:stack off=<pc> r0=... c0=...` into registers and
// Context slots, by index.
func ParseInspect(line string) (regs, ctx map[int]string, err error) {
	if !strings.HasPrefix(line, "dbg:stack ") {
		return nil, nil, fmt.Errorf("not a dbg:stack line: %q", line)
	}
	regs, ctx = map[int]string{}, map[int]string{}
	locs := fieldRE.FindAllStringSubmatchIndex(line, -1)
	for i, l := range locs {
		end := len(line)
		if i+1 < len(locs) {
			end = locs[i+1][0]
		}
		n, _ := strconv.Atoi(line[l[4]:l[5]])
		v := line[l[1]:end]
		if line[l[2]:l[3]] == "r" {
			regs[n] = v
		} else {
			ctx[n] = v
		}
	}
	return regs, ctx, nil
}

// LiveVars is the vars live at pc: whole-method ones, and inlined ones whose range
// holds pc; of two live with one name, the later-declared wins (an inner scope).
func LiveVars(vars []imagemap.Var, pc int) []imagemap.Var {
	var out []imagemap.Var
	at := map[string]int{}
	for _, v := range vars {
		if v.To >= 0 && (pc < v.From || pc >= v.To) {
			continue
		}
		if i, ok := at[v.Name]; ok {
			out[i] = v
			continue
		}
		at[v.Name] = len(out)
		out = append(out, v)
	}
	return out
}

// showLocals prints `self` and then each live var as `name = value`.
func (s *Session) showLocals(regs, ctx map[int]string, vars []imagemap.Var) {
	s.say("  self = %s", regs[0])
	for _, v := range vars {
		val, ok := "", false
		if v.Ctx >= 0 {
			val, ok = ctx[v.Ctx]
		} else {
			val, ok = regs[v.Reg]
		}
		if !ok {
			val = "(not yet set)"
		}
		s.say("  %s = %s", v.Name, val)
	}
}
```

- [ ] **Step 3: Wire the verbs** in `session.go`'s `Do`, before the `words` lookup:

```go
	case word == "bt" || word == "backtrace":
		s.stopFirst()
		got := texts(s.ask("dbg:backtrace", `^dbg:(frames|error)`, 10*time.Second))
		if len(got) == 0 {
			s.say("-- no answer to dbg:backtrace")
			return true
		}
		frames, err := ParseFrames(got[len(got)-1])
		if err != nil {
			s.say("-- %s", err)
			return true
		}
		s.mu.Lock()
		s.frames = frames
		s.mu.Unlock()
		for i, f := range frames {
			where, _, _ := s.sm.Where(f[0], f[1])
			s.say("  #%d %s", i, where)
		}
		return true
	case word == "locals":
		s.stopFirst()
		n := 0
		if arg != "" {
			if n, err = strconv.Atoi(arg); err != nil || n < 0 {
				s.say("-- locals takes a frame number from bt, got %q", arg)
				return true
			}
		}
		// The frame's method and pc: frame 0 is the stop; deeper ones need a bt.
		off, pc, ok := s.frameAt(n)
		if !ok {
			s.say("-- no frame %d (run bt first)", n)
			return true
		}
		got := texts(s.ask(fmt.Sprintf("dbg:inspect %d", n), `^dbg:(stack|error)`, 10*time.Second))
		if len(got) == 0 {
			s.say("-- no answer to dbg:inspect")
			return true
		}
		regs, ctx, err := ParseInspect(got[len(got)-1])
		if err != nil {
			s.say("-- %s", err)
			return true
		}
		m, _ := s.sm.Method(off)
		where, _, _ := s.sm.Where(off, pc)
		s.say("-- frame %d %s", n, where)
		s.showLocals(regs, ctx, LiveVars(m.Vars, pc))
		return true
```

`Session` gains `frames [][2]int` (reset to nil in `setStopped`, since a new stop invalidates them) and:

```go
// frameAt is frame n's (blob offset, pc): frame 0 is the stop itself; a deeper one
// is from the last bt at this stop.
func (s *Session) frameAt(n int) (off, pc int, ok bool) {
	st := s.current()
	if st == nil || st.boot {
		return 0, 0, false
	}
	if n == 0 {
		return st.off, st.pc, true
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if n >= len(s.frames) {
		return 0, 0, false
	}
	return s.frames[n][0], s.frames[n][1], true
}
```

Declare `err` in `Do` where the new cases need it (`var err error` at the top of the `locals` case). `dbg:inspect` hides its raw line from the person in `locals`: set `s.quiet` around the ask, exactly as the deleted `registry()` did. Add `bt`, `locals [N]` to `Help`. Run `cd custos && go test -count=1 ./...` → `ok`.

- [ ] **Step 4: The debug target grows a captured local.** In `46_debug_target.nsl`, after `total println.` add `(self scaled: 5) println.`, add `25` to the `expect:` block after `15`, update the header comment (it names `dbg:methods`) and add:

```
  (* rete#323: a block the compiler KEEPS, capturing an argument (k) and a temp
     (acc), so a stop inside it has a backtrace through a block and named
     context-slot locals. *)
  scaled: k = (
    | acc add |
    acc:: 0.
    add:: [ :x | acc:: acc + (x * k) ].
    add value: 2.
    add value: 3.
    ^ acc )
```

Run it alone: `cd nodus/nsl && python3 tools/run-one.py ../tests/04_somns_basic/46_debug_target.nsl B` (pass `B`, memory) → output matches the expect block.

- [ ] **Step 5: Harness asserts for the new shapes.** In `test_debugger_integration.py` (Path B), after the existing flow, break on the line of `acc:: acc + (x * k)` (its block method's offset and pc from the map), continue, send `dbg:backtrace` and assert the answer has ≥ 3 frames with the first frame's offset equal to the block's and the second to `scaled:`'s; send `dbg:inspect 1` and assert it carries `c0=` and `c1=` fields (scaled:'s Context: `acc`, `k` — read the slot order from the map's `vars`, do not hard-code it). In `test_custos_debug.py` add, after the `pause` stop:

```python
        child.sendline("bt")
        see(r"#0 [^\r\n]*")
        assert "wrong build" not in child.after and "-1" not in child.after
        see(r"#1 [^\r\n]*")
```

and, at a breakpoint in a method with a named arg, `locals` showing `n = ` with a value (use `47_debug_pause_target.nsl`'s `countTo:` at line 37: `child.sendline("locals"); see(r"  n = \d+")`).

- [ ] **Step 6: Run the gates** — `cd custos && go test -count=1 ./...`; `cd nodus/nsl && python3 tools/test_debugger_integration.py && python3 tools/test_custos_debug.py` → both PASSED.

- [ ] **Step 7: Commit**

```bash
git add custos/internal/debug/frames.go custos/internal/debug/frames_test.go custos/internal/debug/session.go \
  custos/internal/debug/session_test.go nodus/tests/04_somns_basic/46_debug_target.nsl \
  nodus/nsl/tools/test_debugger_integration.py nodus/nsl/tools/test_custos_debug.py
git commit -m "custos debug: bt and locals, named from the image map, refs #323"
```

---

### Task 5: Documents, the full gates, and the board

**Files:**
- Modify: `docs/tutorials/04-debugger.md` (§3 table, §4 bullets, §6 transcript)
- Modify: `docs/development.md`, `nodus/nsl/docs/NEWSPEAK_DIALECT.md` (verb grammar — `rg -n 'dbg:methods|dbg:paused|dbg:break' <file>` for each hit)
- Modify: `nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md` — not rewritten; one addendum line at its top pointing to the new design for the registry's deletion (it is a dated record)
- Modify: the debugger's mainline reference doc (find it: `rg -ln 'dbg:inspect' nodus/oevm/docs nodus/nsl/docs --glob '!**/design/**' --glob '!**/plans/**'`) — the promotion target

- [ ] **Step 1: Tutorial 04.** §3: add `| bt | | the call stack, innermost first, one file:line per frame |` and `| locals [N] | | frame N's self, arguments, temporaries and captured locals by name (§4) |`; `methods` now reads "every method of the app's file, with its line range". §4: delete the "refused with its name" block clause (a line in a kept block is a breakpoint like any other); replace "inspect shows registers, not names" with a bullet on `locals` (names from the map; a captured local before its method's first bytecode shows `(not yet set)`; an inlined loop variable is shown only inside the loop body) and keep `inspect` as the raw registers; replace "outside the registry" with "a stop in the kernel is named by its kernel.nsl line". §6: replace the transcript in Step 4.

- [ ] **Step 2: Run the tutorial gate** — it is part of the suite; run the suite in the background as in Task 3 Step 8 and read the aux line: `debugger-integration: PASS`, `custos-debug: PASS`, `tutorial-debugger: PASS`, `Failed: 0`. Then `for m in protocol porta custos; do (cd $m && go test -count=1 ./...); done` and (not beside the suite) `cd nodus/oevm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`.

- [ ] **Step 3: Board run on `c6spare`** (never `c6bench`). Resolve the port through the collector env files, not memory. Give `DebugProbeC6.nsl` a captured local in a kept block (as in Task 4 Step 4, called from `tick`), then from `nodus/oevm/zephyr`: `OEVM_DEBUG_HOOKS=1 ./run.sh --node DebugProbeC6.nsl` with the board's flash target, stop any reader of the port, and attach `custos debug -map nodus/oevm/zephyr/DebugProbeC6.oimg.map.json -port <by-id path>`. Session: `break <line in the kept block>`, `continue`, `bt`, `locals`, `locals 1`, `pause` + `bt` (a kernel stop, every frame named), `continue`. Save the log under `~/rete-bench/323/` and post the transcript, with the board, firmware commit and date, as a ≤ 5-line comment on #323 plus the transcript in a fenced block.

- [ ] **Step 4: Tutorial §6** gets that transcript (trimmed as the current one is, with the trim stated), with the date, board and log path. Re-run only the tutorial gate's own check if it can run alone (`rg -n 'def run_tutorial_gate' -A20 nodus/nsl/tools/run_tests_oevm.py`); otherwise the suite once more.

- [ ] **Step 5: Promote** the verb grammar (`dbg:backtrace`, `dbg:frames`, `c<k>=`, offsets in break/paused, no `dbg:methods`) into the mainline doc found above; `vars` is already in `protocol/IMAGE-ARTIFACTS.md` (Task 1). Add a line to `docs/design/2026-10-05-debugger-stack-and-names-design.md`'s top: `_Built: <commits>; promoted to <doc>, protocol/IMAGE-ARTIFACTS.md §3._` Rerun `tools/docmap.sh` if any doc moved (none should).

- [ ] **Step 6: Commit**

```bash
git add docs/tutorials/04-debugger.md docs/development.md nodus/nsl/docs/NEWSPEAK_DIALECT.md \
  nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md \
  docs/design/2026-10-05-debugger-stack-and-names-design.md nodus/oevm/zephyr/DebugProbeC6.nsl <the mainline doc>
git commit -m "docs: the debugger's bt and locals; tutorial 04 re-taken on c6spare, refs #323"
```

Close-out is David's merge; the #323 comment names the commits, the suite line, the Go gates and the board log.
