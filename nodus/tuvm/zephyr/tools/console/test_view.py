# Copyright (c) 2026 Ekorau LLC
import datetime as dt
import http.client
import json
import threading

import pytest

import logstore
import view

# Captured before any test monkeypatches view.logstore.LogStore below: that
# patch replaces the attribute on the SHARED module object (this file's
# `logstore` and view.py's `view.logstore` are the same `sys.modules` entry),
# so a fixture that called `logstore.LogStore(...)` after patching would be
# calling its own replacement, not the real class.
_RealLogStore = logstore.LogStore


LOG = ("2026-08-06T10:00:00.000-07:00 checkin ok\n"
       "2026-08-06T10:00:01.000-07:00 mprof[window]: methods=2 activations=300 overflow=0 cap=256\n"
       "2026-08-06T10:00:01.001-07:00 mprof: 16 200\n"
       "2026-08-06T10:00:01.002-07:00 mprof: 32 100\n")

# Two windows in one day's log — realistic, since a day's file is never
# rotated mid-day. resolve() sums every `mprof:` line it is GIVEN, so handing
# it the whole log would merge these under the SECOND header, silently.
TWO_WINDOWS = (
    "2026-08-06T10:00:00.000-07:00 mprof[window]: methods=1 activations=5 overflow=0 cap=256\n"
    "2026-08-06T10:00:00.001-07:00 mprof: 16 5\n"
    "2026-08-06T10:05:00.000-07:00 mprof[window]: methods=1 activations=9 overflow=0 cap=256\n"
    "2026-08-06T10:05:00.001-07:00 mprof: 32 9\n")

MAP = {"16": {"id": "m16", "qual": ["Cbor>>u8At:"]},
       "32": {"id": "m32", "qual": ["Cbor>>encode:"]}}

# A re-capture that legitimately counts zero activations is ordinary — this
# is NOT malformed data. The LAST window here has a header and no dump
# lines at all.
ZERO_ROW_LAST_WINDOW = (
    "2026-08-06T10:00:00.000-07:00 mprof[window]: methods=1 activations=5 overflow=0 cap=256\n"
    "2026-08-06T10:00:00.001-07:00 mprof: 16 5\n"
    "2026-08-06T10:05:00.000-07:00 mprof[window]: methods=0 activations=0 overflow=0 cap=256\n")


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


def test_render_includes_the_capture_time_when_given_one():
    """round 2: a capture found by searching backward into older days must
    say WHEN it was taken, or a three-day-old table reads as a current one
    — the exact false green that made the narrower `days()[-1]`-only search
    look safe by accident."""
    html = view.render(view.latest_window(LOG, MAP), overflow=0,
                       when="2026-08-06T10:00:01.000-07:00")
    assert "2026-08-06T10:00:01.000-07:00" in html


def test_latest_window_does_not_merge_an_earlier_window_in_the_same_log():
    rows = view.latest_window(TWO_WINDOWS, MAP)
    assert len(rows) == 1
    assert rows[0][0] == "Cbor>>encode:"
    assert rows[0][1] == 9


def test_latest_window_answers_none_when_the_last_window_has_no_rows():
    """round 3, finding 5: the "is there a window" gate used to run against
    the WHOLE text while resolve() ran against the LAST window's slice — an
    earlier window's dump lines could pass the gate even though the last
    window (the one actually shown) was empty, reopening the None-vs-[]
    conflation this function exists to prevent."""
    rows = view.latest_window(ZERO_ROW_LAST_WINDOW, MAP)
    assert rows is None


def test_render_of_a_zero_row_last_window_is_prose_not_a_headers_only_table():
    html = view.render(view.latest_window(ZERO_ROW_LAST_WINDOW, MAP), overflow=0)
    assert "no capture" in html.lower()
    assert "<table>" not in html


# --- page assembly -----------------------------------------------------
#
# Round 1 shipped with a literal `%` in PAGE's inlined CSS, which made
# `PAGE % {...}` raise ValueError on EVERY request while /health kept
# answering ok — the exact "process alive, pipeline dead" shape #147 already
# named for the collector's own health check. Nothing below called page()
# or hit a route, so 70/70 passed through a total outage of the one route
# anyone visits.

def test_page_assembly_does_not_raise_and_keeps_the_css_percent_literal():
    out = view.page("c6spare", "<div id='table'>rows</div>")
    assert "width:100%" in out           # not mangled, not %-consumed
    assert "c6spare" in out
    assert "<div id='table'>rows</div>" in out


# --- routes, as routes ---------------------------------------------------
#
# A real HTTPServer on an ephemeral port, driven over an actual socket —
# calling do_GET()/do_POST() directly would not have caught round 1 either,
# since the ValueError fires inside the %-substitution that only runs when a
# request is actually dispatched through the handler machinery.

