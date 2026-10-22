#!/usr/bin/env python3
"""Stop, patch a method, restart its frame, resume — on the desktop (rete#313).

48_patch_restart_target.nsl calls report twice. The harness runs it on the image
VM with the debugger on, breaks report on its println line (after `v:: self value`
has run), and there:

  dbg:patch <text>   replaces report with 48_patch_restart_target_v2.nsl's, recompiled
                     and re-encoded against the running image by patch_method.py; the
                     text rides the line, as it does over a board's cable
  dbg:restart        restarts the stopped frame from its first instruction, in v2
  dbg:continue

Without the restart, the stopped frame would finish in the old code and print
`value 1`; without the patch, nothing would change. The pass is `patched 42`
twice: once from the restarted frame, once from the second send, which finds the
replacement through the override table. v2 needs more registers than the frame
had, so the restart also grows the frame in place.

Refusals are gated too: a v2 that uses a string the running image does not hold
is refused by name, and the VM refuses a patch line that is not the format (a
wrong version, a cut line) rather than installing part of it. Exits non-zero on failure.
"""
import json, os, queue, subprocess, sys, threading

HERE = os.path.dirname(os.path.abspath(__file__))
NSROOT = os.path.dirname(HERE)
OEVM = os.environ.get("OEVM_DIR") or os.path.join(NSROOT, "..", "oevm")
TESTS = os.path.join(NSROOT, "..", "tests", "04_somns_basic")
OLD = os.path.join(TESTS, "48_patch_restart_target.nsl")
NEW = os.path.join(TESTS, "48_patch_restart_target_v2.nsl")
OIMG = "/tmp/48_patch_restart_target.oimg"
VM = "/tmp/oevm_img"
LAYOUT = "/tmp/oevm_layout.json"


def build():
    sys.path.insert(0, HERE)
    import run_tests_oevm as R  # noqa: PLC0415
    ok, err = R._build_pathb_once()
    if not ok:
        raise RuntimeError(f"Path B VM build failed: {err}")
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), OLD, OIMG,
                    "--format=oimg", f"--layout={LAYOUT}"], cwd=NSROOT, check=True,
                   stdout=subprocess.DEVNULL)


def println_break():
    """report's blob offset, and the pc of its println line (the source's line
    containing "println" inside report)."""
    m = json.load(open(OIMG + ".map.json"))
    src = open(OLD).read().splitlines()
    for off, meth in m["methods"].items():
        if meth["name"] == "PatchRestartTarget>>report":
            for pc, line in meth["lines"]:
                if "println" in src[line - 1]:
                    return int(off), pc
    raise AssertionError("report's println line is not in the map")


class VMProc:
    def __init__(self):
        env = {**os.environ, "OEVM_DEBUG": "1"}
        self.p = subprocess.Popen([VM, OIMG], cwd=OEVM, env=env, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.q = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.q.put(line.rstrip("\n"))
        self.q.put(None)

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def expect(self, prefix, timeout=15):
        while True:
            line = self.q.get(timeout=timeout)
            if line is None:
                raise AssertionError(f"the VM ended waiting for {prefix!r}:\n" + "\n".join(self.seen))
            self.seen.append(line)
            if line.startswith(prefix):
                return line

    def finish(self, timeout=15):
        self.p.stdin.close()
        self.p.wait(timeout=timeout)
        while (line := self.q.get(timeout=timeout)) is not None:
            self.seen.append(line)
        return self.p.returncode


def patch(new_src, *flags):
    return subprocess.run([sys.executable, os.path.join(HERE, "patch_method.py"), *flags, OIMG + ".map.json",
                           OLD, new_src, "PatchRestartTarget>>report"],
                          cwd=NSROOT, capture_output=True, text=True)


def run_patch_restart():
    r = patch(NEW)
    assert r.returncode == 0, r.stdout + r.stderr
    off, pc = println_break()
    vm = VMProc()
    vm.expect("dbg:ready")
    vm.send(f"dbg:break {off} {pc}")
    vm.expect("dbg:ok break")
    vm.send("dbg:continue")
    stop = vm.expect("dbg:paused break")
    text = r.stdout.strip()
    for bad in (f"oevm-patch 2 {text.split(' ', 2)[2]}", text[:len(text) // 2]):
        vm.send(f"dbg:patch {bad}")
        got = vm.expect("dbg:")
        assert got == "dbg:error patch", (bad, got)
    vm.send(f"dbg:patch {text}")
    vm.expect("dbg:ok patch")
    vm.send("dbg:restart")
    vm.expect("dbg:ok restart")
    vm.send(f"dbg:clear {off} {pc}")
    vm.expect("dbg:ok clear")
    vm.send("dbg:continue")
    code = vm.finish()
    out = [l for l in vm.seen if l.startswith(("value ", "patched "))]
    assert code == 0, (code, vm.seen)
    assert out == ["patched 42", "patched 42"], vm.seen
    return f"stopped ({stop}), two bad lines refused, patched, restarted, resumed: {out}"


def run_refusal():
    """A v2 whose report uses a string the running image does not hold."""
    bad = "/tmp/48_patch_restart_target_v3.nsl"
    with open(NEW) as f:
        text = f.read().replace("('patched ' ,", "('never seen ' ,")
    with open(bad, "w") as f:
        f.write(text)
    r = patch(bad)
    assert r.returncode == 1 and "holds no" in r.stdout and "never seen" in r.stdout, r.stdout + r.stderr
    return "a new string refused: " + r.stdout.strip().splitlines()[-1]


def run_line_cap():
    """rete#432 P8: a patch over the cable's 511-byte line is refused for the cable
    and written for a blob (a patch set through porta), where no line buffer reads it."""
    big = "/tmp/48_patch_restart_target_v4.nsl"
    with open(NEW) as f:
        text = f.read().replace("v:: self value.", "v:: self value" + " + 1" * 120 + ".")
    with open(big, "w") as f:
        f.write(text)
    r = patch(big)
    assert r.returncode == 1 and "a debug line holds" in r.stdout, r.stdout + r.stderr
    b = patch(big, "--blob")
    assert b.returncode == 0 and len("dbg:patch " + b.stdout.strip()) > 511, b.stdout + b.stderr
    return f"a {len(b.stdout.strip())} B patch: refused for the cable, written with --blob"


def main():
    build()
    a = run_patch_restart()
    b = run_refusal()
    c = run_line_cap()
    print("test_patch_restart: PASSED (Path B, desktop)")
    print(a)
    print(b)
    print(c)


if __name__ == "__main__":
    main()
