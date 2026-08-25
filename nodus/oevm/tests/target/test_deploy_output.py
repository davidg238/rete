"""rete#236 — the build's diagnostics must survive a SUCCESSFUL build.

`deploy::flash` ran the build inside `out=$( … 2>&1 )` and echoed `$out` only when
the build FAILED, then grepped it for the `[archive]` build id and dropped the rest.
So a build that succeeded discarded every warning it emitted.

That is not hypothetical. `zephyr/run.sh` prints

    [thread] radio layer not requested — dataset overlay NOT applied.

for an esp32c6 built with no OpenThread, and its own rete#165 comment records the
failure it was written for: "shipped an image with no radio: PortaBridgeNodeC6
failed its UDP bind at boot and sat dead for over an hour". On 2026-08-22 it fired
again for `liveness-heartbeat`, was discarded because the build succeeded, and the
node sat silent for 300 s while the harness reported only `TIMEOUT … never reached
its done line` — true, and naming nothing. One board cycle.

The module's own header already states the contract these tests pin:

    deploy::flash   -> prints the build id, and NOTHING else, on stdout
    Everything diagnostic goes to stderr, because the caller captures stdout.

`2>&1` was the line that broke it: `[archive]` goes to stdout (archive.sh:48) and
the diagnostics go to stderr, so merging them put the diagnostics somewhere only a
failure would print.

These drive the REAL bash function, with a stub `run.sh` standing in for the build.
A Python restatement would pass while the shell the harness actually runs was broken.
"""
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIB = HERE / "lib" / "deploy_flash.sh"

BUILD_ID = "porta-v1-final-1070-gdeadbee-20260822T120000"
WARNING = "[thread] ! NO dataset overlay — this image will NOT join the porta mesh."


def run_flash(tmp, run_sh_body):
    """Drive deploy::flash against a stub zephyr/run.sh with the given body.

    Everything deploy::flash reaches for outside itself is stubbed: the west venv,
    the SDK directory, the collector pause/resume, the board reset and the by-id
    lookup. What is NOT stubbed is deploy::flash itself.
    """
    tmp = Path(tmp)
    zdir = tmp / "zephyr"
    zdir.mkdir()
    (zdir / "run.sh").write_text(run_sh_body)

    venv = tmp / "venv"
    (venv / "bin").mkdir(parents=True)
    (venv / "bin" / "activate").write_text("")   # sourced; deliberately inert
    sdk = tmp / "sdk"
    sdk.mkdir()

    script = f"""
    set -uo pipefail
    target::say()  {{ echo "  $*"; }}
    target::warn() {{ echo "  ! $*" >&2; }}
    target::die()  {{ echo "FAIL: $*" >&2; exit 1; }}
    target::board_byid()       {{ echo "/dev/null"; }}
    target::collector_pause()  {{ :; }}
    target::collector_resume() {{ :; }}
    target::reset_board()      {{ :; }}

    TARGET_WEST_VENV="{venv}"
    TARGET_ZEPHYR_SDK="{sdk}"
    TARGET_ZEPHYR_BASE="{tmp}/zephyrbase"
    NODE=TimerlessNodeC6.nsl
    BOARD_ID=esp32c6

    source "{LIB}"
    deploy::flash "{zdir}" c6spare
    """
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=120)


SUCCESS_WITH_WARNING = f"""#!/usr/bin/env bash
echo "[bake]  TimerlessNodeC6.nsl  (model B)"
echo "{WARNING}" >&2
echo "[archive] {BUILD_ID} -> /tmp/somewhere.bin"
exit 0
"""


def test_a_warning_from_a_successful_build_reaches_the_caller():
    """THE REGRESSION. A build that exits 0 having warned must not swallow the warning."""
    with tempfile.TemporaryDirectory() as d:
        r = run_flash(d, SUCCESS_WITH_WARNING)
        assert r.returncode == 0, f"stdout={r.stdout!r} stderr={r.stderr!r}"
        assert WARNING in r.stderr, (
            "the build warned and the warning was discarded because the build succeeded\n"
            f"stderr was: {r.stderr!r}"
        )


def test_the_build_id_is_still_the_only_thing_on_stdout():
    """The other half of the contract, so surfacing diagnostics cannot break the caller.

    `run.sh`'s progress chatter goes to stdout too, and the runner captures stdout to
    read the build id out of it — so stdout must carry the id and nothing else.
    """
    with tempfile.TemporaryDirectory() as d:
        r = run_flash(d, SUCCESS_WITH_WARNING)
        assert r.returncode == 0, f"stderr={r.stderr!r}"
        assert r.stdout.strip() == BUILD_ID, f"stdout was {r.stdout!r}"


def test_a_failing_build_still_reports_why():
    """The path that already worked, pinned so the fix cannot trade one for the other."""
    body = """#!/usr/bin/env bash
echo "error: ld returned 1 exit status" >&2
exit 1
"""
    with tempfile.TemporaryDirectory() as d:
        r = run_flash(d, body)
        assert r.returncode != 0
        assert "ld returned 1 exit status" in r.stderr, f"stderr={r.stderr!r}"
        assert "run.sh failed" in r.stderr


def test_a_build_that_emits_no_build_id_fails_loudly():
    """A build that succeeds but produces no anchor is not a usable deploy."""
    body = """#!/usr/bin/env bash
echo "[bake]  TimerlessNodeC6.nsl  (model B)"
exit 0
"""
    with tempfile.TemporaryDirectory() as d:
        r = run_flash(d, body)
        assert r.returncode != 0
        assert "no build id" in r.stderr, f"stderr={r.stderr!r}"
