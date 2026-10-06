#!/usr/bin/env python3
"""`custos debug` against a real VM, as a person would drive it (rete#339).

The front-end case of rete#311, moved here from test_debugger_integration.py when
dbg.py was rewritten in Go as `custos debug`: a breakpoint by LINE lands at that line
and shows it, a pause of the running program is answered as a stop, a line inside a
block is refused by name rather than mapped to its home method, and the program
finishes. Every `--` line is custos's translation of a dbg:* line, so a wrong id,
offset or line fails here. The translations themselves are unit-tested in Go
(custos/internal/debug).

The same session is then driven as an agent drives it (rete#362): custos serving
/debug over --debug-host, every command's {OK, Error, State} checked, and the stop
awaited by long-polling on Seq.

Needs Go, and exits 77 without it, which the suite reports as a named SKIP: making the
nsl suite hard-fail without a Go toolchain would couple the node half to the tools half
at the gate (run_tests_oevm.run_custos_gate). Exits non-zero on failure.
"""
import os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NSROOT = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(NSROOT))
# Honor OEVM_DIR (as run_tests_oevm.py does) so branch worktrees gate against their
# OWN VM.
OEVM = os.environ.get("OEVM_DIR") or os.path.join(NSROOT, "..", "oevm")
PAUSE_APP = os.path.join(NSROOT, "..", "tests", "04_somns_basic", "47_debug_pause_target.nsl")
OIMG = "/tmp/custos_debug_pause.oimg"
VM = "/tmp/oevm_img"
CUSTOS = "/tmp/custos_debug_gate"


def build():
    sys.path.insert(0, HERE)
    import run_tests_oevm as R  # noqa: PLC0415
    ok, err = R._build_pathb_once()
    if not ok:
        raise RuntimeError(f"Path B VM build failed: {err}")
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), PAUSE_APP, OIMG,
                    "--format=oimg", "--layout=/tmp/oevm_layout.json"],
                   cwd=NSROOT, check=True)
    subprocess.run(["go", "build", "-C", os.path.join(ROOT, "custos"), "-o", CUSTOS, "./cmd/custos"],
                   check=True)


def run():
    import pexpect
    src = os.path.basename(PAUSE_APP)
    env = {**os.environ, "OEVM_TIMER_WALLCLOCK": "1"}
    child = pexpect.spawn(CUSTOS, ["debug", "-map", OIMG + ".map.json", "-source", PAUSE_APP,
                                   "--", VM, OIMG],
                          cwd=OEVM, env=env, encoding="utf-8", timeout=15)
    transcript = []

    def see(pattern):
        child.expect(pattern)
        transcript.append(child.after.strip())

    try:
        see(r"-- stopped at boot")
        # The pause first, while nothing has held the program: a hold lets the 200 ms
        # timer queue ticks, and a queued tick prints before any pause can land.
        child.sendline("continue")
        see(r"tick 1")
        child.sendline("pause")
        which = child.expect([r"-- stopped \(pause\)[^\r\n]*", r"tick 2"])
        assert which == 0, "custos debug's pause did not stop the running program before tick 2"
        transcript.append(child.after.strip())
        # rete#323: every frame named, the kernel's too, none unknown.
        child.sendline("bt")
        see(r"#0 [^\r\n]*")
        assert "wrong build" not in child.after and "no blob" not in child.after, child.after
        see(r"#1 [^\r\n]*")
        assert "wrong build" not in child.after and "no blob" not in child.after, child.after
        child.sendline("break 37")
        see(r"-- break %s:37  DebugPauseTarget>>countTo:" % re.escape(src))
        see(r"dbg:ok break")
        child.sendline("continue")
        see(r"-- stopped \(break\) at %s:37  DebugPauseTarget>>countTo:" % re.escape(src))
        see(r"37 \|     1 to: n do:")
        child.sendline("locals")
        see(r"-- frame 0 at %s:37  DebugPauseTarget>>countTo:" % re.escape(src))
        see(r"  n = \d+")
        child.sendline("clear 37")
        see(r"dbg:ok clear")
        # rete#323: a block is its own method with its own blob offset, so a line in
        # it is a breakpoint like any other.
        child.sendline("break 26")
        see(r"-- break %s:26  DebugPauseTarget_block2>>value:" % re.escape(src))
        see(r"dbg:ok break")
        child.sendline("clear 26")
        see(r"dbg:ok clear")
        child.sendline("continue")
        see(r"done")
        see(r"-- the program ended")
        child.expect(pexpect.EOF)
    finally:
        try:
            child.close()
        except Exception:
            pass
    return "\n".join(transcript)


def run_http():
    """rete#362: the agent path. custos serves the session over --debug-host; every
    answer is read as data ({OK, Error, State}), and a stop is waited for by
    long-polling on Seq, never by sleeping."""
    import json, socket, time, urllib.error, urllib.request
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    base = f"http://127.0.0.1:{port}"
    src = os.path.basename(PAUSE_APP)
    env = {**os.environ, "OEVM_TIMER_WALLCLOCK": "1"}
    proc = subprocess.Popen([CUSTOS, "--bind", "127.0.0.1", "--port", str(port),
                             "--debug-host", f"{VM} {OIMG}",
                             "--debug-map", OIMG + ".map.json", "--debug-source", PAUSE_APP],
                            cwd=OEVM, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)

    def get(path):
        with urllib.request.urlopen(base + path, timeout=30) as r:
            return json.load(r)

    def do(cmd):
        req = urllib.request.Request(base + "/debug/do", data=cmd.encode(), method="POST")
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)

    def until(what, ok, seq=-1):
        # The long-poll answers on the next change, or after 25 s with nothing new.
        deadline = time.monotonic() + 30
        while True:
            st = get(f"/debug/state?after={seq}")
            if ok(st):
                return st
            if time.monotonic() > deadline:
                raise AssertionError(f"never {what}; last Run={st['Run']} Stop={st['Stop']}")
            seq = st["Seq"]

    try:
        # custos's listener is up when it answers; this is start-up, not a stop.
        for _ in range(100):
            if proc.poll() is not None:
                raise AssertionError(f"custos exited: {proc.stderr.read()}")
            try:
                get("/debug/state")
                break
            except urllib.error.URLError:
                time.sleep(0.1)
        until("at boot", lambda st: st["Run"] == "boot")
        d = do("break 37")
        assert d["OK"] and d["Error"] == "", d
        assert f"{src}:37" in d["State"]["Breaks"], d["State"]["Breaks"]
        d = do("break 1")  # the file's header comment: outside every method
        assert not d["OK"] and d["Error"], d
        d = do("continue")
        assert d["OK"], d
        st = until("stopped at 37 with its locals",
                   lambda st: st["Run"] == "stopped" and st["Stop"]["Line"] == 37
                   and st["Locals"] is not None, d["State"]["Seq"])
        names = [v["Name"] for v in st["Locals"]["Vars"]]
        assert "n" in names, names
        assert do("clear 37")["OK"]
        d = do("continue")
        until("ended", lambda st: st["Run"] == "ended", d["State"]["Seq"])
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    return f"http: break 37 OK, break 1 refused, stopped at 37 with {names}, ended"


def main():
    if shutil.which("go") is None:
        print("custos-debug: SKIP (go is not on PATH)")
        sys.exit(77)
    build()
    transcript = run()
    http = run_http()
    print("test_custos_debug: PASSED (Path B, custos debug by source line, and /debug over --debug-host)")
    print(transcript)
    print(http)


if __name__ == "__main__":
    main()
