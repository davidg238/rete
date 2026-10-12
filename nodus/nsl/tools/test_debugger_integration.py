#!/usr/bin/env python3
"""In-image debugger integration harness (Path B: an .oimg on the image VM).

Drives 46_debug_target.nsl end-to-end with OEVM_DEBUG=1 and asserts the full
debug flow — `dbg:break`, `dbg:continue`, `dbg:inspect`, `dbg:backtrace`,
`dbg:step`, `dbg:over`, `dbg:out`, final app output.

A method is its BLOB OFFSET (rete#323): the harness reads countTo:'s and
main:args:'s offsets from the image map the compiler writes beside the .oimg
(protocol/IMAGE-ARTIFACTS.md §3), exactly as custos does. Path A (a solidified
image) has no blob, so the debugger needs a .oimg — the Path A half of this
harness was deleted with the registry (a regression, named in the #323 design).

The build is delegated to run_tests_oevm._build_pathb_once() — the same step
used by the conformance suite — so the harness and the suite share layout.json
and oevm_img.

  dbg:pause (rete#311): 47_debug_pause_target.nsl ticks on a 200 ms wall-clock timer;
          pexpect waits for `tick 1` -- the program is RUNNING -- then sends a line.
          The line must stop it before `tick 2`, the loop must answer, and `continue`
          must let it finish.

  The person's front end, `custos debug`, drives the same pause target by source
          line in test_custos_debug.py (its own aux gate, because it needs Go).

Exits non-zero on failure.
"""
import json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NSROOT = os.path.dirname(HERE)
# Honor OEVM_DIR (as run_tests_oevm.py does) so branch worktrees gate against
# their OWN VM — compiler and VM must come from the same branch or lowering
# changes run new bytecode on an old VM ("unknown opcode").
OEVM = os.environ.get("OEVM_DIR") or os.path.join(NSROOT, "..", "oevm")
APP = os.path.join(NSROOT, "..", "tests", "04_somns_basic", "46_debug_target.nsl")
PAUSE_APP = os.path.join(NSROOT, "..", "tests", "04_somns_basic", "47_debug_pause_target.nsl")
PAUSE_OIMG_B = "/tmp/47_debug_pause_target.oimg"
BINARY_B = "/tmp/oevm_img"
OIMG_B = "/tmp/46_debug_target.oimg"


def build_b():
    """Build layout.json + oevm_img (once), then compile the target to .oimg."""
    sys.path.insert(0, HERE)
    import run_tests_oevm as R  # noqa: PLC0415
    ok, err = R._build_pathb_once()
    if not ok:
        raise RuntimeError(f"Path B VM build failed: {err}")
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), APP, OIMG_B,
                    "--format=oimg", "--layout=/tmp/oevm_layout.json"],
                   cwd=NSROOT, check=True)


def image_map():
    with open(OIMG_B + ".map.json") as f:
        return json.load(f)


def pc_after_first_send(name, argc):
    """The instruction index just past the first `argc`-argument send in method `name`,
    decoded from the image: the peephole pass (2026-10-10) moves instructions, so a
    debugger expectation about "just past the countTo: send" is derived, not pinned."""
    m = image_map()
    lay = m["image"]["layout"]
    cm = lay["structs"]["CompiledMethod"]["fields"]
    oopsz = lay["oop_size"]
    blob = open(OIMG_B, "rb").read()
    import struct
    off = blob_offset(name)
    fmt = "<Q" if oopsz == 8 else "<I"
    ins = struct.unpack_from(fmt, blob, off + cm["instructions"]["offset"])[0] >> 2
    n = struct.unpack_from("<I", blob, off + cm["instruction_count"]["offset"])[0]
    for i in range(n):
        w = struct.unpack_from("<I", blob, ins + 4 * i)[0]
        op, d = w & 0xFF, (w >> 8) & 0xFF
        if op in (2, 13, 15, 0x21, 0x22) and d == argc:   # OP_SEND, _IMPLICIT, _SUPER, _SELF, _PRIVATE
            return i + 1
    raise AssertionError(f"no {argc}-arg send in {name}")


