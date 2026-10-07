#!/usr/bin/env python3
"""custos's patch verb against a real VM, driven as an agent drives it (rete#313, peer
design D17).

48_patch_restart_target.nsl calls report twice. On a --host board, over HTTP only:
break report on its println line, continue to the stop, POST /debug/patch with the
whole edited source (48_patch_restart_target_v2.nsl, which prints `patched 42`),
`restart`, clear, continue. custos compiles the one method against the running image
(the map carries its layout) and the VM installs it; the pass is `patched 42` twice,
once from the restarted frame and once from the second send. A source that needs a
constant the image does not hold is refused by name, and the VM is not touched.

Needs Go, and exits 77 without it, which the suite reports as a named SKIP (as
test_custos_debug.py does). Exits non-zero on failure.
"""
import json, os, shutil, subprocess, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_custos_debug import NSROOT, VM, build, serve  # noqa: E402

TESTS = os.path.join(NSROOT, "..", "tests", "04_somns_basic")
OLD = os.path.join(TESTS, "48_patch_restart_target.nsl")
NEW = os.path.join(TESTS, "48_patch_restart_target_v2.nsl")
OIMG = "/tmp/custos_patch_target.oimg"
METHOD = "PatchRestartTarget>>report"


def println_line():
    for n, line in enumerate(open(OLD), 1):
        if "('value ' , v printString) println" in line:
            return n
    raise AssertionError("no println line in " + OLD)


def do(base, cmd):
    req = urllib.request.Request(f"{base}/debug/do", data=cmd.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def patch(base, source):
    req = urllib.request.Request(f"{base}/debug/patch?method={METHOD}", data=source.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)


def state(base):
    with urllib.request.urlopen(f"{base}/debug/state", timeout=60) as r:
        return json.load(r)


def until(base, pred, what, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        st = state(base)
        if pred(st):
            return st
        time.sleep(0.2)
    raise AssertionError(f"waited {timeout}s for {what}: {json.dumps(state(base))[:800]}")


def run_http():
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), OLD, OIMG,
                    "--format=oimg", "--layout=/tmp/oevm_layout.json"], cwd=NSROOT, check=True,
                   capture_output=True)
    line = println_line()
    args = ["--host", f"dev={VM}", "--map", f"dev={OIMG}.map.json", "--source", f"dev={OLD}"]
    with serve(args) as root:
        base = f"{root}/board/dev"
        d = do(base, f"break {line}")
        assert d["OK"], d
        assert do(base, "continue")["OK"]
        st = until(base, lambda s: s["Run"] == "stopped", "the break")
        assert st["Stop"]["Line"] == line and st["Stop"]["Method"] == METHOD, st["Stop"]
        # A refusal is the compiler's, by name, and leaves the VM as it was.
        bad = open(NEW).read().replace("('patched ' ,", "('never seen ' ,")
        refused = patch(base, bad)
        assert not refused["OK"] and "holds no" in refused["Error"] and "never seen" in refused["Error"], refused
        assert refused["State"]["Run"] == "stopped", refused["State"]
        r = patch(base, open(NEW).read())
        assert r["OK"], r
        assert do(base, "restart")["OK"]
        assert do(base, f"clear {line}")["OK"]
        assert do(base, "continue")["OK"]
        st = until(base, lambda s: s["Run"] == "ended", "the program's end")
        out = [l for l in st["Transcript"] if l.startswith(("value ", "patched "))]
        assert out == ["patched 42", "patched 42"], st["Transcript"]
        why = refused["Error"].splitlines()[-1]
        return (f"http: stopped at line {line}, a new string refused ({why}), "
                f"patched, restarted, resumed: {out}")


def main():
    if shutil.which("go") is None:
        print("custos-patch: SKIP (go is not on PATH)")
        sys.exit(77)
    build()
    http = run_http()
    print("test_custos_patch: PASSED (Path B, /debug/patch on a --host board)")
    print(http)


if __name__ == "__main__":
    main()
