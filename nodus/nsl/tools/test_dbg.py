"""dbg.py's two translations (rete#311), without a VM: file:line -> (id, offset) through
the .stmap and the registry, and a `dbg:paused` (id, offset) -> file:line. The shapes
are 47_debug_pause_target's, as the compiler writes them; the end-to-end run against a
real VM is in test_debugger_integration.py."""
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dbg import SourceMap

STMAP = {
    "source": "t.nsl",
    "entry": "T",
    "functions": {
        "T>>main:args:": [[1, 19], [5, 20], [21, 24], [26, 28], [51, 32]],
        "T>>countTo:": [[0, 36], [2, 37], [16, 38]],
        "T_block2>>value:": [[1, 25], [5, 26]],
        "Kernel>>pumpEvents": [[0, 4000]],
    },
}
REGISTRY = ["dbg:ready", "1 #main:args: 77", "2 #countTo: 77", "3 #init_platform: 77",
            "4 #printString 80", "dbg:ok methods"]


@pytest.fixture
def sm(tmp_path):
    p = tmp_path / "t.stmap"
    p.write_text(json.dumps(STMAP))
    (tmp_path / "t.nsl").write_text("\n".join("line %d" % n for n in range(1, 41)))
    s = SourceMap(str(p))
    s.load_registry(REGISTRY)
    return s


def test_a_line_resolves_to_its_method_and_first_offset(sm):
    assert sm.resolve("37") == (2, 2, 37, "countTo:")
    assert sm.resolve("t.nsl:20") == (1, 5, 20, "main:args:")


def test_a_line_without_code_inside_a_method_moves_forward(sm):
    assert sm.resolve("30") == (1, 51, 32, "main:args:")


def test_a_line_outside_every_method_names_its_neighbours(sm):
    with pytest.raises(ValueError, match=r"line 34 has no code.*32 \(main:args:\), 36 \(countTo:\)"):
        sm.resolve("34")


def test_a_line_in_a_block_is_refused_not_mapped_to_its_home(sm):
    # 26 lies inside main:args:'s span (19-32); taking the span would break at line 28.
    with pytest.raises(ValueError, match=r"T_block2>>value:"):
        sm.resolve("26")


def test_another_file_is_refused(sm):
    with pytest.raises(ValueError, match="built from t.nsl"):
        sm.resolve("other.nsl:37")


def test_the_registry_keeps_only_the_entry_group_for_lines(sm):
    assert sm.ids == {"main:args:": 1, "countTo:": 2, "init_platform:": 3}
    assert sm.sels[4] == ("printString", False)


def test_a_stop_reads_back_as_the_line_of_the_last_statement_started(sm):
    assert sm.where(2, 2) == ("at t.nsl:37  countTo:", "  37 | line 37")
    assert sm.where(2, 9)[0] == "at t.nsl:37  countTo:"
    assert sm.where(1, 0)[0] == "at t.nsl:19  main:args:"     # before the first entry


def test_a_stop_outside_the_app_says_so(sm):
    assert sm.where(-1, 3)[0].startswith("outside the registry")
    assert sm.where(4, 0)[0] == "in the kernel's printString, offset 0"


def test_an_old_stmap_without_an_entry_is_refused_by_name(tmp_path):
    p = tmp_path / "old.stmap"
    p.write_text(json.dumps({"source": "t.nsl", "functions": {}}))
    with pytest.raises(ValueError, match="names no entry class"):
        SourceMap(str(p))
