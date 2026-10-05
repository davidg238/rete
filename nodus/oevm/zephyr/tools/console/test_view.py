# Copyright (c) 2026 Ekorau LLC
import datetime as dt
import http.client
import json
import subprocess
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

MAP = {"16": {"name": "Cbor>>u8At:", "file": 1, "lines": []},
       "32": {"name": "Cbor>>encode:", "file": 1, "lines": []}}

# A re-capture that legitimately counts zero activations is ordinary — this
# is NOT malformed data. The LAST window here has a header and no dump
# lines at all.
ZERO_ROW_LAST_WINDOW = (
    "2026-08-06T10:00:00.000-07:00 mprof[window]: methods=1 activations=5 overflow=0 cap=256\n"
    "2026-08-06T10:00:00.001-07:00 mprof: 16 5\n"
    "2026-08-06T10:05:00.000-07:00 mprof[window]: methods=0 activations=0 overflow=0 cap=256\n")


def test_latest_window_is_extracted_from_surrounding_traffic():
    rows, total = view.latest_window(LOG, MAP, top=10)
    assert total == 2
    assert rows[0][0] == "Cbor>>u8At:"
    assert rows[0][1] == 200


def test_no_window_reports_absence_rather_than_an_empty_table():
    rows, total = view.latest_window("2026-08-06T10:00:00.000-07:00 checkin ok\n", MAP)
    assert rows is None
    assert total == 0


def test_render_marks_an_incomplete_window():
    html = view.render(view.latest_window(LOG, MAP)[0], overflow=5, when='t')
    assert "INCOMPLETE" in html


def test_render_of_no_window_says_so():
    html = view.render(None, overflow=0)
    assert "no capture" in html.lower()


def test_render_includes_the_capture_time_when_given_one():
    """round 2: a capture found by searching backward into older days must
    say WHEN it was taken, or a three-day-old table reads as a current one
    — the exact false green that made the narrower `days()[-1]`-only search
    look safe by accident."""
    html = view.render(view.latest_window(LOG, MAP)[0], overflow=0,
                       when="2026-08-06T10:00:01.000-07:00")
    assert "2026-08-06T10:00:01.000-07:00" in html


def test_latest_window_does_not_merge_an_earlier_window_in_the_same_log():
    rows, total = view.latest_window(TWO_WINDOWS, MAP)
    assert total == 1
    assert len(rows) == 1
    assert rows[0][0] == "Cbor>>encode:"
    assert rows[0][1] == 9


def test_latest_window_answers_none_when_the_last_window_has_no_rows():
    """round 3, finding 5: the "is there a window" gate used to run against
    the WHOLE text while resolve() ran against the LAST window's slice — an
    earlier window's dump lines could pass the gate even though the last
    window (the one actually shown) was empty, reopening the None-vs-[]
    conflation this function exists to prevent."""
    rows, _ = view.latest_window(ZERO_ROW_LAST_WINDOW, MAP)
    assert rows is None


def test_render_of_a_zero_row_last_window_is_prose_not_a_headers_only_table():
    html = view.render(view.latest_window(ZERO_ROW_LAST_WINDOW, MAP)[0], overflow=0)
    assert "no capture" in html.lower()
    assert "<table>" not in html


def test_render_says_how_many_rows_it_is_not_showing():
    """final review, finding 2: the page rendered top=64 of a 177-row capture
    and said nothing at all about the other 113 — a truncated profile reads as
    a complete one, which is the argument mprof_resolve.py's own docstring
    makes and whose CLI already prints this sentence."""
    rows, total = view.latest_window(LOG, MAP, top=1)
    html = view.render(rows, overflow=0, when="t", total=total)
    assert "showing top 1 of 2 rows" in html
    assert "percentages are of ALL activations" in html


def test_render_is_silent_about_truncation_when_nothing_was_truncated():
    rows, total = view.latest_window(LOG, MAP)
    html = view.render(rows, overflow=0, when="t", total=total)
    assert "showing top" not in html


def test_render_says_so_when_the_capture_time_could_not_be_read():
    """final review, minor: showing the capture time is the CONDITION under
    which this view may search backward into older days, so its absence has to
    be visible. Omitting the line silently leaves a table of unknown age
    looking exactly like a fresh one."""
    html = view.render(view.latest_window(LOG, MAP)[0], overflow=0, when=None)
    assert "capture time unknown" in html


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
    sidecar.write_text(json.dumps({"methods": MAP}))
    view.Handler.board = "testboard"
    view.Handler.node = "testnode"
    view.Handler.sidecar = str(sidecar)
    view.Handler.top = 64
    # A path, never a real store: every test below replaces subprocess.run, so
    # nothing opens it. It is here because --porta-db is REQUIRED at the CLI and
    # a fixture that left it None would exercise a configuration the CLI cannot
    # produce.
    view.Handler.porta_db = str(tmp_path / "porta.db")
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


def test_get_health_answers_ok_when_the_page_actually_assembles(server):
    status, body = _get(server, "/health")
    assert status == 200
    assert body == b"ok"


