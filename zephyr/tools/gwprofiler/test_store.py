from store import Store

def test_latest_cell_returns_newest_by_seq(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    s.upsert_cell("n1", "diag/spans/result", 1, {"_tag": "SpanCapture", "n": 2}, 100.0)
    s.upsert_cell("n1", "diag/spans/result", 2, {"_tag": "SpanCapture", "n": 5}, 200.0)
    assert s.latest_cell("n1", "diag/spans/result")["n"] == 5

def test_events_are_append_only(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    s.write_event("n1", "diag/spans/result", 1, {"n": 2}, 100.0, "in")
    s.write_event("n1", "diag/spans/result", 2, {"n": 5}, 200.0, "in")
    assert s.event_count("n1") == 2

def test_missing_cell_is_none(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    assert s.latest_cell("n1", "diag/spans/result") is None
