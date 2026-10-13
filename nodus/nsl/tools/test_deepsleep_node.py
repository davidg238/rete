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

NODE = Path(__file__).resolve().parent.parent.parent / "oevm" / "zephyr" / "MagTagDeepSleepNode.nsl"
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

def run_wake(rtc_file, ntp=None, no_ntp=False, awake_ms=None):
    oimg = _compile_once()
    # NOTE: no OEVM_NO_BELL (would raise). OEVM_NO_NET (rete#380): the node is a porta
    # node now; with no socket its bridge is `socket: nil` and checks in nowhere, so a wake
    # costs no reply budgets and needs no porta.
    # OEVM_NODE_ID: the host has no factory address and the prim FAILS without one
    # (platform.c PRIM_pf_nodeId, rete#118); the bridge is constructed with an id either
    # way. The MagTag's own, so a row that ever escapes names the right board.
    env = dict(os.environ, OEVM_RTC_FILE=str(rtc_file), OEVM_NO_NET="1", OEVM_NODE_ID="7cdfa11aaaaa")
    # `0 millis` is the node's awake window. On host it is ms since process start, which
    # for a wake that does no WiFi is ~0 — so a test that needs a DEVICE-sized awake
    # window (boot + WiFi + strike, 15-20 s) pins it.
    if awake_ms is not None: env["OEVM_MILLIS_FIXED_MS"] = str(awake_ms)
    if no_ntp: env["OEVM_NO_NTP"] = "1"
    elif ntp is not None: env["OEVM_NTP_SECONDS"] = str(ntp)
    else: env.pop("OEVM_NTP_SECONDS", None)
    p = subprocess.run(["/tmp/oevm_img", oimg], capture_output=True, text=True, timeout=30, env=env)
    # deepSleep: exits 0; a nonzero rc means an unhandled raise — surface it.
    assert p.returncode == 0, f"node rc={p.returncode}\nSTDOUT:\n{p.stdout}\nSTDERR:\n{p.stderr}"
    return p.stdout

# rete#415 Q8/Q9: the kernel's profile request across sleeps, in its own small node.
PROFILE_NODE = TOOLS / "fixtures" / "ProfileAcrossSleepNode.nsl"
_PROFILE_OIMG = None
def run_profile_wake(rtc_file):
    global _PROFILE_OIMG
    if not _PROFILE_OIMG:
        ok, err = R._build_pathb_once()
        if not ok: raise RuntimeError(err)
        oimg = "/tmp/profile_across_sleep.oimg"
        comp = subprocess.run(
            [sys.executable, str(TOOLS / "ns_compile_oevm.py"),
             str(PROFILE_NODE), oimg, "--format=oimg", "--layout=/tmp/oevm_layout.json"],
            capture_output=True, text=True)
        if comp.returncode != 0:
            raise RuntimeError("compile failed:\n" + (comp.stderr or comp.stdout))
        _PROFILE_OIMG = oimg
    env = dict(os.environ, OEVM_RTC_FILE=str(rtc_file), OEVM_NO_NET="1", OEVM_NODE_ID="7cdfa11aaaaa")
    p = subprocess.run(["/tmp/oevm_img", _PROFILE_OIMG], capture_output=True, text=True, timeout=30, env=env)
    assert p.returncode == 0, f"node rc={p.returncode}\nSTDOUT:\n{p.stdout}\nSTDERR:\n{p.stderr}"
    return [l for l in p.stdout.splitlines() if l.startswith(("wake", "ends", "profile:"))]

def test_profile_request_survives_one_sleep():
    # One request, one dump, one rule (Q9 a): opened at once; cut by sleep, so carried;
    # opened again from boot, before main:; cut again, and this time served; then the
    # replayed cell opens nothing, and only a new id profiles again.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        assert run_profile_wake(rtc) == [
            "wake 1", "ends 0 served 0", "profile: window carried to the next wake"], "wake 1"
        w2 = run_profile_wake(rtc)
        assert w2 == ["profile: window from boot, request 5", "wake 2", "ends 0 served 0",
                      "profile: window cut by sleep"], w2
        assert run_profile_wake(rtc) == ["wake 3", "ends 0 served 5"], "wake 3"
        assert run_profile_wake(rtc) == ["wake 4", "ends 1 served 6"], "wake 4"

def read_rtc(path):
    with open(path, "rb") as f: return struct.unpack("<10i", f.read(40))

