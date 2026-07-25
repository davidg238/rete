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
RTC_MAGIC = 348000704   # 0x14BE11C0 — must fit a 32-bit SMI (|v| < 2^30); the old
                        # 0x94BE11C0 only round-tripped on the 64-bit host

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

def seed_rtc(path, last=0, boot=0, anchor=0, elapsed=0, drift=0, stale=0,
             d24max=0, d24min=0, syncs=0):
    with open(path, "wb") as f:
        f.write(struct.pack("<10i", RTC_MAGIC, last, boot, anchor, elapsed,
                            drift, stale, d24max, d24min, syncs))

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

def test_hold_when_ahead():
    # Watch end at 04:00 (half 8). Seed lastStruckHalf=7, predictedNow=14400 (04:00).
    # NTP says 03:59:57 -> trueHalf 7 <= struck 8 after the +1 strike -> HOLD, no re-strike.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=14397)
        # count the node's OWN per-wake marker, not "struck" — the host bell leaf also
        # prints "bell: struck N", so out.count("struck") would be 2, not 1.
        assert out.count("wake half") == 1, out               # exactly one strike wake
        assert "-> hold half 8" in out, out                    # count not moved back
        _, last, *_ = read_rtc(rtc)
        assert last == 8, read_rtc(rtc)                        # ratchet held at 8

def test_advance_silent_when_behind():
    # Missed wakes: seed lastStruckHalf=7 but NTP says it is already 06:00 (half 12).
    # After the +1 strike (half 8) the sync finds trueHalf 12 > 8 -> advance to 12,
    # SILENTLY (only the one strike this wake, skipped bells not rung).
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=21600)                         # 06:00:00
        assert out.count("wake half") == 1, out               # one strike wake, skipped bells NOT rung
        assert "-> advance half 12" in out, out
        _, last, *_ = read_rtc(rtc)
        assert last == 12, read_rtc(rtc)

def test_sync_fail_keeps_striking():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400, stale=0)
        out = run_wake(rtc, no_ntp=True)
        assert out.count("ntp miss") == 3, out                 # 3 attempts, all miss
        assert "sync failed, stale grows" in out, out
        _, last, _, _, _, _, stale, *_ = read_rtc(rtc)
        assert last == 8 and stale == 14400, read_rtc(rtc)     # struck, count kept, stale grew

def test_watch_end_panel():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400, syncs=2, d24max=8, d24min=-3)
        out = run_wake(rtc, ntp=14400)                      # exact 04:00 -> on-time, drift 0
        # heap row is intentionally skipped on host (16 MB arena -> "/16384K" overflows 12 cols).
        assert "sync true 14400 predicted 14400 -> hold half 8" in out, out   # on-time -> drift measured
        painted = [l for l in out.splitlines() if l.startswith("| ")]
        assert len(painted) == 9, painted               # 10 rows minus the host-skipped heap row
        assert out.count("| ----------") == 2, out       # BOTH separator rows render
        for row in ("| tuvm", "| ppm", "| up", "| ntp", "| 24h", "| sync"):
            assert row in out, (row, out)
        # row 8 = the unlabeled 24h-min continuation row (distinct from the 24h max row)
        assert any(l.strip("| ").strip() == "-3" for l in painted), painted
        assert "+0.0s" in out, out                        # drift 0 measured this watch
        for line in painted:
            assert len(line) - 2 <= 12, line              # 12-col hard limit (design §5)

def _mono_mod48(seq):
    # Each step advances by exactly +1 half mod 48; the 47->0 wrap is (0-47)%48 == 1,
    # a forward step, never a decrease. Any step outside 1..47 is a ratchet violation.
    for a, b in zip(seq, seq[1:]):
        step = (b - a) % 48
        if step != 1:
            return False
    return True

def test_ratchet_monotonic_across_sleeps():
    # Seed the count near the day's end (last struck = 44) so a short run CROSSES the
    # 47->0 midnight wrap — 12 cold wakes would only reach half 12 and never wrap. No NTP,
    # so the ratchet advances purely on its own timebase across every simulated sleep.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=44, anchor=0, elapsed=0)
        halves = []
        for _ in range(10):                        # 45,46,47,0,1,2,3,4,5,6
            out = run_wake(rtc)                     # no ntp -> free-run on the ratchet
            halves.append(int(out.split("wake half ")[1].split(" ")[0]))
        assert _mono_mod48(halves), halves          # strictly +1 each sleep, wrap included
        assert halves[0] == 45 and 0 in halves, halves   # crossed the 47->0 midnight wrap
        assert halves[3] == 0, halves               # the wrap lands where expected

if __name__ == "__main__":
    test_cold_boot_strike()
    test_hold_when_ahead()
    test_advance_silent_when_behind()
    test_sync_fail_keeps_striking()
    test_watch_end_panel()
    test_ratchet_monotonic_across_sleeps()
    print("ok")
