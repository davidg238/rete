#!/usr/bin/env python3
# tuvm#94 stage 5 — drive the REAL MagTagDeepSleepNode across simulated deep sleeps.
# Host deepSleep: exits(0); OEVM_RTC_FILE persists the .rtc.data-equivalent struct to
# disk, so re-exec'ing the node is a real wake/sleep cycle with no hardware. A reusable
# feature (David, 2026-07-24), grown by Tasks 5-7.
import os, struct, subprocess, sys, tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import run_tests_oevm as R   # reuse the Path-B compile + host-oevm invocation

NODE = Path.home() / "workspace/tuvm/zephyr/MagTagDeepSleepNode.nsl"
RTC_MAGIC = -1799482944   # 0x94BE11C0 as int32

# --- Path-B plumbing (mirrors run_tests_oevm.run_test_file_oimg:496-546) -----------
# Compile the node to .oimg ONCE, then re-run the image VM per wake with a custom env.
_OIMG = None
def _compile_once():
    global _OIMG
    if _OIMG: return _OIMG
    ok, err = R._build_pathb_once()          # builds /tmp/oevm_img + /tmp/oevm_layout.json
    if not ok: raise RuntimeError(err)
    oimg = "/tmp/magtag_deepsleep.oimg"
    comp = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"),
         str(NODE), oimg, "--format=oimg", "--layout=/tmp/oevm_layout.json"],
        capture_output=True, text=True)
    if comp.returncode != 0:
        raise RuntimeError("compile failed:\n" + (comp.stderr or comp.stdout))
    _OIMG = oimg
    return oimg

def run_wake(rtc_file, ntp=None, no_ntp=False):
    oimg = _compile_once()
    env = dict(os.environ, OEVM_RTC_FILE=str(rtc_file))   # NOTE: no OEVM_NO_BELL (would raise)
    if no_ntp: env["OEVM_NO_NTP"] = "1"
    elif ntp is not None: env["OEVM_NTP_SECONDS"] = str(ntp)
    else: env.pop("OEVM_NTP_SECONDS", None)
    p = subprocess.run(["/tmp/oevm_img", oimg], capture_output=True, text=True, timeout=30, env=env)
    # deepSleep: exits 0; a nonzero rc means an unhandled raise — surface it.
    assert p.returncode == 0, f"node rc={p.returncode}\nSTDOUT:\n{p.stdout}\nSTDERR:\n{p.stderr}"
    return p.stdout

def read_rtc(path):
    with open(path, "rb") as f: return struct.unpack("<10i", f.read(40))

# --- scenarios --------------------------------------------------------------------
def test_cold_boot_strike():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"          # absent -> cold boot
        out = run_wake(rtc)
        assert "cold boot: free-run from 00:00" in out, out
        assert "wake half 1 bells 1" in out, out      # first ratchet strike
        assert "sleep: arming 1800 s" in out, out       # predictedNow 0 -> full interval
        magic, last, boot, *_ = read_rtc(rtc)
        assert magic == RTC_MAGIC and last == 1 and boot == 1, read_rtc(rtc)

if __name__ == "__main__":
    test_cold_boot_strike(); print("ok")
