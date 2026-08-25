import json
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
GRAMMAR = TOOLS.parent / "grammar" / "ns-parse.js"
sys.path.insert(0, str(TOOLS))


def _parse(src_text, tmp_path):
    f = tmp_path / "frag.nsl"
    f.write_text(src_text)
    r = subprocess.run(["node", str(GRAMMAR), str(f)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def compile_source(src_text, tmp_path):
    """Hermetic compile entry for tests: parse via the real node grammar, then
    drive MethodCompiler exactly like main() does (mirrors test_source_map.py's
    _parse + OevmCompiler idiom). Returns the OevmCompiler so callers can pull
    individual MethodCompiler frames out of compiler.method_compilers, keyed
    "Vtable>>selector" (same keying as source_maps)."""
    import ns_compile_oevm as C
    ast = _parse(src_text, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()
    return compiler


# `a` is read+written inside the nested block, `b` is read -> both captured in
# `m`'s frame. The block's own params/temps are irrelevant here (none).
SRC_TWO_CAPTURED = """
class T = () (
  m = ( | a b | a:: 1. b:: 2. [ a:: a + 1. b ] value. ^ a )
) : ( )
"""

# `x` is only ever touched in its own frame (no nested block) -> not captured.
SRC_NONE_CAPTURED = 'class T = () ( m = ( | x | x:: 5. ^ x ) ) : ( )'


def test_only_referenced_locals_captured(tmp_path):
    compiler = compile_source(SRC_TWO_CAPTURED, tmp_path)
    m = compiler.method_compilers["T>>m"]

    assert m.captured_locals == {"a", "b"}
    # Dense slot assignment, stable order.
    assert set(m.capture_slot.keys()) == {"a", "b"}
    assert sorted(m.capture_slot.values()) == [0, 1]


def test_uncaptured_locals_stay_registers(tmp_path):
    compiler = compile_source(SRC_NONE_CAPTURED, tmp_path)
    m = compiler.method_compilers["T>>m"]

    assert m.captured_locals == set()
    assert m.capture_slot == {}


# Two sequential `to:do:` loops: the inlined-block induction vars `i` and `j`
# BOTH reuse the same register (3), and each is captured by a nested real
# block. Under the Task 4 option-b contract (see the capture_slot assignment
# comment in ns_compile_oevm.py) these are NOT Context-homed: an inlined-loop
# induction var lives in an inlined-block child Scope, not the frame's
# local_vars, and the loop codegen reads/writes it via a plain register —
# Context-homing it would rewrite the loop's OWN same-frame reads to a slot
# nobody writes -> nil. So they appear in captured_locals (the analysis) but
# are EXCLUDED from capture_slot (the homing), staying register+home_ctx-homed.
# This test used to pin the pre-Task-4 contract ({"i": 0, "j": 1}) and sat in
# an unrun file while the contract changed under it (rete#228's audit).
SRC_TIED_REGS = (
    'class T = () ( m = ( | n | '
    '1 to: 3 do: [ :i | [ i ] value ]. '
    '1 to: 3 do: [ :j | [ j ] value ]. ^ n ) ) : ( )'
)


def test_tied_register_inlined_captures_stay_register_homed(tmp_path):
    # Compile twice in-process; the analysis and the homing must be identical.
    m1 = compile_source(SRC_TIED_REGS, tmp_path).method_compilers["T>>m"]
    m2 = compile_source(SRC_TIED_REGS, tmp_path).method_compilers["T>>m"]

    assert m1.captured_locals == {"i", "j"}
    # `i` and `j` share the same original register — the tie the old slot sort
    # broke by name; still asserted so a register-allocator change that stops
    # the sharing shows up here rather than silently weakening the test.
    assert m1._capture_regs["i"] == m1._capture_regs["j"]
    # The current contract: induction vars are excluded from Context homing.
    assert m1.capture_slot == {}
    assert m2.capture_slot == {}


def test_inlined_block_local_not_captured(tmp_path):
    # `i` is referenced ONLY inside its own inlined `to:do:` block (frame_depth
    # 0 — inlined blocks share the enclosing frame), so it is NOT captured.
    src = ('class T = () ( m = ( | n | '
           '1 to: 3 do: [ :i | n:: n + i ]. ^ n ) ) : ( )')
    m = compile_source(src, tmp_path).method_compilers["T>>m"]
    assert m.captured_locals == set()
    assert m.capture_slot == {}


def test_depth_gt_1_attributes_to_defining_frame(tmp_path):
    # `a` is read from a block nested two frames deep (`[ [ a ] value ] value`);
    # the capture must be attributed to `m`'s frame (the definer), not an
    # intermediate block frame.
    src = 'class T = () ( m = ( | a | [ [ a ] value ] value. ^ a ) ) : ( )'
    m = compile_source(src, tmp_path).method_compilers["T>>m"]
    assert m.captured_locals == {"a"}
    assert m.capture_slot == {"a": 0}
