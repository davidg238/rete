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


# --- allocation attribution (design 2026-08-12) -----------------------------

# A window carrying BOTH line kinds, which is what a real log looks like. The
# no-frame bucket is deliberately non-zero: it is part of the denominator.
ALLOC_DUMP = (
    "mprof[window]: methods=2 activations=300 overflow=0 cap=256\n"
    "mprof: 16 200\n"
    "mprof: 32 100\n"
    "mprofa[window]: noframe_arena=100/2 noframe_cheap=0/0 allocOverflow=0\n"
    "mprofa: 16 600 12 0 0\n"
    "mprofa: 32 200 4 100 2\n"
)


def test_the_activation_regex_still_matches_when_alloc_rows_are_present():
    """The whole reason mprofa: is a separate prefix rather than extra columns.

    DUMP_RE is anchored at exactly two fields; widening the mprof: line would
    make it match NOTHING and report 'no mprof: lines found' — a silent false
    negative in the only tool that reads this output.
    """
    rows = mprof_resolve.DUMP_RE.findall(ALLOC_DUMP)
    assert rows == [("16", "200"), ("32", "100")]


def test_alloc_regex_does_not_swallow_activation_rows():
    rows = mprof_resolve.ALLOC_RE.findall(ALLOC_DUMP)
    assert rows == [("16", "600", "12", "0", "0"), ("32", "200", "4", "100", "2")]


def test_alloc_rows_sort_by_total_bytes_and_resolve_names():
    rows = mprof_resolve.resolve_alloc(MAP, ALLOC_DUMP)
    # u8At: 600+0 = 600; encode: 200+100 = 300. Denominator is 900 attributed
    # plus the 100-byte no-frame bucket = 1000.
    assert rows[0][0] == "Cbor>>u8At:"
    assert rows[0][1:5] == (600, 12, 0, 0)
    assert rows[0][5] == 60.0
    assert rows[1][0] == "Cbor>>encode:"
    assert rows[1][5] == 30.0


def test_noframe_bytes_are_in_the_denominator_not_dropped():
    """A run that allocates mostly outside any frame must NOT read as though a
    few methods account for everything."""
    heavy_noframe = (
        "mprofa[window]: noframe_arena=900/9 noframe_cheap=0/0 allocOverflow=0\n"
        "mprofa: 16 100 1 0 0\n"
    )
    rows = mprof_resolve.resolve_alloc(MAP, heavy_noframe)
    assert rows[0][5] == 10.0        # 100 of 1000, not 100 of 100


def test_alloc_unresolved_offset_keeps_the_offset_and_is_not_dropped():
    rows = mprof_resolve.resolve_alloc(
        MAP, "mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=0\n"
             "mprofa: 999 50 1 0 0\n")
    assert len(rows) == 1
    assert "UNKNOWN" in rows[0][0] and "999" in rows[0][0]


def test_alloc_report_warns_when_the_noframe_bucket_dominates(capsys):
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(MAP, f)
        map_file = f.name
    with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
        f.write("mprof[window]: methods=1 activations=1 overflow=0 cap=256\n")
        f.write("mprof: 16 1\n")
        f.write("mprofa[window]: noframe_arena=900/9 noframe_cheap=0/0 allocOverflow=0\n")
        f.write("mprofa: 16 100 1 0 0\n")
        log_file = f.name

    mprof_resolve.main([None, map_file, log_file, "--alloc"])
    captured = capsys.readouterr()
    assert "no-frame bucket EXCEEDS" in captured.out
    assert "distinct allocating methods: 1" in captured.out


def test_alloc_report_flags_a_saturated_table(capsys):
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(MAP, f)
        map_file = f.name
    with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
        f.write("mprof[window]: methods=1 activations=1 overflow=0 cap=256\n")
        f.write("mprof: 16 1\n")
        f.write("mprofa[window]: noframe_arena=0/0 noframe_cheap=0/0 allocOverflow=7\n")
        f.write("mprofa: 16 100 1 0 0\n")
        log_file = f.name

    mprof_resolve.main([None, map_file, log_file, "--alloc"])
    captured = capsys.readouterr()
    assert "INCOMPLETE" in captured.out and "allocOverflow=7" in captured.out


def test_top_does_not_shrink_the_distinct_count_or_the_noframe_denominator(capsys):
    """`--top N` is a DISPLAY flag, and both numbers below are properties of the
    window. It used to make "distinct allocating methods" print N — the D8
    sizing input reading back the flag — and compute the no-frame share against
    only the shown rows, which inflates the design's §8 risk metric in the
    direction that hides it (a bigger denominator would have made it smaller,
    but truncation drops rows from the denominator, so the printed share rises).
    """
    with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
        json.dump(MAP, f)
        map_file = f.name
    with tempfile.NamedTemporaryFile(mode='w', suffix='.log', delete=False) as f:
        f.write(ALLOC_DUMP)
        log_file = f.name

    mprof_resolve.main([None, map_file, log_file, "--alloc", "--top", "1"])
    out = capsys.readouterr().out
    # Two methods allocated; one is shown.
    assert "distinct allocating methods: 2" in out
    assert "showing top 1 of 2 allocating methods" in out
    # 100 no-frame bytes over 900 attributed + 100 = 10.0%, NOT 100/(600+100).
    assert "= 10.0% of all attributed bytes" in out
    assert "Cbor>>encode:" not in out.split("--- allocation attribution ---")[1]
