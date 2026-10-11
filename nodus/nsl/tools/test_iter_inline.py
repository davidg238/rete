"""rete#37 — kernel iterators with a literal block, inlined at the call site behind a class guard
(design docs/design/2026-10-11-37-do-inline-design.md).

The compiler's loops reproduce six kernel methods by hand, so those methods are pinned here: a kernel
edit to any of them, or an OrderedCollection override of an inherited one, fails this test rather
than leaving inlined call sites that disagree with the kernel. Off in a debug build (Q3).
"""
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ns_compile_oevm as C  # noqa: E402

KERNEL_METHODS = [
    # Array
    "public do: aBlock = ( 1 to: self size do: [ :i | aBlock value: (self basicAt: i) ]. ^ self )",
    # OrderedCollection
    "public do: aBlock = ( 1 to: count do: [ :i | aBlock value: (contents basicAt: i) ]. ^ self )",
    # Collection, inherited by OrderedCollection
    "public inject: a into: b = ( | acc | acc:: a. self do: [ :e | acc:: b value: acc value: e ]. ^ acc )",
    "public collect: b = ( | r | r:: OrderedCollection new. self do: [ :e | r add: (b value: e) ]. ^ r )",
    "public select: b = ( | r | r:: OrderedCollection new. self do: [ :e | (b value: e) ifTrue: [ r add: e ] ]. ^ r )",
    "public reject: b = ( | r | r:: OrderedCollection new. self do: [ :e | (b value: e) ifFalse: [ r add: e ] ]. ^ r )",
]

APP = (
    "class IterInlineApp packageUsing: manifest = () (\n"
    "public main: platform args: args = (\n"
    "  | oc s |\n"
    "  oc:: platform OrderedCollection new. s:: 0.\n"
    "  oc do: [ :e | s:: s + e ].\n"
    "  oc collect: [ :e | e ].\n"
    "  oc select: [ :e | e > 1 ].\n"
    "  oc reject: [ :e | e > 1 ].\n"
    "  oc inject: 0 into: [ :a :e | a + e ].\n"
    "  oc do: [ :e | oc add: [ e ] ].\n"
    "  ^ s\n"
    ")\n"
    ") : (\n"
    ")\n"
)


def compiled(tmp_path):
    src = tmp_path / "iter_inline_app.nsl"
    src.write_text(APP)
    return C.compile_source(str(src), 8)


def app_ops(c):
    app = [m for k, m in c.method_compilers.items() if k.endswith("main:args:") and "IterInlineApp" in k]
    assert len(app) == 1
    return [i & 0xFF for i in app[0].instructions]


def test_kernel_methods_are_the_ones_inlined():
    kernel = Path(C.KERNEL_PATH).read_text()
    for m in KERNEL_METHODS:
        assert kernel.count(m) == 1, m


def test_orderedcollection_inherits_the_collection_methods(tmp_path):
    c = compiled(tmp_path)
    for sel in ("inject:into:", "collect:", "select:", "reject:"):
        assert f"Kernel_OrderedCollection>>{sel}" not in c.method_compilers, sel


def test_guards_per_site(tmp_path):
    o = app_ops(compiled(tmp_path))
    # do: guards OrderedCollection and Array; the four others OrderedCollection only. The last do:
    # captures its element in a nested closure, so it keeps the send (#417).
    assert o.count(C.OP_IS_VT) == 2 + 4
    assert o.count(C.OP_ITER_AT) == 2 + 4


def test_off_in_a_debug_build(tmp_path, monkeypatch):
    monkeypatch.setattr(C, "ITER_INLINE", False)
    o = app_ops(compiled(tmp_path))
    assert C.OP_IS_VT not in o
    assert C.OP_ITER_AT not in o
