# Copyright (c) 2026 Ekorau LLC
import json
import tempfile
import mprof_resolve


MAP = {"16": {"id": "m16", "qual": ["Cbor>>u8At:"]},
       "32": {"id": "m32", "qual": ["Cbor>>encode:"]}}

# activations=300 counts only the SLOTTED rows; overflow=100 activations were
# dropped. The true denominator is 400, so u8At: is 50%, not 66.7%.
DUMP = ("mprof[dump]: methods=2 activations=300 overflow=100 cap=256\n"
        "mprof: 16 200\n"
        "mprof: 32 100\n")


def test_percent_denominator_includes_overflow():
    rows = mprof_resolve.resolve(MAP, DUMP)
    assert rows[0] == ("Cbor>>u8At:", 200, 50.0)
    assert rows[1] == ("Cbor>>encode:", 100, 25.0)


def test_percent_without_overflow_uses_activations():
    clean = ("mprof[dump]: methods=2 activations=300 overflow=0 cap=256\n"
             "mprof: 16 200\n"
             "mprof: 32 100\n")
    rows = mprof_resolve.resolve(MAP, clean)
    assert rows[0][2] == 200 / 300 * 100


def test_top_n_truncates_but_keeps_the_denominator():
    rows = mprof_resolve.resolve(MAP, DUMP, top=1)
    assert len(rows) == 1
    assert rows[0] == ("Cbor>>u8At:", 200, 50.0)


def test_unresolved_offset_keeps_the_offset_and_is_not_dropped():
    rows = mprof_resolve.resolve({}, DUMP)
    assert len(rows) == 2
    assert "16" in rows[0][0]
    assert "UNKNOWN" in rows[0][0]


def test_mixed_windows_warning_fires_on_sum_mismatch(capsys):
    """Rows summing to 250 but header saying activations=300 is a truncation."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(MAP, f)
        map_file = f.name
    with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
        f.write("mprof[dump]: methods=2 activations=300 overflow=0 cap=256\n")
        f.write("mprof: 16 200\n")
        f.write("mprof: 32 50\n")
        log_file = f.name

    mprof_resolve.main([None, map_file, log_file])
    captured = capsys.readouterr()
    assert "mixed windows" in captured.out


def test_mixed_windows_warning_does_not_fire_on_saturated_log_with_intact_rows(capsys):
    """Rows summing to 300 with overflow=100 should NOT fire the warning."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(MAP, f)
        map_file = f.name
    with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
        f.write("mprof[dump]: methods=2 activations=300 overflow=100 cap=256\n")
        f.write("mprof: 16 200\n")
        f.write("mprof: 32 100\n")
        log_file = f.name

    mprof_resolve.main([None, map_file, log_file])
    captured = capsys.readouterr()
    assert "mixed windows" not in captured.out
