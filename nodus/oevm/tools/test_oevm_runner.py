# Copyright (c) 2026 Ekorau LLC
"""Tests for oevm_runner.py and ed25519_verify.py (rete#25).

Runnable two ways, on purpose:

    python3 tools/test_oevm_runner.py     # standalone, no pytest needed
    python3 -m pytest                     # picked up by the nsl suite's
                                          # `mprof-tools-py` gate, which runs
                                          # pytest over nodus/oevm/tools

The state-machine tests drive the runner as a SUBPROCESS with a **fake VM** — a
shell script that does what a VM would do at each boot (stage an image and exit
42, touch the confirm marker, crash, or hang past the watchdog). Driving the
real exec loop in-process would mean faking `Popen`, which is exactly the seam
under test; a subprocess costs a second and tests the thing.

The fake VMs branch on a boot counter, so one script describes a whole scenario
("boot 1 stages, boot 2 crashes, boot 3 idles") in the order the runner will
walk it.

Signing: the state-machine tests sign with a small ed25519 signer written HERE,
not in `ed25519_verify.py` — that file stays verify-only because the runner has
no business holding a key. A test-local signer could in principle agree with a
broken verifier, which is why `test_verify_cross_impl` signs with the real Go
porta binary instead: Go-sign / Python-verify is the property that matters.
"""

import atexit
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519_verify  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(HERE, "oevm_runner.py")
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
PORTA_DIR = os.path.join(REPO_ROOT, "porta")

IMAGE_A = b"IMAGE-A-CONTENT"
IMAGE_B = b"IMAGE-B-CONTENT"
SEED = bytes(range(32))          # any 32 bytes; the keypair is derived from it


# --------------------------------------------------------------------------
# a test-local ed25519 signer (see the module docstring for why it lives here)
# --------------------------------------------------------------------------
def keypair(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= (1 << 254)
    A = ed25519_verify._mul(a, ed25519_verify._B)
    return a, h[32:], compress(A)


def compress(P):
    zinv = pow(P[2], ed25519_verify.p - 2, ed25519_verify.p)
    x = P[0] * zinv % ed25519_verify.p
    y = P[1] * zinv % ed25519_verify.p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def sign(seed, msg):
    a, prefix, pub = keypair(seed)
    q = ed25519_verify.q
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), "little") % q
    R = compress(ed25519_verify._mul(r, ed25519_verify._B))
    k = ed25519_verify._sha512_modq(R + pub + msg)
    s = (r + k * a) % q
    return R + int.to_bytes(s, 32, "little")


def flip_a_byte(b):
    out = bytearray(b)
    out[0] ^= 0x01
    return bytes(out)


def flip_sig_scalar(sig):
    """Flip a byte in a signature's S half.

    Flipping byte 0 instead would corrupt the encoded point R, which usually
    fails to decompress — so the signature is rejected without the verification
    equation ever being evaluated. (Found by sabotaging `verify` to `return
    True`: the naive flip still "detected" it.) Byte 32 is the low byte of the
    scalar S: it always decodes, it stays below the group order, and rejecting
    it requires actually checking `[s]B == R + [h]A`."""
    out = bytearray(sig)
    out[32] ^= 0x01
    return bytes(out)


# --------------------------------------------------------------------------
# node-dir provisioning + the fake VM
# --------------------------------------------------------------------------
def sha_hex(data):
    return hashlib.sha256(data).hexdigest()