def seed_rtc(path, last=0, boot=0, anchor=0, elapsed=0, drift=0, stale=0,
             d24max=0, d24min=0, syncs=0):
    # `drift` is field 5 = the CUMULATIVE calibration ppm applied when arming the sleep
    # (not a single measurement). 0 = uncalibrated, so a seeded scenario arms its bare
    # target unless it deliberately exercises disciplining.
    with open(path, "wb") as f:
        f.write(struct.pack("<10i", RTC_MAGIC, last, boot, anchor, elapsed,
                            drift, stale, d24max, d24min, syncs))

# --- scenarios --------------------------------------------------------------------
def test_cold_boot_syncs_and_never_strikes():
    # A COLD BOOT MUST NOT STRIKE (David, 2026-07-24): power returns at an arbitrary
    # second, so a bell then is a bell at a meaningless time. Instead: sync, let
    # applySync: advance the ratchet from the free-run 0 to true time, paint, and sleep
    # to the next TRUE boundary. NTP says 22:15:08 (sod 80108) -> half 44, and the
    # 22:30 boundary is 81000 - 80108 = 892 s out, NOT a full slot.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"          # absent -> cold boot
        out = run_wake(rtc, ntp=80108)
        assert "cold boot: free-run from 00:00" in out, out
        assert "wake half" not in out, out                    # THE POINT: no bell at boot
        assert "sync true 80108 predicted 0 -> advance half 44" in out, out
        assert "sleep: target 892 s" in out, out              # phase-aligned, not 1800
        # ...and armed 9 s longer, because initColdState: seeds the factory calibration:
        # a reflashed board is disciplined from its FIRST sleep, not from its first sync.
        assert "sleep: arming 901 s" in out, out
        magic, last, boot, anchor, elapsed, *_ = read_rtc(rtc)
        assert magic == RTC_MAGIC, read_rtc(rtc)
        assert last == 44, read_rtc(rtc)                      # ratchet set to true time
        assert boot == 0, read_rtc(rtc)                       # no strike -> no bootTick
        assert anchor == 80108, read_rtc(rtc)
        # Painted AFTER the sync, so it shows the SYNCED state: the ntp row reads `sync`
        # (synced, no drift measurable on an advance) and stale is 0 — not the `--`
        # placeholder the pre-sync paint used to show.
        painted = [l for l in out.splitlines() if l.startswith("| ")]
        assert len(painted) == 9, painted
        assert any(l.startswith("| ntp") and l.endswith("sync") for l in painted), painted
        assert any(l.startswith("| sync") and l.endswith("0d/00h") for l in painted), painted
        for line in painted:
            assert len(line) - 2 <= 12, line

def test_cold_boot_first_strike_has_the_right_count():
    # The end-to-end of the above: after a synced cold boot, the FIRST bell the house
    # hears must be the right count at the right time. Boot at 22:15:08 -> the next
    # boundary is 22:30 = half 45 = 5 bells.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        run_wake(rtc, ntp=80108)                              # cold boot, silent
        out = run_wake(rtc)                                   # the 22:30 wake
        assert "wake half 45 bells 5" in out, out
        _, last, boot, *_ = read_rtc(rtc)
        assert last == 45 and boot == 1, read_rtc(rtc)
        # The arming interval is deliberately NOT asserted here: it depends on the awake
        # window, which on an unpinned host run is whatever this process took. Tests that
        # care pass awake_ms. (`0 millis` used to be CLOCK_MONOTONIC since the MACHINE
        # booted, which made every phase after wake 1 meaningless — src/vm.c now answers
        # ms since this process started, matching the k_uptime_get() contract on device.)

def test_cold_boot_ntp_fail_free_runs_silently():
    # Design §8: no NTP at boot -> keep the free-run-from-00:00 state and correct at the
    # first good watch-end. Still NO strike (an unknown clock is not a reason to ring),
    # and the panel says FAIL rather than dashes.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        out = run_wake(rtc, no_ntp=True)
        assert "cold boot: free-run from 00:00" in out, out
        assert out.count("ntp miss") == 3, out
        assert "sync failed, stale grows" in out, out
        assert "wake half" not in out, out                    # silent even with no clock
        assert "sleep: target 1800 s" in out, out             # free-run: a full slot
        assert "sleep: arming 1819 s" in out, out             # ...disciplined by the seed
        _, last, boot, *_ = read_rtc(rtc)
        assert last == 0 and boot == 0, read_rtc(rtc)
        painted = [l for l in out.splitlines() if l.startswith("| ")]
        assert any(l.startswith("| ntp") and l.endswith("FAIL") for l in painted), painted

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
        for row in ("| oevm", "| ppm", "| up", "| ntp", "| 24h", "| sync"):
            assert row in out, (row, out)
        # row 8 = the unlabeled 24h-min continuation row (distinct from the 24h max row)
        assert any(l.strip("| ").strip() == "-3" for l in painted), painted
        assert "+0.0s" in out, out                        # drift 0 measured this watch
        for line in painted:
            assert len(line) - 2 <= 12, line              # 12-col hard limit (design §5)

