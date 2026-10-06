#!/usr/bin/env python3
"""custos's profile page against a real VM, driven as an agent drives it (rete#364).

custos serves /profile over --profile-host, which starts the host VM afresh for each
capture, without its debugger, so the console feeds the kernel's Profiler as a
standard image's cable does. POST /profile/capture answers the window ranked by
method name; the gate asserts the pause target's own main:args: is among the rows
and that no row is unknown, which a wrong map, offset or window fails. (The window
opens at boot and closes before the first 200 ms tick, so countTo: is not in it.)
A window longer than the program fails at once (502, "program ended before the
window closed"), not after a timeout.

Needs Go, and exits 77 without it, which the suite reports as a named SKIP (as
test_custos_debug.py does). Exits non-zero on failure.
"""
import json, os, shutil, socket, subprocess, sys, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_custos_debug import CUSTOS, OEVM, OIMG, VM, build  # noqa: E402


def run_http():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    base = f"http://127.0.0.1:{port}"
    env = {**os.environ, "OEVM_TIMER_WALLCLOCK": "1"}
    proc = subprocess.Popen([CUSTOS, "--bind", "127.0.0.1", "--port", str(port),
                             "--profile-host", f"{VM} {OIMG}",
                             "--profile-map", OIMG + ".map.json"],
                            cwd=OEVM, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)

    def capture(ms):
        req = urllib.request.Request(f"{base}/profile/capture?ms={ms}", data=b"", method="POST")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, json.load(r)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise AssertionError(f"custos exited: {proc.stderr.read()}")
            try:
                urllib.request.urlopen(base + "/profile", timeout=5).close()
                break
            except urllib.error.URLError:
                time.sleep(0.1)
        # The pause target ticks three times on 200 ms: a 200 ms window closes inside it.
        code, c = capture(200)
        assert code == 200, (code, c)
        names = [r["Name"] for r in c["Report"]["Rows"]]
        assert "DebugPauseTarget>>main:args:" in names, names
        assert not any(r["Unknown"] for r in c["Report"]["Rows"]), c["Report"]["Rows"]
        # A second capture starts a second VM: the page is not a one-shot.
        code, c2 = capture(200)
        assert code == 200, (code, c2)
        # A window longer than the program answers at once, naming why.
        t0 = time.monotonic()
        code, body = capture(5000)
        took = time.monotonic() - t0
        assert code == 502 and "program ended before the window closed" in body, (code, body)
        assert took < 5, f"took {took:.1f}s: it waited out the window"
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    return (f"http: 200 ms window ranked {len(names)} methods incl. main:args:, a second capture "
            f"200, a 5000 ms window 502 in {took:.1f}s")


def main():
    if shutil.which("go") is None:
        print("custos-profile: SKIP (go is not on PATH)")
        sys.exit(77)
    build()
    http = run_http()
    print("test_custos_profile: PASSED (Path B, /profile over --profile-host)")
    print(http)


if __name__ == "__main__":
    main()
