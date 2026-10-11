"""rete#37 — the kernel's do: on OP_ITER_AT (design docs/design/2026-10-11-37-iter-at-design.md).

D5: a kernel edit that breaks the recognised shape would fall back to the slow loop without
a word, so the two kernel do: methods are pinned here. D3: the shape is recognised in kernel
source only — the same loop in an app keeps OP_AT.
"""
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ns_compile_oevm as C  # noqa: E402

APP = (
    "class IterAtApp packageUsing: manifest = () (\n"
    "public main: platform args: args = (\n"
    "  | a s | a:: platform Array new: 3. s:: 0.\n"
    "  1 to: a size do: [ :i | s:: s + (a basicAt: i) ].\n"
    "  ^ s\n"
    ")\n"
    ") : (\n"
    ")\n"
)


def compiled(tmp_path):
    src = tmp_path / "iter_at_app.nsl"
    src.write_text(APP)
    return C.compile_source(str(src), 8)


def ops(method):
    return [i & 0xFF for i in method.instructions]


def test_kernel_do_methods_use_iter_at(tmp_path):
    c = compiled(tmp_path)
    for name in ("Kernel_Array>>do:", "Kernel_OrderedCollection>>do:"):
        o = ops(c.method_compilers[name])
        assert o.count(C.OP_ITER_AT) == 1, name
        assert C.OP_AT not in o, name


def test_app_loop_keeps_op_at(tmp_path):
    c = compiled(tmp_path)
    app = [m for k, m in c.method_compilers.items() if k.endswith("main:args:")
           and "IterAtApp" in k]
    assert len(app) == 1
    o = ops(app[0])
    assert C.OP_ITER_AT not in o
    assert C.OP_AT in o