def provision(root):
    """The gate's provisioning contract: slot a, its manifest entry, pub.key."""
    os.makedirs(os.path.join(root, "slots"), exist_ok=True)
    os.makedirs(os.path.join(root, "stage"), exist_ok=True)
    _, _, pub = keypair(SEED)
    with open(os.path.join(root, "pub.key"), "w") as f:
        f.write(pub.hex() + "\n")
    with open(os.path.join(root, "slots", "a.oimg"), "wb") as f:
        f.write(IMAGE_A)
    ha = sha_hex(IMAGE_A)
    sa = sign(SEED, bytes.fromhex(ha)).hex()
    manifest = {"active": "a", "prev": None, "trial": False,
                "slots": {"a": {"hash": ha, "sig": sa}, "b": None}}
    with open(os.path.join(root, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    return manifest


def write_fake_vm(root, boots):
    """boots: {boot_number: shell body}. Anything past the last listed boot
    idles, so a scenario never falls off the end into a busy re-exec loop."""
    path = os.path.join(root, "fakevm.sh")
    cases = "\n".join("  %d) %s ;;" % (n, body) for n, body in sorted(boots.items()))
    with open(path, "w") as f:
        f.write(
            "#!/bin/sh\n"
            "CNT=%s/boots\n"
            "n=$(cat \"$CNT\" 2>/dev/null || echo 0)\n"
            "n=$((n+1))\n"
            "echo $n > \"$CNT\"\n"
            "echo \"vm: boot $n trial=${OEVM_OTA_TRIAL:-0} image=$1\"\n"
            "case \"$n\" in\n%s\n  *) sleep 60 ;;\nesac\n" % (shq(root), cases))
    os.chmod(path, 0o755)
    return path


def shq(s):
    return "'" + s.replace("'", "'\\''") + "'"


def stage_and_exit_42(image, hash_hex, sig_hex):
    """Shell for a boot that behaves like otaStage + otaActivate."""
    return ("printf %%s %s > \"$OEVM_OTA_STAGE\"; "
            "printf '{\"hash\":\"%s\",\"sig\":\"%s\"}' > \"$OEVM_OTA_READY\"; "
            "exit 42" % (shq(image.decode()), hash_hex, sig_hex))


# --------------------------------------------------------------------------
# driving the runner
# --------------------------------------------------------------------------
class RunnerProc:
    def __init__(self, root, vm, watchdog=30.0, extra_env=()):
        argv = [sys.executable, RUNNER, "--node-dir", root, "--vm", vm,
                "--watchdog-secs", str(watchdog)]
        for kv in extra_env:
            argv += ["--env", kv]
        self.root = root
        self.lines = []
        self._lock = threading.Lock()
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     bufsize=1, start_new_session=True)
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()

    def _pump(self):
        for line in self.proc.stdout:
            with self._lock:
                self.lines.append(line.rstrip("\n"))

    def snapshot(self):
        with self._lock:
            return list(self.lines)

    def wait_for(self, pattern, timeout=15.0, after=0):
        """Wait for a `runner:` line matching `pattern` at index >= after.
        Returns its index, so a test can assert ORDER, not just presence."""
        rx = re.compile(pattern)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for i, line in enumerate(self.snapshot()):
                if i >= after and rx.search(line):
                    return i
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        raise AssertionError(
            "timed out waiting for %r after index %d; runner exit=%r; log:\n  %s"
            % (pattern, after, self.proc.poll(), "\n  ".join(self.snapshot())))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            self.proc.wait(timeout=5)
        self._t.join(timeout=5)

    def manifest(self):
        with open(os.path.join(self.root, "manifest.json")) as f:
            return json.load(f)


# --------------------------------------------------------------------------
# the five tests
# --------------------------------------------------------------------------
def test_confirm_path():
    """A valid staged image is trialled, confirms, and becomes permanent."""
    with tempfile.TemporaryDirectory() as root:
        provision(root)
        hb = sha_hex(IMAGE_B)
        sb = sign(SEED, bytes.fromhex(hb)).hex()
        vm = write_fake_vm(root, {
            1: stage_and_exit_42(IMAGE_B, hb, sb),
            2: 'touch "$OEVM_OTA_CONFIRM"; sleep 60',
        })
        r = RunnerProc(root, vm)
        try:
            r.wait_for(r"runner: exec a hash=" + sha_hex(IMAGE_A))
            i = r.wait_for(r"runner: staged-ok")
            i = r.wait_for(r"runner: trial b", after=i)
            i = r.wait_for(r"runner: exec b hash=" + hb, after=i)
            r.wait_for(r"runner: confirmed b", after=i)
            m = r.manifest()
            assert m["active"] == "b", m
            assert m["trial"] is False, m
            assert m["prev"] == "a", m
            assert m["slots"]["b"] == {"hash": hb, "sig": sb}, m
        finally:
            r.stop()


def test_reject_bad_sig():
    """A staged image whose signature is off by one byte never gets a boot."""
    with tempfile.TemporaryDirectory() as root:
        provision(root)
        hb = sha_hex(IMAGE_B)
        bad = flip_sig_scalar(sign(SEED, bytes.fromhex(hb))).hex()
        vm = write_fake_vm(root, {1: stage_and_exit_42(IMAGE_B, hb, bad)})
        r = RunnerProc(root, vm)
        try:
            i = r.wait_for(r"runner: staged-reject bad-signature")
            # ... and the old image is simply started again.
            r.wait_for(r"runner: exec a hash=" + sha_hex(IMAGE_A), after=i)
            m = r.manifest()
            assert m["active"] == "a", m
            assert m["trial"] is False, m
            assert m["slots"]["b"] is None, m
            assert os.path.getsize(os.path.join(root, "slots", "b.oimg")) == 0
            assert not os.path.exists(os.path.join(root, "stage", "ready.json"))
            assert "runner: trial b" not in r.snapshot()
        finally:
            r.stop()


def test_revert_on_crash():
    """A trial image that dies before confirming costs exactly one revert."""
    with tempfile.TemporaryDirectory() as root:
        provision(root)
        hb = sha_hex(IMAGE_B)
        sb = sign(SEED, bytes.fromhex(hb)).hex()
        vm = write_fake_vm(root, {
            1: stage_and_exit_42(IMAGE_B, hb, sb),
            2: "exit 1",
        })
        r = RunnerProc(root, vm)
        try:
            i = r.wait_for(r"runner: trial b")
            i = r.wait_for(r"runner: revert a", after=i)
            r.wait_for(r"runner: exec a hash=" + sha_hex(IMAGE_A), after=i)
            m = r.manifest()
            assert m["active"] == "a", m
            assert m["trial"] is False, m
            assert m["prev"] is None, m       # no second chance for b
            assert "runner: confirmed b" not in r.snapshot()
        finally:
            r.stop()


def test_revert_on_watchdog():
    """A trial image that never confirms is killed — process group and all."""
    with tempfile.TemporaryDirectory() as root:
        provision(root)
        hb = sha_hex(IMAGE_B)
        sb = sign(SEED, bytes.fromhex(hb)).hex()
        pidfile = os.path.join(root, "child.pid")
        vm = write_fake_vm(root, {
            1: stage_and_exit_42(IMAGE_B, hb, sb),
            # A grandchild: the watchdog must kill the process GROUP, not just
            # the VM it spawned, or a hung boot leaves an orphan behind.
            2: 'sleep 60 & echo $! > %s; wait' % shq(pidfile),
        })
        r = RunnerProc(root, vm, watchdog=1.0)
        try:
            i = r.wait_for(r"runner: trial b")
            t0 = time.monotonic()
            i = r.wait_for(r"runner: revert a", after=i, timeout=15.0)
            assert time.monotonic() - t0 < 10.0, "watchdog took too long"
            r.wait_for(r"runner: exec a hash=" + sha_hex(IMAGE_A), after=i)
            m = r.manifest()
            assert m["active"] == "a", m
            assert m["trial"] is False, m
            with open(pidfile) as f:
                child = int(f.read().strip())
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                try:
                    os.kill(child, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("grandchild pid %d survived the kill" % child)
        finally:
            r.stop()


def test_verify_fail_refuses_to_boot():
    """VERIFY-on-every-exec, negatively.

    Not one of the plan's five, added because sabotaging `slot_is_trustworthy`
    to `return True` left all five green: the D4 claim ("nothing boots that the
    runner has not checked") had only positive evidence. A tampered slot file
    with nowhere to fall back to must refuse to run at all — exit 3, not a
    best-effort boot.
    """
    with tempfile.TemporaryDirectory() as root:
        provision(root)
        vm = write_fake_vm(root, {})
        with open(os.path.join(root, "slots", "a.oimg"), "wb") as f:
            f.write(IMAGE_A + b"-TAMPERED")
        r = RunnerProc(root, vm)
        try:
            r.wait_for(r"runner: verify-fail a")
            r.proc.wait(timeout=10)
            assert r.proc.returncode == 3, r.proc.returncode
            assert not any(l.startswith("vm: ") for l in r.snapshot()), r.snapshot()
        finally:
            r.stop()


_PORTA_BIN = []


def build_porta(tmp):
    if _PORTA_BIN:
        return _PORTA_BIN[0]
    out = os.path.join(tmp, "porta")
    subprocess.run(["go", "build", "-o", out, "./cmd/porta"],
                   cwd=PORTA_DIR, check=True, timeout=600,
                   capture_output=True, text=True)
    _PORTA_BIN.append(out)
    return out


def test_verify_cross_impl():
    """Go signs, Python verifies. The golden that pins the file format to Task 2.

    Deliberately not a memorized vector: a vector proves this file matches a
    transcript, whereas this proves it matches the binary that will actually
    sign the images.
    """
    if not any(os.access(os.path.join(d, "go"), os.X_OK)
               for d in os.environ.get("PATH", "").split(os.pathsep) if d):
        print("test_verify_cross_impl: SKIP (no go on PATH)")
        return
    # Keep the built binary out of the TemporaryDirectory that goes away, so a
    # second call in the same session reuses it.
    binroot = tempfile.mkdtemp(prefix="porta-bin-")
    atexit.register(shutil.rmtree, binroot, True)
    with tempfile.TemporaryDirectory() as root:
        porta = build_porta(binroot)
        db = os.path.join(root, "porta.db")
        gen = subprocess.run([porta, "--db", db, "key", "gen"],
                             capture_output=True, text=True, timeout=60)
        assert gen.returncode == 0, gen.stderr + gen.stdout

        blob = os.path.join(root, "app.oimg")
        with open(blob, "wb") as f:
            f.write(b"a cross-implementation image, %d bytes of it" % 42)
        put = subprocess.run([porta, "--db", db, "blob", "put", blob],
                             capture_output=True, text=True, timeout=60)
        assert put.returncode == 0, put.stderr + put.stdout

        fields = {}
        for line in put.stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("hash", "sig"):
                fields[parts[0]] = parts[1]
        assert set(fields) == {"hash", "sig"}, put.stdout

        with open(os.path.join(root, "porta.pub")) as f:
            pub = bytes.fromhex(f.read().strip())
        assert len(pub) == 32

        raw_hash = bytes.fromhex(fields["hash"])
        sig = bytes.fromhex(fields["sig"])
        assert len(raw_hash) == 32 and len(sig) == 64
        # porta hashes the file itself — confirm we agree on WHAT was signed.
        with open(blob, "rb") as f:
            assert fields["hash"] == sha_hex(f.read())

        t0 = time.monotonic()
        assert ed25519_verify.verify(pub, sig, raw_hash) is True
        elapsed = time.monotonic() - t0
        assert elapsed < 0.5, "verify took %.3f s" % elapsed

        # Both halves of the signature, plus the message and the key. The
        # scalar flip is the one that forces the verification equation to be
        # evaluated (see flip_sig_scalar); the byte-0 flip usually dies at
        # point decompression instead, which is a different code path worth
        # covering but a much weaker assertion on its own.
        assert ed25519_verify.verify(pub, flip_sig_scalar(sig), raw_hash) is False
        assert ed25519_verify.verify(pub, flip_a_byte(sig), raw_hash) is False
        assert ed25519_verify.verify(pub, sig, flip_a_byte(raw_hash)) is False
        assert ed25519_verify.verify(flip_a_byte(pub), sig, raw_hash) is False
        assert ed25519_verify.verify(pub, sig[:63], raw_hash) is False
        print("test_verify_cross_impl: go-sign/python-verify OK "
              "(%.1f ms/verify)" % (elapsed * 1000))


# --------------------------------------------------------------------------
def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        sys.stdout.write("%-28s " % t.__name__)
        sys.stdout.flush()
        try:
            t()
        except Exception as e:                       # noqa: BLE001
            print("FAIL\n    %s: %s" % (type(e).__name__, e))
            failed += 1
        else:
            print("PASS")
    print("Passed: %d / Failed: %d" % (len(tests) - failed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
