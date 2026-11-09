#!/usr/bin/env python3
# rete#440 R2, fourth review of #470: a raise inside the held loop must not end the stop.
# HeldRaiseNode (fixtures/) is a debug image that stops itself, so the stop is served by
# Debugger>>readLine -> PortaBridge>>heldStep. Its socket's first send raises inside
# startExchange; the next PULL is answered with a dbg/req `dbg:continue` row, as porta
# would deliver it. Unhandled, the raise escapes the debugger's hook and the VM ABORTS
# (measured 2026-11-08: `Error: send failed`, rc -6), killing the stopped program. Handled,
# the stop reports `dbg:error held …`, reads the continue through the link, and finishes.
# stdin is /dev/null: the cable answers EOF, so only the link can continue the stop.
import os, subprocess, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import run_tests_oevm as R

NODE = TOOLS / "fixtures" / "HeldRaiseNode.nsl"
OIMG = "/tmp/held_raise_node.oimg"


def run_node():
    ok, err = R._build_pathb_once()
    assert ok, err
    comp = subprocess.run([sys.executable, str(TOOLS / "ns_compile_oevm.py"), str(NODE), OIMG,
                           "--format=oimg", "--layout=/tmp/oevm_layout.json"],
                          capture_output=True, text=True)
    assert comp.returncode == 0, comp.stderr or comp.stdout
    p = subprocess.Popen(["/tmp/oevm_img", OIMG], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, env=dict(os.environ, OEVM_DEBUG="1"))
    try:
        out, _ = p.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill(); out, _ = p.communicate()
        out += "\n[TIMEOUT]"
    return p.returncode, out


def test_a_raise_in_the_held_loop_keeps_the_stop_served():
    rc, out = run_node()
    assert "HELD RAISE READY" in out, out
    # The raise is reported, by name, where a person debugging can see it.
    assert "dbg:error held send failed" in out, out
    # The stop survived the raise: the link's continue was read, and the program ran on.
    assert "after the stop" in out, out
    assert "done" in out, out
    assert rc == 0, (rc, out)


if __name__ == "__main__":
    rc, out = run_node()
    print(out); print("rc", rc)
