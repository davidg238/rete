"""rete#416 — the parse cache: a hit is the parser's own answer, and an edited source misses."""
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ns_compile_oevm as C  # noqa: E402

SRC = "class P = () (\n  public f = ( ^ {} )\n)\n"


def test_hit_equals_parse_and_edit_misses(tmp_path, monkeypatch):
    monkeypatch.setenv("NSL_PARSE_CACHE", str(tmp_path / "cache"))
    src = tmp_path / "p.nsl"
    src.write_text(SRC.format(1))
    ok, cold = C.parse_file(str(src))
    assert ok
    assert len(list((tmp_path / "cache").iterdir())) == 1
    ok, warm = C.parse_file(str(src))
    assert ok and warm == cold
    src.write_text(SRC.format(2))
    ok, edited = C.parse_file(str(src))
    assert ok and edited != cold
    assert len(list((tmp_path / "cache").iterdir())) == 2


def test_parse_error_is_not_cached(tmp_path, monkeypatch):
    monkeypatch.setenv("NSL_PARSE_CACHE", str(tmp_path / "cache"))
    src = tmp_path / "bad.nsl"
    src.write_text("class = (\n")
    ok, err = C.parse_file(str(src))
    assert not ok and err
    assert not (tmp_path / "cache").exists() or not list((tmp_path / "cache").iterdir())
