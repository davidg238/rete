"""rete#216 — the observation window, tested WITHOUT a board.

`bridge-health` returned PASS 3/3 in 22 s on 2026-08-16 against a build that was
dead four ticks later: the gate scores at tick 2 and the node died at tick 6
(#213). A target gate that passes and then lets the node die is a false green,
and it is invisible in exactly the case a target gate exists to catch.

The window itself is pure logic over the collector's log — "did new progress
lines keep arriving, and did a fault signature appear" — so it is falsifiable
here, at no hardware cost, and the board is then spent only on the thing that
genuinely needs one. That split matters: an assertion that can only be exercised
by flashing a C6 is an assertion that gets exercised rarely.

These drive the REAL bash function through `bash -c`, not a Python restatement
of it. A reimplementation would pass while the shell the harness actually runs
was broken.
"""
import subprocess
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIB = HERE / "lib" / "collect_console.sh"

BANNER = "2026-08-17T10:00:00 *** Booting Zephyr OS build ga4e6bc2-dirty-20260816T151140 ***"


def observe(logfile, progress_re, window_s, poll_s=1, build_id="ga4e6bc2-dirty-20260816T151140"):
    """Run collect::observe against a stub target::logfile pointing at `logfile`."""
    script = f"""
    set -uo pipefail
    target::logfile() {{ echo "{logfile}"; }}
    target::say() {{ echo "$*"; }}
    source "{LIB}"
    OBSERVE_POLL_S={poll_s}
    collect::observe "{build_id}" bridge-health "{progress_re}" {window_s} c6spare
    """
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=120)


def _ticker(path, stop, period=0.2):
    n = 100
    while not stop.is_set():
        with open(path, "a") as f:
            f.write(f"2026-08-17T10:00:01 tick {n} misses 0 cursor 2 acked {n*7} dropped 0\n")
        n += 1
        time.sleep(period)


def test_a_node_that_keeps_ticking_passes():
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\n")
        stop = threading.Event()
        t = threading.Thread(target=_ticker, args=(log, stop), daemon=True)
        t.start()
        try:
            r = observe(log, r"^.*tick [0-9]+", 3)
        finally:
            stop.set()
            t.join(timeout=5)
        assert r.returncode == 0, r.stdout + r.stderr


def test_a_node_that_stops_ticking_fails_on_the_stall():
    """#213's exact shape: the TAPs passed, then the console went silent. No
    grep for a fault signature can catch this — the failure IS the silence."""
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\n" + "".join(
            f"2026-08-17T10:00:0{i} tick {i} misses 0 cursor 2 acked {i*7} dropped 0\n"
            for i in range(1, 7)))
        r = observe(log, r"^.*tick [0-9]+", 3)
        assert r.returncode != 0, r.stdout
        assert "stall" in (r.stdout + r.stderr).lower(), r.stdout + r.stderr


def test_a_fault_line_during_the_window_fails_even_while_ticking():
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\n")
        stop = threading.Event()
        t = threading.Thread(target=_ticker, args=(log, stop), daemon=True)
        t.start()

        # The fault must arrive INSIDE the window, not before it opens. The
        # window marks the log's end when it starts, so a line already present
        # is deliberately out of scope — that is what keeps the boot banner's
        # own `rst:0x` from failing every healthy run.
        def late_fault():
            time.sleep(0.5)
            with open(log, "a") as f:
                f.write("2026-08-17T10:00:02 VM GC Error: Out of Memory -> abort()\n")
        f = threading.Thread(target=late_fault, daemon=True)
        f.start()
        try:
            r = observe(log, r"^.*tick [0-9]+", 3)
        finally:
            stop.set()
            t.join(timeout=5)
            f.join(timeout=5)
        assert r.returncode != 0, r.stdout
        assert "fault" in (r.stdout + r.stderr).lower(), r.stdout + r.stderr


def test_a_workload_with_no_ongoing_output_is_not_failed_for_silence():
    """stale-handle and pulse-coalesce are one-shot probes: they emit their TAPs
    and stop. Demanding progress from them would fail every run, so `none` is a
    legal progress declaration — but the fault scan still applies, which is the
    half of the window that is useful for a halting workload."""
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\nTAP stale-handle done pass=3 fail=0\n")
        r = observe(log, "none", 2)
        assert r.returncode == 0, r.stdout + r.stderr


def test_a_halting_workload_still_fails_on_a_fault():
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\nTAP stale-handle done pass=3 fail=0\n")
        stop = threading.Event()

        def late_fault():
            time.sleep(0.5)
            with open(log, "a") as f:
                f.write("2026-08-17T10:00:05 E: Guru Meditation Error: Core 0 panic'ed\n")
        t = threading.Thread(target=late_fault, daemon=True)
        t.start()
        try:
            r = observe(log, "none", 3)
        finally:
            stop.set()
            t.join(timeout=5)
        assert r.returncode != 0, r.stdout
        assert "fault" in (r.stdout + r.stderr).lower(), r.stdout + r.stderr


def test_the_boot_banner_before_the_window_is_not_read_as_a_reboot():
    """The anchor IS a boot banner and a C6 prints two of them, so a fault scan
    that started at the anchor would flag `rst:0x` on every healthy run — the
    window must only read what arrives AFTER the verdict."""
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(
            "2026-08-17T09:59:59 rst:0x1 (POWERON),boot:0x1c\n"
            + BANNER + "\n"
            + "2026-08-17T10:00:00 rst:0x1 (POWERON),boot:0x1c\n"
            + BANNER + "\n"
            + "TAP stale-handle done pass=3 fail=0\n")
        r = observe(log, "none", 2)
        assert r.returncode == 0, r.stdout + r.stderr


def test_a_none_scenario_does_not_claim_progress_it_never_checked():
    """Found by running the gate on hardware, 2026-08-17. Both probes declare
    `none` and both printed "still progressing, no fault" — a positive claim
    about liveness that was never asserted, in a harness whose entire purpose is
    that a gate must not report what it did not test. The fault half IS real for
    these scenarios; the progress half is not, and the line must say which."""
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\nTAP stale-handle done pass=3 fail=0\n")
        r = observe(log, "none", 2)
        assert r.returncode == 0, r.stdout + r.stderr
        assert "progressing" not in r.stdout, (
            "a none-progress scenario must not report progress it never checked:\n" + r.stdout)
        assert "no fault" in r.stdout, r.stdout
