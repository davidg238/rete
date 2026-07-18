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
# BOTH reuse the same register (3), so the capture-slot sort ties on register.
# The (register, name) total-order tie-break must give a stable mapping every
# run (a plain-register key would fall back to hash-randomized set order).
SRC_TIED_REGS = (
    'class T = () ( m = ( | n | '
    '1 to: 3 do: [ :i | [ i ] value ]. '
    '1 to: 3 do: [ :j | [ j ] value ]. ^ n ) ) : ( )'
)


def test_tied_register_capture_slots_are_deterministic(tmp_path):
    # Compile twice in-process; the mapping must be identical and exact.
    m1 = compile_source(SRC_TIED_REGS, tmp_path).method_compilers["T>>m"]
    m2 = compile_source(SRC_TIED_REGS, tmp_path).method_compilers["T>>m"]

    assert m1.captured_locals == {"i", "j"}
    # `i` and `j` share the same original register -> tie broken by name.
    assert m1._capture_regs["i"] == m1._capture_regs["j"]
    # Exact, stable slot assignment (i before j by name tie-break).
    assert m1.capture_slot == {"i": 0, "j": 1}
    assert m1.capture_slot == m2.capture_slot


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
