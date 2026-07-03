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


def _method_using(compiler, needle):
    """Return (instructions, constants) for the first solidified method whose
    id mentions `needle`."""
    for m in compiler.solidify.methods:
        if needle in m["id"]:
            return m["instructions"], m["constants"]
    raise AssertionError(f"no method id contains {needle!r}")


def _sends_to_const(insts, constants, op, name):
    """How many `op` instructions reference the constant string `name`."""
    if name not in constants:
        return 0
    cidx = constants.index(name)
    return sum(1 for w in insts
               if (w & 0xFF) == op and ((w >> 16) & 0xFF) == cidx)


# platform IS in scope (Probe takes `usingPlatform: platform`, so it is a slot
# reachable from the method): `Array` must route through platform via OP_SEND.
WITH_PLATFORM = """class Probe usingPlatform: platform = () (
public make = ( ^ Array new: 3 )
) : ( )
"""

# platform is NOT in scope (packageUsing: manifest): legacy implicit-send stays.
WITHOUT_PLATFORM = """class Probe packageUsing: manifest = () (
public make = ( ^ Array new: 3 )
) : ( )
"""


def test_capitalised_global_routes_through_platform(tmp_path):
    import ns_compile_oevm as C
    ast = _parse(WITH_PLATFORM, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()
    insts, consts = _method_using(compiler, "make")
    assert _sends_to_const(insts, consts, C.OP_SEND, "Array") >= 1, \
        "Array should be a plain OP_SEND to the platform receiver"
    assert _sends_to_const(insts, consts, C.OP_SEND_IMPLICIT, "Array") == 0, \
        "Array must NOT be an implicit-send-to-self when platform is in scope"


def test_capitalised_global_falls_back_without_platform(tmp_path):
    import ns_compile_oevm as C
    ast = _parse(WITHOUT_PLATFORM, tmp_path)
    compiler = C.OevmCompiler(ast)
    compiler.compile()
    insts, consts = _method_using(compiler, "make")
    assert _sends_to_const(insts, consts, C.OP_SEND_IMPLICIT, "Array") >= 1, \
        "without a platform in scope, Array keeps the legacy implicit-send"