@pytest.fixture
def server(tmp_path, monkeypatch):
    """A real view.Handler on an ephemeral port, backed by a real LogStore
    rooted at `tmp_path` (seeded with one day, 2026-08-06, holding LOG) —
    not a monkeypatched `_log_text`, because round 2's bug was in HOW the
    handler chose which day to read, and a fixture that hands it text
    directly cannot exercise that choice at all.

    `view.logstore.LogStore` is replaced with a factory pinned to `tmp_path`
    rather than passing a root through Handler, since Handler.--board is
    the only knob the real CLI exposes and adding a test-only --root would
    be a second way to point at a store."""
    sidecar = tmp_path / "sidecar.json"
    sidecar.write_text(json.dumps(MAP))
    view.Handler.board = "testboard"
    view.Handler.node = "testnode"
    view.Handler.sidecar = str(sidecar)
    view.Handler.top = 64
    store = _RealLogStore(str(tmp_path), board="testboard")
    with open(store.path_for(dt.date(2026, 8, 6)), "w", encoding="utf-8") as fh:
        fh.write(LOG)
    monkeypatch.setattr(view.logstore, "LogStore",
                        lambda board=None, **kw: _RealLogStore(str(tmp_path), board=board))
    srv = view.HTTPServer(("127.0.0.1", 0), view.Handler)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield srv
    finally:
        srv.shutdown()
        thread.join()
        srv.server_close()


def _get(srv, path):
    conn = http.client.HTTPConnection("127.0.0.1", srv.server_port, timeout=5)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


def test_get_root_returns_a_real_page_with_bytes_not_a_dead_connection(server):
    status, body = _get(server, "/")
    assert status == 200
    assert len(body) > 0
    assert b"node profile" in body
    assert b"width:100%" in body


def test_get_health_answers_ok_independent_of_root(server):
    status, body = _get(server, "/health")
    assert status == 200
    assert body == b"ok"


def test_get_unknown_path_is_404(server):
    status, _ = _get(server, "/nope")
    assert status == 404


def test_get_root_falls_back_to_an_older_day_and_names_when_it_was_taken(
        server, tmp_path):
    """round 2, finding 3: reading only `days()[-1]` (today) made a real
    capture in an OLDER file invisible the instant midnight rolled past it —
    Task 6's one hardware capture (181 rows, zero UNKNOWN) sat in yesterday's
    file while today's held nothing, and the view said 'No capture' as if it
    had never happened. The `server` fixture already seeded 2026-08-06 with
    LOG (one window); this adds a NEWER, window-less day on top of it and
    the page must still find and show the older one — WITH its own capture
    time, not today's, so a stale table cannot be mistaken for a fresh one."""
    store = _RealLogStore(str(tmp_path), board="testboard")
    with open(store.path_for(dt.date(2026, 8, 7)), "w", encoding="utf-8") as fh:
        fh.write("2026-08-07T09:00:00.000-07:00 checkin ok\n")   # no window

    status, body = _get(server, "/")
    assert status == 200
    assert b"No capture" not in body
    assert b"u8At" in body                        # the OLDER day's rows
    assert b"2026-08-06T10:00:01" in body          # its OWN capture time
    assert b"2026-08-07" not in body               # not today's date


def test_get_root_reports_an_unreadable_sidecar_rather_than_dying_silently(
        server, tmp_path, capsys):
    """round 3, finding 6: a missing/unreadable/malformed sidecar used to
    crash do_GET with nothing sent to the client — while GET /health kept
    answering ok throughout, the same silent-outage shape round 1 fixed one
    specific cause of. Caught narrowly (OSError, json.JSONDecodeError), not
    a bare `except Exception` that would also have hidden round 1's bug."""
    view.Handler.sidecar = str(tmp_path / "does-not-exist.json")

    status, body = _get(server, "/")
    assert status == 200                       # not a dead connection
    assert b"error reading the capture" in body
    assert b"node profile" in body             # the page shell still renders
    assert "Traceback" in capsys.readouterr().err   # the diagnosis reaches the log


def test_post_capture_reports_when_porta_is_absent(server, monkeypatch):
    """The safe branch to exercise: porta missing from PATH. Never assert a
    successful `porta put` here — that would fire a real write at a real
    node on the bench."""
    def _missing(*a, **k):
        raise FileNotFoundError("porta")
    monkeypatch.setattr(view.subprocess, "run", _missing)

    conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=5)
    try:
        payload = b"ms=500"
        conn.request("POST", "/capture", body=payload,
                     headers={"Content-Length": str(len(payload))})
        resp = conn.getresponse()
        status, body = resp.status, resp.read()
    finally:
        conn.close()
    assert status == 200
    assert b"capture NOT started" in body