def blob_offset(name):
    """The blob offset of method `name` in the target's image map."""
    hits = [int(k) for k, m in image_map()["methods"].items() if m["name"] == name]
    assert len(hits) == 1, f"{name!r} names {len(hits)} methods in the map"
    return hits[0]


def kept_block_at(line):
    """(blob offset, first pc) of the app's kept block whose lines include `line`."""
    for k, m in image_map()["methods"].items():
        if m["file"] == 0 and "_block" in m["name"]:
            pcs = [o for o, l in m["lines"] if l == line]
            if pcs:
                return int(k), min(pcs)
    raise AssertionError(f"no kept block on line {line}")


def session(script):
    env = {**os.environ, "OEVM_DEBUG": "1"}
    proc = subprocess.run([BINARY_B, OIMG_B], input=script,
                          capture_output=True, text=True, timeout=15, env=env)
    return proc.stdout


def finished(out):
    return "\n15\n25" in out


def after_break(out, cid, label):
    paused = f"dbg:paused break {cid} 0"
    assert paused in out, f"{label} breakpoint did not fire:\n{out}"
    return out.split(paused, 1)[1]


def step_stop(text, label, out):
    m = re.search(r"dbg:paused step (-?\d+) (\d+)", text)
    assert m, f"{label} no dbg:paused step line:\n{out}"
    return int(m.group(1)), int(m.group(2))


def run_flow(cid, main_off):
    """break / inspect / backtrace / step / continue."""
    out = session(f"dbg:break {cid} 0\ndbg:continue\ndbg:inspect\ndbg:backtrace\n"
                  "dbg:step\ndbg:continue\n")
    assert "dbg:ready" in out, f"missing dbg:ready:\n{out}"
    assert "dbg:ok break" in out, f"breakpoint not set:\n{out}"
    rest = after_break(out, cid, "flow")
    # dbg:inspect: countTo: was called with n=5 -> r1=5
    stack = [ln for ln in rest.splitlines() if ln.startswith("dbg:stack off=0")]
    assert stack and "r1=5" in stack[0], f"inspect missing known local r1=5:\n{out}"
    # dbg:backtrace: innermost countTo:@0, then main:args: at its call site.
    frames = [ln for ln in rest.splitlines() if ln.startswith("dbg:frames ")]
    assert frames, f"no dbg:frames line:\n{out}"
    pairs = frames[0].split()[1:]
    assert pairs[0] == f"{cid}@0", f"innermost frame is {pairs[0]}, want {cid}@0:\n{out}"
    assert pairs[1].startswith(f"{main_off}@"), f"caller frame is {pairs[1]}, want main:args: {main_off}:\n{out}"
    assert all(not p.startswith("-1@") for p in pairs), f"a frame with no offset:\n{out}"
    # dbg:step must land in the SAME app method at a small advanced offset, NOT in
    # the debugger's own re-entrant frames.
    step_off, step_pc = step_stop(rest, "step", out)
    assert step_off == cid, (
        f"step landed in method {step_off}, expected countTo: {cid} "
        f"(stepping the debugger's own frames?):\n{out}")
    assert 0 < step_pc < 20, f"step pc {step_pc} is not inside countTo: past 0:\n{out}"
    assert finished(out), f"app did not finish after stepped continue:\n{out}"
    return out


def run_over_out(cid, main_off):
    """dbg:over stays in countTo:; dbg:out lands in main:args: past the call."""
    out = session(f"dbg:break {cid} 0\ndbg:continue\ndbg:over\ndbg:continue\n")
    o, pc = step_stop(after_break(out, cid, "over"), "over", out)
    assert o == cid and 0 < pc < 20, f"dbg:over landed at {o}@{pc}, want inside countTo: {cid}:\n{out}"
    assert finished(out), f"app did not finish after dbg:over + continue:\n{out}"

    out = session(f"dbg:break {cid} 0\ndbg:continue\ndbg:out\ndbg:continue\n")
    o, pc = step_stop(after_break(out, cid, "out"), "out", out)
    assert o == main_off, f"dbg:out landed in {o}, expected main:args: {main_off}:\n{out}"
    want = pc_after_first_send("DebugTargetTest>>main:args:", 1)
    assert pc == want, f"dbg:out pc should be {want} (just past the countTo: send), got {pc}:\n{out}"
    assert finished(out), f"app did not finish after dbg:out + continue:\n{out}"


