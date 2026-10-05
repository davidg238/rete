#!/usr/bin/env python3
"""In-image debugger integration harness (Path A + Path B).

Drives 46_debug_target.nsl end-to-end with OEVM_DEBUG=1 and asserts the full
debug flow — `dbg:methods`, `dbg:break`, `dbg:continue`, `dbg:inspect`,
`dbg:step`, `dbg:over`, `dbg:out`, final app output — on BOTH execution paths:

  Path A: solidify -> gcc -> linked binary (/tmp/oevm_debugger).
          Interactive driver uses pexpect when available; falls back to piped
          Popen (two-pass for the registry lookup, then full session).

  Path B: compile to .oimg -> image VM binary (/tmp/oevm_img).
          Driven via piped stdin Popen (the image VM reads stdin fine).
          Build is delegated to run_tests_oevm._build_pathb_once() — the same
          step used by the conformance suite — so both paths share the same
          layout.json and oevm_img binary.

Shared assertion helpers (assert_flow, assert_over_out) validate the transcript
identically for both paths; only the builder and driver differ.

  dbg:pause (rete#311): 47_debug_pause_target.nsl ticks on a 200 ms wall-clock timer;
          pexpect waits for `tick 1` -- the program is RUNNING -- then sends a line.
          The line must stop it before `tick 2`, the loop must answer, and `continue`
          must let it finish. Path B only: stopping is the VM's and the backend's
          (SIGIO on stdin here, the UART ISR on a board), not the image path's.

  The person's front end, `custos debug`, drives the same pause target by source
          line in test_custos_debug.py (its own aux gate, because it needs Go).

Exits non-zero on failure.
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NSROOT = os.path.dirname(HERE)
# Honor OEVM_DIR (as run_tests_oevm.py does) so branch worktrees gate against
# their OWN VM — compiler and VM must come from the same branch or lowering
# changes run new bytecode on an old VM ("unknown opcode").
OEVM = os.environ.get("OEVM_DIR") or os.path.join(NSROOT, "..", "oevm")
APP = os.path.join(NSROOT, "..", "tests", "04_somns_basic", "46_debug_target.nsl")
PAUSE_APP = os.path.join(NSROOT, "..", "tests", "04_somns_basic", "47_debug_pause_target.nsl")
PAUSE_OIMG_B = "/tmp/47_debug_pause_target.oimg"
BINARY_A = "/tmp/oevm_debugger"
BINARY_B = "/tmp/oevm_img"
OIMG_B = "/tmp/46_debug_target.oimg"

# A selector we KNOW the target defines, so dbg:methods must list it.
TARGET_SELECTOR = "countTo:"


# ---------------------------------------------------------------------------
# Build helpers
# ---------------------------------------------------------------------------

def build_a():
    """Path A: solidify -> gcc -> linked binary."""
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), APP,
                    os.path.join(OEVM, "src", "solidified_image.c")],
                   cwd=NSROOT, check=True)
    subprocess.run(["gcc", "-Wall", "-Wextra", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE",
                    "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
                    "src/serial.c",
                    "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c",
                    "src/backend/posix/platform_posix.c",
                    "src/backend/posix/serial_posix.c", "src/backend/posix/udp_posix.c",
                    "src/backend/posix/http_posix.c",
                    "src/backend/posix/pulse_posix.c", "src/backend/posix/timer_posix.c",
                    "src/backend/posix/span_posix.c",   # tuvm#61
                    "src/backend/posix/sensor_posix.c",
                    "src/backend/posix/ota_posix.c",   # rete#25
                    "src/solidified_image.c", "src/oevm_main.c",
                    "-o", BINARY_A, "-lm"], cwd=OEVM, check=True)


def build_b():
    """Path B: build layout.json + oevm_img (once), then compile target to .oimg."""
    sys.path.insert(0, HERE)
    import run_tests_oevm as R  # noqa: PLC0415
    ok, err = R._build_pathb_once()
    if not ok:
        raise RuntimeError(f"Path B VM build failed: {err}")
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), APP, OIMG_B,
                    "--format=oimg", "--layout=/tmp/oevm_layout.json"],
                   cwd=NSROOT, check=True)


# ---------------------------------------------------------------------------
# Shared assertion helpers (one checker, two paths)
# ---------------------------------------------------------------------------

def _method_id(block, selector):
    """Return the numeric methodId of the registry line for `selector`."""
    for line in block.splitlines():
        if selector in line:
            tok = line.strip().split()[0]
            if tok.isdigit():
                return int(tok)
    raise AssertionError(f"no registry id for {selector!r}:\n{block}")


def _assert_registry(block):
    """The registry must list our known selector with a numeric id."""
    hit = None
    for line in block.splitlines():
        if TARGET_SELECTOR in line:
            hit = line.strip()
            break
    assert hit, f"selector {TARGET_SELECTOR!r} not in dbg:methods registry:\n{block}"
    first = hit.split()[0]
    assert first.isdigit(), f"registry line did not start with a numeric id: {hit!r}"


def assert_flow(out, label=""):
    """Assert the full break/inspect/step/continue flow in a captured transcript.

    `out` is the full stdout of the debug session.  Both Path A and Path B
    produce the same observable sequence; the only difference is how they were
    built and driven.
    """
    assert "dbg:ready" in out, f"{label} missing dbg:ready:\n{out}"
    assert "dbg:ok methods" in out, f"{label} missing dbg:ok methods:\n{out}"

    methods_block = out.split("dbg:ok methods", 1)[0]
    _assert_registry(methods_block)
    cid = _method_id(methods_block, TARGET_SELECTOR)

    assert "dbg:ok break" in out, f"{label} breakpoint not set:\n{out}"

    paused = f"dbg:paused break {cid} 0"
    assert paused in out, f"{label} breakpoint did not fire:\n{out}"
    after_break = out.split(paused, 1)[1]

    # dbg:inspect: countTo: was called with n=5 -> r1=5
    stack_lines = [ln for ln in after_break.splitlines() if ln.startswith("dbg:stack off=0")]
    assert stack_lines, f"{label} no dbg:stack line after pause:\n{out}"
    assert "r1=5" in stack_lines[0], f"{label} inspect missing known local r1=5:\n{stack_lines[0]}"

    # dbg:step: must land in the SAME app method (countTo: id=cid) at a small
    # advanced offset, NOT in the debugger's own re-entrant frames (which would
    # report a foreign id like -1 / large offset like 73).
    m = re.search(r"dbg:paused step (-?\d+) (\d+)", after_break)
    assert m, f"{label} no dbg:paused step line after dbg:step:\n{out}"
    step_id, step_off = int(m.group(1)), int(m.group(2))
    assert step_id == cid, (
        f"{label} step landed in foreign method id={step_id}, expected app countTo: "
        f"id={cid} (stepping the debugger's own frames?):\n{out}")
    assert step_off != 0, f"{label} step did not advance offset:\n{out}"
    assert step_off != 73, (
        f"{label} step offset 73 is the debugger onBreakContext: epilogue — stepping "
        f"leaked into the debugger's frames:\n{out}")
    assert step_off < 20, (
        f"{label} step offset {step_off} too large to be within countTo::\n{out}")

    # App must finish after stepped continue (NOT stuck single-stepping).
    assert "\n15\n" in out or out.rstrip().endswith("15"), \
        f"{label} app did not finish after stepped continue:\n{out}"
    return cid


def assert_over(out, cid, label=""):
    """Assert dbg:over flow: pauses at next instruction IN THE SAME method.

    Empirically anchored: dbg:over at countTo: off=0 fires `dbg:paused step 2 1`
    (same method id=cid, offset advances from 0 to 1) on both Path A and Path B.
    """
    paused = f"dbg:paused break {cid} 0"
    assert paused in out, f"{label} breakpoint did not fire for over test:\n{out}"
    after_break = out.split(paused, 1)[1]

    m = re.search(r"dbg:paused step (-?\d+) (\d+)", after_break)
    assert m, f"{label} no dbg:paused step line after dbg:over:\n{out}"
    over_id, over_off = int(m.group(1)), int(m.group(2))
    assert over_id == cid, (
        f"{label} dbg:over landed in foreign method id={over_id}, expected id={cid}:\n{out}")
    assert over_off != 0, f"{label} dbg:over did not advance offset:\n{out}"
    assert over_off < 20, (
        f"{label} dbg:over offset {over_off} too large — should stay within countTo::\n{out}")
    # App must still finish after continue
    assert "\n15\n" in out or out.rstrip().endswith("15"), \
        f"{label} app did not finish after dbg:over + continue:\n{out}"


def assert_out(out, cid, main_id, label=""):
    """Assert dbg:out flow: pauses in the CALLER (main:args:) after countTo: returns.

    Empirically anchored: dbg:out at countTo: off=0 fires `dbg:paused step 1 6`
    (main:args: method id=main_id=1, offset=6) on both Path A and Path B.
    The app still finishes afterward.
    """
    paused = f"dbg:paused break {cid} 0"
    assert paused in out, f"{label} breakpoint did not fire for out test:\n{out}"
    after_break = out.split(paused, 1)[1]

    m = re.search(r"dbg:paused step (-?\d+) (\d+)", after_break)
    assert m, f"{label} no dbg:paused step line after dbg:out:\n{out}"
    out_id, out_off = int(m.group(1)), int(m.group(2))
    assert out_id == main_id, (
        f"{label} dbg:out landed in method id={out_id}, expected main:args: "
        f"id={main_id} (should step out into caller):\n{out}")
    assert out_off == 6, (  # empirical: main:args: call-site offset just past countTo: send
        f"{label} dbg:out offset should be 6 (caller main:args: offset past countTo: call site), "
        f"got {out_off}:\n{out}")
    # App must still finish after continue
    assert "\n15\n" in out or out.rstrip().endswith("15"), \
        f"{label} app did not finish after dbg:out + continue:\n{out}"
    return out_off


# ---------------------------------------------------------------------------
# Path A — interactive driver + popen fallback
# ---------------------------------------------------------------------------

def _env(debug=True):
    env = dict(os.environ)
    if debug:
        env["OEVM_DEBUG"] = "1"
    else:
        env.pop("OEVM_DEBUG", None)
    return env


def drive_a_pexpect():
    """Path A interactive driver using pexpect (preferred)."""
    import pexpect
    child = pexpect.spawn(BINARY_A, cwd=OEVM, env=_env(), encoding="utf-8", timeout=15)
    transcript = []
    try:
        child.expect("dbg:ready")
        transcript.append("dbg:ready")

        # dbg:methods -> registry lines terminated by `dbg:ok methods`.
        child.sendline("dbg:methods")
        child.expect("dbg:ok methods")
        methods_block = child.before
        transcript.append(methods_block)
        _assert_registry(methods_block)

        cid = _method_id(methods_block, TARGET_SELECTOR)
        transcript.append(f"countTo: id={cid}")
        child.sendline(f"dbg:break {cid} 0")
        child.expect("dbg:ok break")
        transcript.append("dbg:ok break")

        child.sendline("dbg:continue")
        child.expect(f"dbg:paused break {cid} 0")
        transcript.append(f"dbg:paused break {cid} 0")

        child.sendline("dbg:inspect")
        child.expect(r"dbg:stack off=0 .*r1=5")
        stack_line = "dbg:stack off=0 " + child.after.split("dbg:stack off=0 ", 1)[-1]
        transcript.append(stack_line)

        child.sendline("dbg:step")
        child.expect(r"dbg:paused step (-?\d+) (\d+)")
        step_id = int(child.match.group(1))
        step_off = int(child.match.group(2))
        assert step_id == cid, (
            f"step landed in foreign method id={step_id}, expected app "
            f"countTo: id={cid} (stepping the debugger's own frames?)")
        assert step_off != 0, f"step did not advance offset (still {step_off})"
        assert step_off != 73, (
            f"step offset {step_off} is the debugger onBreakContext: epilogue "
            f"offset — stepping leaked into the debugger's frames")
        assert step_off < 20, (
            f"step offset {step_off} too large to be within countTo: — "
            f"stepping likely leaked out of the app frame")
        transcript.append(f"dbg:paused step {step_id} {step_off} (in app countTo:)")

        child.sendline("dbg:continue")
        child.expect(r"\n15\r?\n")
        transcript.append("app-output:15")
        child.expect(pexpect.EOF)
    finally:
        try:
            child.close()
        except Exception:
            pass
    return "\n".join(transcript)


def drive_a_popen():
    """Path A popen fallback: two-pass to learn registry, then full session.

    Pass 1: learn countTo: id.  Pass 2: full break/inspect/step/continue session.
    Returns the full combined stdout string so assert_flow() can check it.
    """
    p1 = subprocess.run([BINARY_A], cwd=OEVM, env=_env(), input="dbg:methods\ndbg:continue\n",
                        capture_output=True, text=True, timeout=15)
    out1 = p1.stdout
    assert "dbg:ready" in out1, f"missing dbg:ready:\n{out1}"
    assert "dbg:ok methods" in out1, f"missing dbg:ok methods:\n{out1}"
    methods_block = out1.split("dbg:ok methods", 1)[0]
    _assert_registry(methods_block)
    cid = _method_id(methods_block, TARGET_SELECTOR)

    script = (f"dbg:methods\ndbg:break {cid} 0\ndbg:continue\n"
              f"dbg:inspect\ndbg:step\ndbg:continue\n")
    proc = subprocess.run([BINARY_A], cwd=OEVM, env=_env(), input=script,
                          capture_output=True, text=True, timeout=15)
    return proc.stdout


def run_path_a():
    """Run Path A: build, drive, assert_flow.

    The popen driver produces the full stdout string that assert_flow() checks.
    If pexpect is available, also run the interactive session for additional
    live-interaction smoke coverage (it has its own inline assertions).
    Returns (transcript, mode) where transcript is the popen output.
    """
    build_a()
    # Shared-checker path: always use popen to get a complete stdout for assert_flow
    transcript = drive_a_popen()
    assert_flow(transcript, label="PathA")
    # Additional interactive smoke (best-effort, does not replace assert_flow)
    mode = "popen"
    try:
        import pexpect  # noqa: F401
        drive_a_pexpect()
        mode = "pexpect+popen"
    except ImportError:
        pass
    return transcript, mode


def run_path_a_over_out():
    """Path A over/out coverage (popen, reuses the already-built BINARY_A)."""
    # Learn cid and main_id from a quick registry-only run
    p1 = subprocess.run([BINARY_A], cwd=OEVM, env=_env(), input="dbg:methods\ndbg:continue\n",
                        capture_output=True, text=True, timeout=15)
    out1 = p1.stdout
    assert "dbg:ok methods" in out1, f"PathA-over/out discovery run missing dbg:ok methods:\n{out1}"
    methods_block = out1.split("dbg:ok methods", 1)[0]
    cid = _method_id(methods_block, TARGET_SELECTOR)
    main_id = _method_id(methods_block, "main:args:")

    # dbg:over
    over_script = f"dbg:methods\ndbg:break {cid} 0\ndbg:continue\ndbg:over\ndbg:continue\n"
    over_proc = subprocess.run([BINARY_A], cwd=OEVM, env=_env(), input=over_script,
                                capture_output=True, text=True, timeout=15)
    assert_over(over_proc.stdout, cid, label="PathA-over")

    # dbg:out
    out_script = f"dbg:methods\ndbg:break {cid} 0\ndbg:continue\ndbg:out\ndbg:continue\n"
    out_proc = subprocess.run([BINARY_A], cwd=OEVM, env=_env(), input=out_script,
                               capture_output=True, text=True, timeout=15)
    assert_out(out_proc.stdout, cid, main_id, label="PathA-out")
    return cid, main_id


# ---------------------------------------------------------------------------
# Path B — piped-stdin popen (image VM reads stdin fine)
# ---------------------------------------------------------------------------

def drive_b_popen(cid):
    """Path B: one-pass with full session (registry id already known from cid)."""
    script = (f"dbg:methods\ndbg:break {cid} 0\ndbg:continue\n"
              f"dbg:inspect\ndbg:step\ndbg:continue\n")
    env = {**os.environ, "OEVM_DEBUG": "1"}
    proc = subprocess.run([BINARY_B, OIMG_B], input=script,
                          capture_output=True, text=True, timeout=15, env=env)
    return proc.stdout


def run_path_b():
    """Run Path B: build, discover cid, drive full session, assert_flow."""
    build_b()

    # Step 1: learn the countTo: id from a fresh discovery run
    env = {**os.environ, "OEVM_DEBUG": "1"}
    p1 = subprocess.run([BINARY_B, OIMG_B], input="dbg:methods\ndbg:continue\n",
                        capture_output=True, text=True, timeout=15, env=env)
    out1 = p1.stdout
    assert "dbg:ready" in out1, f"PathB missing dbg:ready:\n{out1}"
    assert "dbg:ok methods" in out1, f"PathB missing dbg:ok methods:\n{out1}"
    methods_block = out1.split("dbg:ok methods", 1)[0]
    _assert_registry(methods_block)
    cid = _method_id(methods_block, TARGET_SELECTOR)

    # Step 2: full break/inspect/step session
    transcript = drive_b_popen(cid)
    assert_flow(transcript, label="PathB")
    return transcript, cid


def run_path_b_over_out(cid):
    """Path B over/out coverage (reuses the already-built BINARY_B/OIMG_B)."""
    env = {**os.environ, "OEVM_DEBUG": "1"}

    # Learn main_id from a fresh registry run
    p1 = subprocess.run([BINARY_B, OIMG_B], input="dbg:methods\ndbg:continue\n",
                        capture_output=True, text=True, timeout=15, env=env)
    out1 = p1.stdout
    assert "dbg:ok methods" in out1, f"PathB-over/out discovery run missing dbg:ok methods:\n{out1}"
    methods_block = out1.split("dbg:ok methods", 1)[0]
    main_id = _method_id(methods_block, "main:args:")

    # dbg:over
    over_script = f"dbg:methods\ndbg:break {cid} 0\ndbg:continue\ndbg:over\ndbg:continue\n"
    over_proc = subprocess.run([BINARY_B, OIMG_B], input=over_script,
                                capture_output=True, text=True, timeout=15, env=env)
    assert_over(over_proc.stdout, cid, label="PathB-over")

    # dbg:out
    out_script = f"dbg:methods\ndbg:break {cid} 0\ndbg:continue\ndbg:out\ndbg:continue\n"
    out_proc = subprocess.run([BINARY_B, OIMG_B], input=out_script,
                               capture_output=True, text=True, timeout=15, env=env)
    assert_out(out_proc.stdout, cid, main_id, label="PathB-out")
    return main_id


# ---------------------------------------------------------------------------
# dbg:pause — a line arriving while the program RUNS stops it (rete#311)
# ---------------------------------------------------------------------------

def run_path_b_pause():
    """Build the pause target (Path B), then drive it interactively through a pty.

    The assertions are the three halves of the rule: the stop is PROMPT (the pause
    line precedes `tick 2`, i.e. it landed inside one 200 ms period), the loop ANSWERS
    (dbg:ok pause, then a dbg:stack for inspect), and `dbg:continue` lets the program
    FINISH (tick 3, done, EOF). A piped script cannot test this: its lines all arrive
    while the VM is held at the boot stop, which is exactly the case the rule excludes.
    """
    import pexpect
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), PAUSE_APP, PAUSE_OIMG_B,
                    "--format=oimg", "--layout=/tmp/oevm_layout.json"],
                   cwd=NSROOT, check=True)
    env = {**os.environ, "OEVM_DEBUG": "1", "OEVM_TIMER_WALLCLOCK": "1"}
    child = pexpect.spawn(BINARY_B, [PAUSE_OIMG_B], cwd=OEVM, env=env, encoding="utf-8", timeout=15)
    child.setecho(False)
    transcript = []
    try:
        child.expect("dbg:ready")
        transcript.append("dbg:ready")
        child.sendline("dbg:continue")
        child.expect("tick 1")
        transcript.append("tick 1 (running)")
        child.sendline("dbg:pause")
        which = child.expect([r"dbg:paused pause (-?\d+) (\d+)", "tick 2"])
        assert which == 0, "the program ticked again before the pause landed: a line in did not stop it"
        transcript.append("dbg:paused pause %s %s" % (child.match.group(1), child.match.group(2)))
        child.expect("dbg:ok pause")
        transcript.append("dbg:ok pause")
        child.sendline("dbg:inspect")
        child.expect(r"dbg:stack off=\d+")
        transcript.append(child.after)
        # rete#312 Q1: in a debug image the cable belongs to this loop, which hands
        # `profile <ms>` to the kernel's one Profiler -- the line porta would write.
        child.sendline("profile 50")
        child.expect("dbg:ok profile")
        child.sendline("profile nope")
        child.expect("dbg:error profile")
        transcript.append("dbg:ok profile / dbg:error profile")
        child.sendline("dbg:continue")
        child.expect("tick 3")
        child.expect("done")
        transcript.append("tick 3, done")
        child.expect(pexpect.EOF)
    finally:
        try:
            child.close()
        except Exception:
            pass
    return "\n".join(transcript)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    try:
        print("=== Path A ===")
        transcript_a, mode_a = run_path_a()
        cid_a, main_id_a = run_path_a_over_out()
        print(f"test_debugger_integration: PASSED (Path A, {mode_a})")
        print("--- Path A transcript ---")
        print(transcript_a)

        print("\n=== Path B ===")
        transcript_b, cid_b = run_path_b()
        main_id_b = run_path_b_over_out(cid_b)
        print("test_debugger_integration: PASSED (Path B, popen)")
        print("--- Path B transcript ---")
        print(transcript_b)

        print("\n=== Path B: dbg:pause ===")
        transcript_p = run_path_b_pause()
        print("test_debugger_integration: PASSED (Path B, dbg:pause)")
        print("--- pause transcript ---")
        print(transcript_p)

        print(f"\nAll assertions passed on both paths (over/out anchored: "
              f"over->step {cid_b} 1, out->step {main_id_b} 6)")
    finally:
        # Restore files overwritten by build_a() so ../oevm stays a clean tree.
        subprocess.run(
            ["git", "-C", OEVM, "checkout",
             "src/solidified_image.c"],
            check=False)


if __name__ == "__main__":
    main()
