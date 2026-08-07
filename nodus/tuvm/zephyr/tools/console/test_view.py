# Copyright (c) 2026 Ekorau LLC
import http.client
import threading

import pytest

import view


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


def test_latest_window_does_not_merge_an_earlier_window_in_the_same_log():
    rows = view.latest_window(TWO_WINDOWS, MAP)
    assert len(rows) == 1
    assert rows[0][0] == "Cbor>>encode:"
    assert rows[0][1] == 9


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
def server(monkeypatch):
    view.Handler.board = "testboard"
    view.Handler.node = "testnode"
    view.Handler.sidecar = None
    view.Handler.top = 64
    monkeypatch.setattr(view.Handler, "_log_text", lambda self: LOG)
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