# --- the overnight run 2026-07-24/25 defects (tuvm#94, design §11) ----------------
# Each of the five was found by an unattended run at the real cadence and NONE by this
# harness, which was green throughout. These are the specific gaps.

def test_hold_arms_the_counts_own_boundary():
    # DEFECT 5, the one that made the bell COUNT wrong from 04:00 onward. The class
    # comment promises "we hold the count and lengthen the next sleep"; the code held the
    # count and then re-anchored to TRUE time, so the next strike fired at the next TRUE
    # boundary and the count/clock gap was baked in — growing one slot per watch end.
    # The 04:00 case from the capture: struck half 8 at 03:57:40 (osc fast), NTP says
    # 14260. The next strike is half 9, whose boundary is 16200 — so the sleep must be
    # 16200 - 14260 = 1940 s (LONGER than a slot), not 140 s to the next true boundary.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=14260)
        assert "-> hold half 8" in out, out
        assert "sleep: target 1940 s" in out, out       # the count's boundary...
        target = int(out.split("sleep: target ")[1].split(" ")[0])
        assert target > 1800, out                       # ...which is LONGER than a slot

def test_sync_across_midnight_holds_and_records_drift():
    # DEFECTS 1 and 2. `trueHalf > lastHalf` is a magnitude compare on a value that wraps
    # 47->0: at midnight the count had already struck half 0 (8 bells) 66 s early, NTP
    # said 23:58:54 (trueHalf 47), and `47 > 0` read a 66 s LEAD as a 23.5 h LAG. The
    # ratchet was pushed backward and 8 bells struck a second time, 63 s later. Modular
    # forward distance reads it as 1 slot ahead -> HOLD, and the real -66 s drift is
    # measured instead of discarded down the advance branch (defect 2).
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=47, anchor=0, elapsed=86400)   # predictedNow 0 = midnight
        out = run_wake(rtc, ntp=86334)                    # 23:58:54 -> trueHalf 47
        assert out.count("wake half") == 1, out           # NOT a second 8 bells
        assert "-> hold half 0" in out, out               # held, not pushed back to 47
        _, last, _, _, _, cal, *_ = read_rtc(rtc)
        assert last == 0, read_rtc(rtc)                   # ratchet never moves backward
        assert cal != 0, read_rtc(rtc)                    # defect 2: drift WAS recorded
        assert "-66.0s" in out, out                       # the real lead, not a 23.5 h gap
        # ...and the next strike is half 1 at 00:30 true: 1800 - 86334 mod 86400.
        assert "sleep: target 1866 s" in out, out

def test_drift_measurement_excludes_the_awake_window():
    # Defect 6 again, in the MEASUREMENT path, and it poisons the calibration loop rather
    # than one bell. predictedNow is the node's clock at the STRIKE; trueSec was read a
    # whole awake window later. Comparing them scores the boot/WiFi seconds as oscillator
    # drift — +1041 ppm per sync at a 15 s window — so a PERFECT clock gets "corrected"
    # every watch end and the calibration walks away from the truth instead of converging.
    #
    # Here the clock is exactly right: struck half 8 at 04:00:00, NTP read 15 s of awake
    # work later says 04:00:15. The measured drift must be 0.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=14415, awake_ms=15000)
        assert "-> hold half 8" in out, out
        assert "+0.0s" in out, out                     # a perfect clock measures as perfect
        _, _, _, _, _, cal, *_ = read_rtc(rtc)
        assert cal == 0, read_rtc(rtc)                 # ...and is not "corrected"

def test_sub_floor_window_reports_the_sync_and_skips_the_ppm():
    # DEFECT 3. A 68 s window between syncs turned a 4 s error into `ppm +58823` and
    # polluted the rolling 24 h window. Filed as cosmetic; re-prioritised once
    # disciplining applies the ppm to the SLEEP, where +58823 is a 5.9% correction.
    # Below the floor: report the sync (ntp row still shows the seconds), skip the rate.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=14332, elapsed=68)   # predictedNow 14400, 68 s window
        out = run_wake(rtc, ntp=14404)                    # +4 s over 68 s = 58823 ppm
        assert "-> hold half 8" in out, out
        assert "below floor" in out, out                  # said out loud, not silent
        _, _, _, _, _, cal, _, d24max, d24min, syncs = read_rtc(rtc)
        assert cal == 0, read_rtc(rtc)                    # calibration NOT polluted
        assert (d24max, d24min, syncs) == (0, 0, 0), read_rtc(rtc)   # window untouched
        assert "+4.0s" in out, out                        # the sync itself IS reported

