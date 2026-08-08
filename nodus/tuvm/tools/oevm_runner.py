#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""oevm_runner.py — the host stand-in for MCUboot (rete#25, design §5).

It is a *bootloader*, not a supervisor: it owns two image slots, decides which
one is trustworthy, execs it, and reacts to how it died. It never talks to
porta and never parses CBOR — its whole world is a manifest, a staged file, two
marker files and an exit code.

Per-node directory (`--node-dir`):

    manifest.json   {"active":"a","prev":null,"trial":false,
                     "slots":{"a":{"hash":"<hex>","sig":"<hex>"},"b":null}}
    slots/a.oimg    slots/b.oimg
    stage/ready.json  {"hash":"<hex>","sig":"<hex>"}   <- written by otaActivate
    confirm                                            <- written by otaConfirm
    pub.key         porta's ed25519 public key, 32 bytes as lower-case hex

Provisioning (slot a's file, its manifest entry and pub.key) is the caller's
job — the runner refuses to invent trust it was not given.

The two contracts with the other seats, stated so a future edit cannot drift
from them silently:

* **Exit 42 is STAGE_READY** (`ota_exit_stage_ready` in `nodus/tuvm/src/
  backend/posix/ota_posix.c`). Any other exit is an ordinary death.
* **The signature covers the 32 RAW hash bytes**, not their hex
  (`blob.SignHash` in `porta/internal/blob/keys.go` signs `hash[:]`).

Every state transition prints one `runner: ...` line to stdout and flushes.
Those lines are a wire contract: the #25 host gate greps them.

    runner: exec <slot> hash=<hex>
    runner: staged-ok
    runner: staged-reject <why>
    runner: trial <slot>
    runner: confirmed <slot>
    runner: revert <slot>
    runner: verify-fail <slot>

The loop never ends on its own (G11: reboot IS the recovery). It stops on
SIGTERM/SIGINT, or exits 3 when neither slot is trustworthy — the one case
where continuing would mean booting something unverified.
"""

import argparse
import hashlib
import json
import os
import shlex
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519_verify  # noqa: E402

SLOTS = ("a", "b")
EXIT_STAGE_READY = 42
EXIT_NOTHING_TRUSTWORTHY = 3
POLL_SECS = 0.05          # confirm-marker / child-exit cadence


def other(slot):
    return "b" if slot == "a" else "a"


def say(msg):
    print("runner: " + msg, flush=True)


# --------------------------------------------------------------------------
# the node directory
# --------------------------------------------------------------------------
class NodeDir:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.manifest_path = os.path.join(self.root, "manifest.json")
        self.stage_dir = os.path.join(self.root, "stage")
        self.ready_path = os.path.join(self.stage_dir, "ready.json")
        self.confirm_path = os.path.join(self.root, "confirm")
        self.pub_path = os.path.join(self.root, "pub.key")
        os.makedirs(os.path.join(self.root, "slots"), exist_ok=True)
        os.makedirs(self.stage_dir, exist_ok=True)

    def slot_path(self, slot):
        return os.path.join(self.root, "slots", slot + ".oimg")

    # -- manifest ----------------------------------------------------------
    def load(self):
        with open(self.manifest_path) as f:
            m = json.load(f)
        m.setdefault("active", "a")
        m.setdefault("prev", None)
        m.setdefault("trial", False)
        m.setdefault("slots", {})
        for s in SLOTS:
            m["slots"].setdefault(s, None)
        return m

    def save(self, m):
        """Write through a temp file: a torn manifest is an unbootable node,
        and this is the one file whose loss cannot be recovered by re-fetching."""
        tmp = self.manifest_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(m, f, indent=2, sort_keys=True)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.manifest_path)

    # -- the trust decision ------------------------------------------------
    def pub_key(self):
        """porta's 32-byte public key, or None if pub.key is absent/malformed."""
        try:
            with open(self.pub_path) as f:
                txt = f.read().strip()
        except OSError:
            return None
        try:
            raw = bytes.fromhex(txt)
        except ValueError:
            return None
        return raw if len(raw) == 32 else None

    def slot_is_trustworthy(self, m, slot):
        """The slot's file hashes to its recorded hash AND that hash carries a
        good porta signature. A slot with no manifest entry has never been
        provisioned and is therefore not runnable — silence is not consent."""
        entry = (m.get("slots") or {}).get(slot)
        if not entry or not entry.get("hash") or not entry.get("sig"):
            return False
        digest = sha256_file(self.slot_path(slot))
        if digest is None or digest != entry["hash"]:
            return False
        pub = self.pub_key()
        if pub is None:
            return False
        try:
            sig = bytes.fromhex(entry["sig"])
            raw_hash = bytes.fromhex(entry["hash"])
        except ValueError:
            return False
        return ed25519_verify.verify(pub, sig, raw_hash)


def sha256_file(path):
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                h.update(chunk)
    except OSError:
        return None
    return h.hexdigest()


def unlink_quiet(path):
    try:
        os.unlink(path)
    except OSError:
        pass


# --------------------------------------------------------------------------
# the runner
# --------------------------------------------------------------------------
class Runner:
    def __init__(self, node_dir, vm, image_args, watchdog_secs, extra_env):
        self.nd = NodeDir(node_dir)
        self.vm = vm
        self.image_args = image_args
        self.watchdog_secs = watchdog_secs
        self.extra_env = extra_env
        self.proc = None
        self.stopping = False

    # -- signals -----------------------------------------------------------
    def install_signal_handlers(self):
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, self._on_term)

    def _on_term(self, signum, frame):
        self.stopping = True
        self.kill_child()

    def kill_child(self):
        """Kill the VM's whole process group. The VM has no children today, but
        a fake VM in a test does (`sleep`), and the target will one day too —
        killing only the leader leaves an orphan holding the console."""
        p = self.proc
        if p is None or p.poll() is not None:
            return
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        except OSError:
            try:
                p.kill()
            except OSError:
                pass
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass

    # -- one boot ----------------------------------------------------------
    def spawn(self, m, active):
        env = os.environ.copy()
        env["OEVM_IMAGE_HASH"] = m["slots"][active]["hash"]
        env["OEVM_OTA_STAGE"] = self.nd.slot_path(other(active))
        env["OEVM_OTA_READY"] = self.nd.ready_path
        env["OEVM_OTA_CONFIRM"] = self.nd.confirm_path
        if m["trial"]:
            env["OEVM_OTA_TRIAL"] = "1"
        else:
            env.pop("OEVM_OTA_TRIAL", None)
        env.update(self.extra_env)
        argv = [self.vm, self.nd.slot_path(active)] + self.image_args
        # start_new_session so the child leads its own process group: the
        # watchdog kill must reach everything the boot spawned.
        return subprocess.Popen(argv, env=env, start_new_session=True)

    def supervise(self, m, active):
        """Run the child to completion (or to a watchdog kill).

        Returns (exit_code_or_None, confirmed). exit_code is None when the
        watchdog killed it — the difference matters: a killed trial reverts
        without ever consulting its exit status."""
        trial = bool(m["trial"])
        confirmed = False
        deadline = time.monotonic() + self.watchdog_secs if trial else None

        while True:
            # Look for the marker BEFORE reaping: a VM that touches confirm and
            # exits in the same breath must still count as confirmed.
            if trial and not confirmed and os.path.exists(self.nd.confirm_path):
                confirmed = True
                deadline = None
                m["trial"] = False
                self.nd.save(m)
                say("confirmed " + active)

            rc = self.proc.poll()
            if rc is not None:
                if trial and not confirmed and os.path.exists(self.nd.confirm_path):
                    confirmed = True
                    m["trial"] = False
                    self.nd.save(m)
                    say("confirmed " + active)
                return rc, confirmed

            if self.stopping:
                return None, confirmed

            if deadline is not None and time.monotonic() >= deadline:
                self.kill_child()
                return None, confirmed

            time.sleep(POLL_SECS)

    # -- exit 42 -----------------------------------------------------------
    def handle_stage_ready(self, m, active):
        """The image asked to be replaced by the staged one. Everything that
        can be checked outside the staged image is checked here, before it is
        ever given a boot: this is the D4 seat — verify OUTSIDE the image."""
        staged = other(active)
        reject = None
        hash_hex = sig_hex = None

        try:
            with open(self.nd.ready_path) as f:
                ready = json.load(f)
            hash_hex = ready["hash"]
            sig_hex = ready["sig"]
            if not isinstance(hash_hex, str) or not isinstance(sig_hex, str):
                raise ValueError("non-string field")
        except (OSError, ValueError, KeyError, TypeError) as e:
            reject = "unreadable-ready-json(%s)" % type(e).__name__

        if reject is None:
            digest = sha256_file(self.nd.slot_path(staged))
            if digest is None:
                reject = "staged-slot-missing"
            elif digest != hash_hex.lower():
                # Also the crash-between-write-and-exit case: ready.json
                # describes an image the slot does not hold.
                reject = "hash-mismatch"

        pub = self.nd.pub_key()
        if reject is None and pub is None:
            reject = "no-pub-key"

        if reject is None:
            try:
                sig = bytes.fromhex(sig_hex)
                raw_hash = bytes.fromhex(hash_hex)
            except ValueError:
                sig = raw_hash = None
            if sig is None or len(sig) != 64 or raw_hash is None or len(raw_hash) != 32:
                reject = "malformed-signature"
            elif not ed25519_verify.verify(pub, sig, raw_hash):
                reject = "bad-signature"

        if reject is not None:
            say("staged-reject " + reject)
            # Truncate rather than delete: the slot file IS the resume cursor
            # for the C seam (ota.c reads its length), so zero means "start
            # over", while a missing file and an empty one must stay the same
            # thing. Then re-exec the image that is still known good.
            with open(self.nd.slot_path(staged), "wb"):
                pass
            unlink_quiet(self.nd.ready_path)
            return m

        say("staged-ok")
        m["prev"] = active
        m["active"] = staged
        m["trial"] = True
        m["slots"][staged] = {"hash": hash_hex.lower(), "sig": sig_hex.lower()}
        self.nd.save(m)
        # Trial entry deletes any stale marker, so a confirm can only ever come
        # from the boot now being tried (design §6, "stale confirm marker").
        unlink_quiet(self.nd.confirm_path)
        unlink_quiet(self.nd.ready_path)
        return m

    def revert(self, m):
        """Back to `prev`, permanently. prev is cleared so a slot can only cost
        one revert — 'a crashing trial image never gets a second chance'."""
        prev = m.get("prev")
        m["trial"] = False
        if prev and prev in SLOTS:
            m["active"] = prev
            m["prev"] = None
            self.nd.save(m)
            say("revert " + prev)
        else:
            # Nothing to fall back to. Clearing trial is not forgiveness: the
            # next pass re-VERIFYs, and an unverifiable slot still exits 3.
            self.nd.save(m)
        return m

    # -- the loop ----------------------------------------------------------
    def run(self):
        while not self.stopping:
            m = self.nd.load()
            active = m["active"]

            # 1. VERIFY, every exec, no exceptions.
            if not self.nd.slot_is_trustworthy(m, active):
                say("verify-fail " + active)
                prev = m.get("prev")
                if prev and prev in SLOTS and self.nd.slot_is_trustworthy(m, prev):
                    m["active"] = prev
                    m["prev"] = None
                    m["trial"] = False
                    self.nd.save(m)
                    say("revert " + prev)
                    continue
                return EXIT_NOTHING_TRUSTWORTHY

            if m["trial"]:
                unlink_quiet(self.nd.confirm_path)
                say("trial " + active)

            # 2. RUN.
            say("exec %s hash=%s" % (active, m["slots"][active]["hash"]))
            self.proc = self.spawn(m, active)
            rc, confirmed = self.supervise(m, active)
            self.proc = None
            if self.stopping:
                break

            was_trial = bool(m["trial"]) and not confirmed
            m = self.nd.load()   # supervise() may have written trial=false

            # 3. On exit.
            if rc == EXIT_STAGE_READY:
                m = self.handle_stage_ready(m, active)
                continue
            if was_trial:
                # Crashed, exited, or was killed by the watchdog without ever
                # confirming. Both look identical from here, and should.
                m = self.revert(m)
                continue
            # Confirmed, or never on trial: reboot IS the recovery (G11).
        return 0


# --------------------------------------------------------------------------
def parse_env(pairs):
    env = {}
    for kv in pairs or []:
        if "=" not in kv:
            raise SystemExit("--env expects K=V, got %r" % kv)
        k, v = kv.split("=", 1)
        env[k] = v
    return env


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="A/B slot runner for oevm — the host stand-in for MCUboot (#25).")
    ap.add_argument("--node-dir", required=True,
                    help="per-node directory holding manifest.json, slots/, pub.key")
    ap.add_argument("--vm", required=True, help="path to the oevm binary")
    ap.add_argument("--image-args", default="",
                    help="extra argv for the VM, as one shell-quoted string")
    ap.add_argument("--watchdog-secs", type=float, default=30.0,
                    help="seconds a trial boot has to write the confirm marker")
    ap.add_argument("--env", action="append", metavar="K=V", default=[],
                    help="extra environment for the VM; repeatable")
    args = ap.parse_args(argv)

    r = Runner(args.node_dir, args.vm, shlex.split(args.image_args),
               args.watchdog_secs, parse_env(args.env))
    r.install_signal_handlers()
    try:
        return r.run()
    finally:
        r.kill_child()


if __name__ == "__main__":
    sys.exit(main())
