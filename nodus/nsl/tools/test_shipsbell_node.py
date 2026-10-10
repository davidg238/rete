#!/usr/bin/env python3
# rete#380 — drive the REAL ShipsBellNodeC6 (the always-on bell) on the host image VM.
# The sibling of test_deepsleep_node.py for the other power mode. The host clock does not
# tick and the host alarm seam refuses every arm, so a boundary never ARRIVES here; what
# this proves is the node's WIRING — the collaborators are built, the sync corrects the
# part, the seed rings nothing and arms the watchdog, the bridge is `socket: nil`, and a
# frozen clock produces no bell and no watchdog complaint — and that it runs the same
# library the deep-sleep node runs. The node never returns: each run is killed after a
# few seconds and its output read back.
import os, subprocess, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import run_tests_oevm as R   # reuse the Path-B compile + host-oevm invocation

NODE = Path(__file__).resolve().parent.parent.parent / "oevm" / "zephyr" / "ShipsBellNodeC6.nsl"

_OIMG = None
def _compile_once():
    global _OIMG
    if _OIMG: return _OIMG
    ok, err = R._build_pathb_once()          # builds /tmp/oevm_img + /tmp/oevm_layout.json
    if not ok: raise RuntimeError(err)
    oimg = "/tmp/shipsbell_c6.oimg"
    comp = subprocess.run(
        [sys.executable, str(TOOLS / "ns_compile_oevm.py"),
         str(NODE), oimg, "--format=oimg", "--layout=/tmp/oevm_layout.json"],
        capture_output=True, text=True)
    if comp.returncode != 0:
        raise RuntimeError("compile failed:\n" + (comp.stderr or comp.stdout))
    _OIMG = oimg
    return oimg

def run_node(seconds, wallclock=None, ntp=None, date=None):
    """Run the node for `seconds` of wall time and answer its console. OEVM_NO_NET makes
    the bridge `socket: nil`; OEVM_TIMER_WALLCLOCK lets the watchdog and the countdown
    actually fire; `stdbuf` because the VM's console is stdio and the run ends in a kill."""
    oimg = _compile_once()
    env = dict(os.environ, OEVM_NO_NET="1", OEVM_NODE_ID="404cca44fe60", OEVM_TIMER_WALLCLOCK="1")
    for k in ("OEVM_WALLCLOCK_SECONDS", "OEVM_NTP_SECONDS", "OEVM_NTP_DATE", "OEVM_NO_NTP"):
        env.pop(k, None)
    if wallclock is not None: env["OEVM_WALLCLOCK_SECONDS"] = str(wallclock)
    if ntp is not None: env["OEVM_NTP_SECONDS"] = str(ntp)
    if date is not None: env["OEVM_NTP_DATE"] = str(date)
    try:
        p = subprocess.run(["stdbuf", "-oL", "/tmp/oevm_img", oimg], capture_output=True,
                           text=True, timeout=seconds, env=env)
    except subprocess.TimeoutExpired as e:
        return (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
    raise AssertionError(f"node returned rc={p.returncode} — it must never return\n{p.stdout}\n{p.stderr}")

def test_boot_just_before_a_boundary_seeds_and_holds():
    # 21:59:55, NTP agrees, the alarm is refused (host). The watchdog is armed for
    # 22:00:02 and fires at +7 s into a clock still reading 21:59:55: slot 43 == the
    # marker, so nothing rings and nothing complains. The countdown is armed too (5 s
    # inside the lead) and finds the same frozen second.
    out = run_node(9, wallclock=79195, ntp=79195, date=20261009)
    assert "shipsbell ready, slot 1800 s, lead 5 s, watchdog +2 s, 10/500/1600 ms" in out, out
    assert "porta: bind FAILED — ringing on, ticking on, reporting nothing" in out, out
    assert "ntp ok 20261009 21:59:55" in out, out
    assert "rtc 21:59:55  slot 43" in out, out
    assert "seed slot 43 at 21:59:55 — nothing rung" in out, out
    assert "trigger=WATCHDOG ONLY" in out, out
    assert "bell slot" not in out, out                      # THE POINT: no bell at boot
    assert "watchdog: no alarm" not in out, out             # the watchdog found the slot rung

def test_no_part_and_no_network_is_silent_and_retries():
    # No DS3231, no NTP: say so, strike nothing, arm a retry. Never seed.
    out = run_node(3)
    assert "ntp miss — keeping the RTC" in out, out
    assert "rtc nil (no DS3231, or never set)" in out, out
    assert "seed slot" not in out, out
    assert "bell slot" not in out, out

def test_network_sets_an_unset_part():
    # No wall clock at boot; NTP says 22:00:00 and the write lands, so the node seeds
    # from the corrected part — slot 44, nothing rung.
    out = run_node(3, ntp=79200, date=20261009)
    assert "ntp ok 20261009 22:00:00" in out, out
    assert "rtc 22:00:00  slot 44" in out, out
    assert "seed slot 44 at 22:00:00 — nothing rung" in out, out
    assert "bell slot" not in out, out

if __name__ == "__main__":
    test_boot_just_before_a_boundary_seeds_and_holds()
    test_no_part_and_no_network_is_silent_and_retries()
    test_network_sets_an_unset_part()
    print("ok")
