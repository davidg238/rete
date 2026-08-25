import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
OEVM = TOOLS.parent.parent / "oevm"
# nodus/tests since the #79 consolidation (this file said ns-tests, a path the
# restructure deleted, and sat unrun while it rotted — rete#228's audit).
NS_TESTS = TOOLS.parent.parent / "tests"


def test_compile_method_call_to_oimg(tmp_path):
    # Build the host-ABI layout descriptor.
    subprocess.run(["gcc", "-std=c99", "-Isrc", "tools/dump_layout.c", "-o", str(tmp_path / "dl")],
                   cwd=OEVM, check=True)
    layout = tmp_path / "layout.json"
    layout.write_bytes(subprocess.run([str(tmp_path / "dl")], capture_output=True, check=True).stdout)

    src = NS_TESTS / "04_somns_basic" / "01_method_call.nsl"  # known-good minimal class+send
    out = tmp_path / "app.oimg"
    r = subprocess.run([sys.executable, str(TOOLS / "ns_compile_oevm.py"), str(src), str(out),
                        "--format=oimg", f"--layout={layout}"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr or r.stdout
    assert out.read_bytes()[:4] == b"OIMG"