def run_block_stop():
    """rete#323: a breakpoint inside a kept block; the backtrace runs through the
    block into scaled:, and scaled:'s frame names its captured locals by slot."""
    src = open(APP).read().splitlines()
    line = next(i + 1 for i, t in enumerate(src) if "acc:: acc + (x * k)" in t)
    blk, pc = kept_block_at(line)
    scaled = blob_offset("DebugTargetTest>>scaled:")
    out = session(f"dbg:break {blk} {pc}\ndbg:continue\ndbg:backtrace\ndbg:inspect 1\n"
                  f"dbg:clear {blk} {pc}\ndbg:continue\n")
    rest = out.split(f"dbg:paused break {blk} {pc}", 1)
    assert len(rest) == 2, f"the block breakpoint did not fire:\n{out}"
    rest = rest[1]
    frames = next(ln for ln in rest.splitlines() if ln.startswith("dbg:frames ")).split()[1:]
    assert len(frames) >= 3, f"backtrace too short: {frames}\n{out}"
    assert frames[0].startswith(f"{blk}@"), f"frame 0 is {frames[0]}, want the block {blk}:\n{out}"
    assert frames[1].startswith(f"{scaled}@"), f"frame 1 is {frames[1]}, want scaled: {scaled}:\n{out}"
    stack = next(ln for ln in rest.splitlines() if ln.startswith("dbg:stack "))
    slots = {v["name"]: v["ctx"] for m in image_map()["methods"].values()
             if m["name"] == "DebugTargetTest>>scaled:" for v in m["vars"] if "ctx" in v}
    assert set(slots) == {"acc", "k"}, f"scaled:'s captured vars are {slots}"
    # First visit: `add value: 2` before any add, so acc is 0; k is 5.
    assert f" c{slots['k']}=5" in stack, f"k (slot {slots['k']}) is not 5:\n{stack}"
    assert f" c{slots['acc']}=0" in stack, f"acc (slot {slots['acc']}) is not 0:\n{stack}"
    assert "\n25\n" in out or out.rstrip().endswith("25"), f"app did not finish:\n{out}"
    return f"block {blk}@{pc}; frames {' '.join(frames[:3])}; {stack}"


def run_refusals(cid):
    """A breakpoint the VM cannot arm, and a clear of one never set."""
    out = session("dbg:break 1 0\ndbg:clear 999999 0\ndbg:methods\ndbg:continue\n")
    assert "dbg:error no-blob" in out, f"a misaligned offset was armed:\n{out}"
    assert "dbg:error no-bp" in out, f"clearing an unset bp did not refuse:\n{out}"
    assert "dbg:error unknown" in out, f"dbg:methods still answers:\n{out}"
    assert finished(out), f"app did not finish:\n{out}"


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
    build_b()
    cid = blob_offset("DebugTargetTest>>countTo:")
    main_off = blob_offset("DebugTargetTest>>main:args:")
    transcript = run_flow(cid, main_off)
    run_over_out(cid, main_off)
    run_refusals(cid)
    block = run_block_stop()
    print("test_debugger_integration: PASSED (break, inspect, backtrace, step, over, out, refusals, block stop)")
    print("--- block stop ---")
    print(block)
    print("--- transcript ---")
    print(transcript)

    print("\n=== dbg:pause ===")
    transcript_p = run_path_b_pause()
    print("test_debugger_integration: PASSED (dbg:pause)")
    print("--- pause transcript ---")
    print(transcript_p)


if __name__ == "__main__":
    main()