def test_disciplining_lengthens_the_sleep_when_the_oscillator_runs_fast():
    # The measured RC error is ~10,700 ppm FAST and stable within 3% across nine hours, so
    # the commanded sleep ends early and bells arrive up to 155 s before the boundary.
    # Scale the armed sleep by the persisted calibration: a 1800 s target at -10,000 ppm
    # must be COMMANDED as 1818 s so that 1800 real seconds elapse.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=6, anchor=0, elapsed=12600, drift=-10000)   # half 7, not a watch end
        out = run_wake(rtc)                                # no sync -> calibration untouched
        assert "wake half 7 bells 7" in out, out
        assert "sleep: target 1800 s cal -10000" in out, out
        assert "sleep: arming 1818 s" in out, out

def test_disciplining_shortens_the_sleep_when_the_oscillator_runs_slow():
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=6, anchor=0, elapsed=12600, drift=10000)
        out = run_wake(rtc)
        assert "sleep: arming 1782 s" in out, out

def test_disciplining_rounds_the_correction_rather_than_truncating_it():
    # The correction is whole seconds, so how the fraction is handled is not cosmetic: a
    # truncating spelling threw away up to a second PER SLOT, which is 428 ppm of fresh
    # systematic error — the same order as the residual disciplining exists to remove, and
    # it showed up as bells walking ~30 s off across a watch in the simulated day.
    # 1785 s at -10513 ppm is 18.77 s of correction: 19, not 18.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=6, anchor=0, elapsed=12600, drift=-10513)
        out = run_wake(rtc, awake_ms=15000)
        assert "sleep: target 1785 s" in out, out
        assert "sleep: arming 1804 s" in out, out

def test_no_calibration_arms_the_bare_target():
    # A never-measured board (and one whose only window was sub-floor) scales by nothing —
    # the correction is opt-in on evidence, never a guess.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=6, anchor=0, elapsed=12600, drift=0)
        out = run_wake(rtc)
        assert "sleep: target 1800 s cal +0" in out, out
        assert "sleep: arming 1800 s" in out, out

def test_sleep_subtracts_the_awake_window_so_the_strike_lands_on_the_boundary():
    # DEFECT 6, found by walking the disciplined node across a simulated day: every bell
    # landed a steady ~15 s LATE — exactly the awake window. The wake sequence is
    # strike -> work -> sleep, so the boot/WiFi/strike seconds fall BETWEEN this strike and
    # the next; accruing them (which elapsedSec does) without also taking them out of the
    # sleep pushes each strike one awake-window past its boundary, forever. It survives a
    # sync (which re-anchors) and it would survive the DS3231 (which fixes the rate), so
    # disciplining alone would have left the bells 15 s late.
    #
    # The invariant that makes it right: after arming, predictedNow must land EXACTLY on
    # the boundary of the count about to be struck — 12600 + 1785 + 15 = 14400.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=6, anchor=0, elapsed=12600)   # 03:30, half 7 next, not a watch end
        out = run_wake(rtc, awake_ms=15000)              # a device-sized awake window
        assert "sleep: target 1785 s" in out, out        # 1800 to the boundary, less 15 awake
        _, _, _, _, elapsed, *_ = read_rtc(rtc)
        assert elapsed == 14400, read_rtc(rtc)           # dead on half 8's boundary

def test_synced_wake_subtracts_only_the_work_after_the_ntp_read():
    # The other half of defect 6. On a SYNCED wake predictedNow is trueSec, which was true
    # when NTP answered — not at boot — so subtracting the whole awake window over-corrects
    # by everything before the read (WiFi association is most of it) and turns a 15 s late
    # bell into a 15 s early one. Only the work AFTER the read may come out of the sleep.
    #
    # Limitation: OEVM_MILLIS_FIXED_MS pins the clock, so this poses "the read happened
    # 15 s in and nothing measurable followed" -> subtract 0. It distinguishes basis-at-read
    # from basis-at-boot, which is the bug; it cannot exercise a nonzero post-read window.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        seed_rtc(rtc, last=7, anchor=0, elapsed=14400)
        out = run_wake(rtc, ntp=14400, awake_ms=15000)     # exact 04:00 -> hold half 8
        assert "-> hold half 8" in out, out
        assert "sleep: target 1800 s" in out, out           # NOT 1785
        assert "awake 0 s" in out, out                      # basis is the read, not boot

