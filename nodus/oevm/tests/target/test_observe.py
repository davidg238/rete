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


def test_a_wedge_the_detector_caught_is_named_as_such_not_as_213s_signature():
    """rete#223. The window used to reach its verdict BEFORE the evidence arrived: the
    stall poll was 10 s and the #215 detector's deadline for the bridge is 8 x 2000 ms,
    so on 2026-08-19 the harness printed "no fault, no panic and no reboot" eighteen
    seconds before the dump that named the condition landed in the same log.

    FAIL is still the right verdict — a node that wedged and rebooted inside an
    `observe` window has died. It is the REASON that was wrong, and the reason is the
    part a human acts on: #213's signature means "unexplained silent death, go hunt",
    while a `oevm: WEDGE` line means "the detector did its job, read the dump"."""
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\n" + "".join(
            f"2026-08-17T10:00:0{i} tick {i} misses 0 cursor 2 acked {i*7} dropped 0\n"
            for i in range(1, 7)))

        def late_wedge():
            time.sleep(0.5)
            with open(log, "a") as f:
                f.write("2026-08-17T10:00:31 oevm: WEDGE cond=pulse-undrained "
                        "overdue=16ms deadline=16000ms up=31424ms pending=1 pdr=0 "
                        "pco=8 pst=0 consec=1\n")
                f.write("2026-08-17T10:00:31 rst:0xc (SW_CPU),boot:0x4c\n")
        t = threading.Thread(target=late_wedge, daemon=True)
        t.start()
        try:
            r = observe(log, r"^.*tick [0-9]+", 6, poll_s=3)
        finally:
            t.join(timeout=5)

        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert "pulse-undrained" in out, (
            "the failure must name the condition the detector reported:\n" + out)
        assert "no fault, no panic and no reboot" not in out, (
            "the window must not claim an absence it can see is false:\n" + out)
        assert "This is #213's signature" not in out, (
            "a wedge the detector CAUGHT must not be reported as #213's unexplained "
            "silence — that sends the reader hunting for a mechanism the dump already "
            "names:\n" + out)


def test_a_stall_does_not_claim_an_absence_it_cannot_yet_know():
    """rete#223, the other half. When the harness reaches its verdict FIRST — its poll is
    shorter than the node's own wedge deadline, which for the bridge is 8 x 2000 ms — it
    cannot know whether a detector dump is still in flight. It used to say "with no fault,
    no panic and no reboot" anyway, and on 2026-08-19 that sentence was refuted eighteen
    seconds later by a dump in the same log.

    Same rule the `none`-progress scenarios already follow: say what was checked, not what
    is merely absent so far."""
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "console.log"
        log.write_text(BANNER + "\n" + "".join(
            f"2026-08-17T10:00:0{i} tick {i} misses 0 cursor 2 acked {i*7} dropped 0\n"
            for i in range(1, 7)))
        r = observe(log, r"^.*tick [0-9]+", 3)
        out = r.stdout + r.stderr
        assert r.returncode != 0, out
        assert "stall" in out.lower(), out
        assert "no fault, no panic and no reboot" not in out, (
            "the window polls faster than a node's wedge deadline, so at this instant it "
            "does not know that:\n" + out)


def test_no_manifest_key_can_leak_into_the_next_scenario():
    """rete#223. `run.sh` sources each manifest into the SAME shell, so a key one scenario
    sets is still set when the next one — which omits it — is sourced. That has already
    happened once: TAP_NAME got an explicit `unset` after the fact. The hazard is not the
    key, it is that adding a key means remembering a second place, so this asserts the
    invariant over EVERY key any manifest declares rather than over a list someone
    maintains by hand."""
    run_sh = (HERE / "run.sh").read_text()
    reset = run_sh.split('source "$manifest"')[0]

    keys = set()
    for env in HERE.glob("*.env"):
        for line in env.read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                name = line.split("=", 1)[0].strip()
                if name.isupper() and name.replace("_", "").isalnum():
                    keys.add(name)

    assert keys, "no manifest keys found — the glob or the parse is wrong"
    missing = sorted(k for k in keys if k not in reset)
    assert not missing, (
        "these manifest keys are never cleared before the next manifest is sourced, so a "
        "scenario that omits them silently inherits the previous scenario's values: "
        + ", ".join(missing))