def test_get_health_goes_red_when_the_page_only_assembles_a_banner(
        server, tmp_path):
    """final review, minor: /health returned `200 ok` unconditionally, so the
    one route anyone visits could be degraded — or, as in round 1, raising on
    every request — while the probe stayed green. It now builds the whole page
    and discards it, which is the only way it can report what it claims to."""
    view.Handler.sidecar = str(tmp_path / "does-not-exist.json")
    status, body = _get(server, "/health")
    assert status == 500
    assert b"not ok" in body


def test_get_health_goes_red_when_page_assembly_raises(server, monkeypatch):
    """Round 1's actual fault was a ValueError out of `PAGE % {...}` — NOT an
    OSError, so the narrow catch on GET / would not have converted it either.
    /health must be red for the class of fault nobody anticipated, which is the
    whole class it exists for."""
    def _boom(*a, **k):
        raise ValueError("unsupported format character")
    monkeypatch.setattr(view, "page", _boom)

    status, body = _get(server, "/health")
    assert status == 500
    assert b"not ok" in body


def test_get_unknown_path_is_404(server):
    status, _ = _get(server, "/nope")
    assert status == 404


def test_get_root_falls_back_to_an_older_day_and_names_when_it_was_taken(
        server, tmp_path):
    """round 2, finding 3: reading only `days()[-1]` (today) made a real
    capture in an OLDER file invisible the instant midnight rolled past it —
    Task 6's one hardware capture (177 rows, zero UNKNOWN) sat in yesterday's
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


def _post_capture(srv, payload=b"ms=500"):
    conn = http.client.HTTPConnection("127.0.0.1", srv.server_port, timeout=5)
    try:
        conn.request("POST", "/capture", body=payload,
                     headers={"Content-Length": str(len(payload))})
        resp = conn.getresponse()
        return resp.status, resp.read()
    finally:
        conn.close()


def test_post_capture_reports_when_porta_is_absent(server, monkeypatch):
    """The safe branch to exercise: porta missing from PATH. Never assert a
    successful `porta put` here — that would fire a real write at a real
    node on the bench."""
    def _missing(*a, **k):
        raise FileNotFoundError("porta")
    monkeypatch.setattr(view.subprocess, "run", _missing)

    status, body = _post_capture(server)
    assert status == 200
    assert b"capture NOT started" in body


def test_post_capture_surfaces_porta_own_stderr_on_a_nonzero_exit(
        server, monkeypatch):
    """final review, CRITICAL 1.3: `porta put`'s own message is the diagnostic.
    str(CalledProcessError) is only "returned non-zero exit status 1", which
    names nothing — and porta's stderr ("unable to open database file") is
    exactly the text that would have exposed the missing --db defect from the
    page instead of from a code review."""
    def _refuses(*a, **k):
        raise subprocess.CalledProcessError(
            1, a[0], output=b"", stderr=b"Error: unable to open database file\n")
    monkeypatch.setattr(view.subprocess, "run", _refuses)

    status, body = _post_capture(server)
    assert status == 200
    assert b"capture NOT started" in body
    assert b"unable to open database file" in body


def _fake_porta(calls, log_lines):
    """A stand-in for the `porta` binary. Records argv and answers `put` with
    porta's real `wrote <path> seq=<n>` line and `log` with `log_lines`."""
    class _R:
        def __init__(self, out):
            self.stdout, self.stderr, self.returncode = out, b"", 0

    def _run(argv, **kw):
        calls.append(argv)
        if "put" in argv:
            return _R(b"wrote dbg/req seq=7\n")
        return _R("\n".join(log_lines).encode())
    return _run


def test_post_capture_passes_the_operator_named_store_to_every_porta_call(
        server, monkeypatch):
    """final review, CRITICAL 1.1/1.2. Without --db, portacli defaults it to a
    RELATIVE "porta.db" and space.Open() runs the schema DDL on it — so sqlite
    CREATES an empty store in this process's cwd, the write "succeeds", and the
    page reports a capture that no served store and no node ever saw."""
    calls = []
    monkeypatch.setattr(view.subprocess, "run", _fake_porta(
        calls, ["2026-08-07T10:00:00Z → dbg/req  seq=7      david  'profile 500'"]))

    status, body = _post_capture(server)
    assert status == 200
    assert b"capture requested" in body
    assert len(calls) == 2                      # the put, then the read-back
    for argv in calls:
        assert "--db" in argv
        assert argv[argv.index("--db") + 1] == view.Handler.porta_db
    assert calls[1][1] == "log"                 # a STREAM is not in the cell tree


def test_post_capture_reports_a_write_that_exits_zero_and_lands_nowhere(
        server, monkeypatch):
    """The defect's own signature: exit 0, nothing in the store. `porta get`
    could not have caught this either way — dbg/req is a stream, so it never
    reaches the `cells` table `get` reads (space/store.go:342)."""
    calls = []
    monkeypatch.setattr(view.subprocess, "run", _fake_porta(
        calls, ["2026-08-07T09:00:00Z → goal/name  seq=6    david  'bench'"]))

    status, body = _post_capture(server)
    assert status == 200
    assert b"capture NOT started" in body
    assert b"exited 0" in body