def test_cold_boot_adopts_true_time_late_at_night():
    # The modular compare cannot tell "47 slots behind" from "1 slot ahead", and a cold
    # boot at 23:30 is genuinely 47 slots from the free-run 0. A never-synced board has no
    # ratchet to protect — it ADOPTS true time, which is what keeps the wrap-safe compare
    # from reading a cold boot as a hold and going silent until tomorrow.
    with tempfile.TemporaryDirectory() as d:
        rtc = Path(d) / "rtc.bin"
        out = run_wake(rtc, ntp=84600)                     # 23:30:00 -> half 47
        assert "-> advance half 47" in out, out
        _, last, *_ = read_rtc(rtc)
        assert last == 47, read_rtc(rtc)
        assert "sleep: target 1800 s" in out, out          # midnight, one slot out

def test_a_simulated_day_on_a_fast_oscillator_keeps_every_bell_on_its_boundary():
    # THE END-TO-END ONE, and the only test here that speaks to the product claim: does the
    # house hear the right number of bells at the right minute? Every other test pins one
    # decision; this walks the real node through a night against a simulated 1.07% fast RC
    # oscillator — the rate actually measured on the board — with a device-sized awake
    # window, across midnight and a watch-end sync.
    #
    # It is the test the 2026-07-24 run should not have needed: defects 5 and 6 and the
    # truncated correction were all invisible to the per-decision tests above and all three
    # are caught here, because only a multi-wake walk lets a per-cycle error accumulate into
    # an audible one. It is a SIMULATION, not a soak — it models the oscillator, so it
    # cannot speak to power draw, radio behaviour, or anything thermal.
    FAST, AWAKE, START = 1.0 / 1.0107, 15, 80108      # 22:15:08, the overnight run's anchor
    with tempfile.TemporaryDirectory() as d:
        rtc, true, struck = Path(d) / "rtc.bin", float(START), []
        for _ in range(12):                            # 22:15 -> ~03:30, crossing midnight
            # NTP answers AWAKE seconds into the wake, which is when the node stamps its
            # clock basis — so the model and the node agree on when "now" was.
            out = run_wake(rtc, ntp=int((true + AWAKE) % 86400), awake_ms=AWAKE * 1000)
            if "wake half " in out:
                half = int(out.split("wake half ")[1].split(" ")[0])
                bells = int(out.split(" bells ")[1].split("\n")[0])
                err = (true % 86400) - half * 1800
                if err >  43200: err -= 86400          # a strike just before midnight
                if err < -43200: err += 86400
                struck.append((half, bells, err))
            armed = int(out.split("sleep: arming ")[1].split(" ")[0])
            true += AWAKE + armed * FAST               # the sleep really ends early
        assert len(struck) == 11, struck               # 12 wakes, the first is a cold boot
        for half, bells, err in struck:
            assert bells == (half % 8 or 8), struck    # the count the hour deserves
            assert abs(err) <= 3, struck               # ...at the minute it deserves it
        assert any(h == 0 for h, _, _ in struck), struck    # crossed midnight + a watch end

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
    test_cold_boot_syncs_and_never_strikes()
    test_cold_boot_first_strike_has_the_right_count()
    test_cold_boot_ntp_fail_free_runs_silently()
    test_hold_when_ahead()
    test_advance_silent_when_behind()
    test_sync_fail_keeps_striking()
    test_watch_end_panel()
    test_hold_arms_the_counts_own_boundary()
    test_sync_across_midnight_holds_and_records_drift()
    test_drift_measurement_excludes_the_awake_window()
    test_sub_floor_window_reports_the_sync_and_skips_the_ppm()
    test_disciplining_lengthens_the_sleep_when_the_oscillator_runs_fast()
    test_disciplining_shortens_the_sleep_when_the_oscillator_runs_slow()
    test_disciplining_rounds_the_correction_rather_than_truncating_it()
    test_no_calibration_arms_the_bare_target()
    test_sleep_subtracts_the_awake_window_so_the_strike_lands_on_the_boundary()
    test_cold_boot_adopts_true_time_late_at_night()
    test_a_simulated_day_on_a_fast_oscillator_keeps_every_bell_on_its_boundary()
    test_ratchet_monotonic_across_sleeps()
    test_profile_request_survives_one_sleep()
    print("ok")
