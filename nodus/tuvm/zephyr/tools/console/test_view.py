# Copyright (c) 2026 Ekorau LLC
import view


LOG = ("2026-08-06T10:00:00.000-07:00 checkin ok\n"
       "2026-08-06T10:00:01.000-07:00 mprof[window]: methods=2 activations=300 overflow=0 cap=256\n"
       "2026-08-06T10:00:01.001-07:00 mprof: 16 200\n"
       "2026-08-06T10:00:01.002-07:00 mprof: 32 100\n")

MAP = {"16": {"id": "m16", "qual": ["Cbor>>u8At:"]},
       "32": {"id": "m32", "qual": ["Cbor>>encode:"]}}


def test_latest_window_is_extracted_from_surrounding_traffic():
    rows = view.latest_window(LOG, MAP, top=10)
    assert rows[0][0] == "Cbor>>u8At:"
    assert rows[0][1] == 200


def test_no_window_reports_absence_rather_than_an_empty_table():
    rows = view.latest_window("2026-08-06T10:00:00.000-07:00 checkin ok\n", MAP)
    assert rows is None


def test_render_marks_an_incomplete_window():
    html = view.render(view.latest_window(LOG, MAP), overflow=5)
    assert "INCOMPLETE" in html


def test_render_of_no_window_says_so():
    html = view.render(None, overflow=0)
    assert "no capture" in html.lower()
