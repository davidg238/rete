"""rete#247 part 1 — compiler errors carry file:line:col + the offending source line.

The Ohm parse stage already positions its refusals; these pin the POST-parse
CompileErrors, which used to print the message alone. The 'Compile error: '
prefix is the frozen substring; everything after it is the enrichment.
"""
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent


def compile_expecting_error(tmp_path, source):
    src = tmp_path / "diag_app.nsl"
    src.write_text(source)
    out = tmp_path / "diag_app.c"   # explicit output: NEVER let the default bake
    r = subprocess.run([sys.executable, str(TOOLS / "ns_compile_oevm.py"),
                        str(src), str(out)],
                       capture_output=True, text=True)
    assert r.returncode == 1, f"expected compile failure, got rc={r.returncode}\n{r.stdout}{r.stderr}"
    assert "Compile error: " in r.stdout, r.stdout   # the frozen substring survives
    return src, r.stdout


def test_nlr_error_names_file_line_and_source(tmp_path):
    # '^' inside an escaping closure (a block passed to a real method) — the
    # no-NLR refusal. The offending statement is on line 4.
    src, out = compile_expecting_error(tmp_path, (
        "class DiagNlr packageUsing: manifest = () (\n"
        "public main: platform args: args = (\n"
        "  | c | c:: platform OrderedCollection new.\n"
        "  c do: [ :e | ^ e ].\n"
        ")\n"
        ") : (\n"
        ")\n"
    ))
    assert f"{src}:4:" in out, out
    assert "c do: [ :e | ^ e ]." in out, out         # the offending source line, quoted
    assert "non-local return" in out, out            # the message itself still present


def test_arrow_error_names_the_arrow_line(tmp_path):
    # rete#245's refusal: '->' with no `platform` in scope. The arrow is on
    # line 3 — the diagnostic must point there, not at the method or class.
    src, out = compile_expecting_error(tmp_path, (
        "class DiagArrow packageUsing: manifest = () (\n"
        "public pair = (\n"
        "  ^ 1 -> 2\n"
        ")\n"
        "public main: platform args: args = ( ^ nil )\n"
        ") : (\n"
        ")\n"
    ))
    assert f"{src}:3:" in out, out
    assert "'->' needs `platform` in scope" in out, out
