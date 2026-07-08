#!/usr/bin/env python3
"""Grant proof: with OEVM_DEBUG set, the Supervisor prints `dbg:ready` (it used
the granted mirror); with it unset, only the app output appears. Path A and B."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
NSROOT = os.path.dirname(HERE)
# Task 9a: repointed from the frozen oevm/ns-tests ancestors to the active
# tuvm VM + nsl-tests corpus (matching run_tests_oevm.py and the runner's own
# "clobber ../tuvm/src/solidified_image.c" restore logic). Building Path B
# against the frozen oevm (registry v4) rejects the v5 images the compiler now
# emits after the registry compaction; tuvm is v5 and matches.
OEVM = os.path.join(NSROOT, "..", "tuvm")
APP = os.path.join(NSROOT, "..", "nsl-tests", "04_somns_basic", "41_supervisor_grant.ns")

# Honor the same backend/cflags seam as run_tests_oevm.py so this gate builds
# identically to the conformance suite (default = posix, no extra cflags).
OEVM_TARGET = os.environ.get("OEVM_TARGET", "posix")
OEVM_BACKEND = [f"src/backend/{OEVM_TARGET}/platform_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/serial_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/udp_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/pulse_{OEVM_TARGET}.c"]
OEVM_EXTRA_CFLAGS = os.environ.get("OEVM_EXTRA_CFLAGS", "").split()

# ---------------------------------------------------------------------------
# Path A: compile to solidified_image.c
# ---------------------------------------------------------------------------

def build_a():
    # Compile the app to solidified_image.c, then build the Path A runner.
    subprocess.run([sys.executable, os.path.join(HERE, "ns_compile_oevm.py"), APP,
                    os.path.join(OEVM, "src", "solidified_image.c")],
                   cwd=NSROOT, check=True)
    subprocess.run(["gcc", "-Wall", "-Wextra", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE",
                    "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c",
                    "src/serial.c",
                    "src/udp.c", "src/mirror.c", *OEVM_BACKEND,
                    "src/solidified_image.c", "src/oevm_main.c",
                    "-o", "/tmp/oevm_grant", *OEVM_EXTRA_CFLAGS, "-lm"], cwd=OEVM, check=True)

def run_a(debug):
    env = dict(os.environ)
    if debug: env["OEVM_DEBUG"] = "1"
    else: env.pop("OEVM_DEBUG", None)
    return subprocess.run(["/tmp/oevm_grant"], cwd=OEVM, env=env,
                          capture_output=True, text=True).stdout

# ---------------------------------------------------------------------------
# Path B: compile to .oimg, build image VM, run with/without OEVM_DEBUG
# ---------------------------------------------------------------------------

def build_b():
    # Step 1: build dump_layout tool and produce layout.json (matches
    # run_tests_oevm.py _build_pathb_once, lines ~55-72).
    subprocess.run(
        ["gcc", "-std=c99", "-Isrc", "tools/dump_layout.c", "-o", "/tmp/oevm_dump_layout"],
        cwd=OEVM, check=True)
    layout_json = subprocess.run(
        ["/tmp/oevm_dump_layout"], capture_output=True, text=True, check=True).stdout
    with open("/tmp/oevm_layout.json", "w") as f:
        f.write(layout_json)

    # Step 2: compile the app to .oimg (matches run_test_file_oimg lines ~297-304).
    subprocess.run(
        [sys.executable, os.path.join(HERE, "ns_compile_oevm.py"),
         APP, "/tmp/supervisor_grant.oimg",
         "--format=oimg", "--layout=/tmp/oevm_layout.json"],
        cwd=NSROOT, check=True)

    # Step 3: build the image VM binary (matches _build_pathb_once lines ~78-91).
    subprocess.run(
        ["gcc", "-Wall", "-std=c99", "-Isrc",
         "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c",
         "src/serial.c",
         "src/udp.c", "src/mirror.c",
         *OEVM_BACKEND,
         "src/oimg_loader.c", "src/kernel_boot.c",
         "oevm_image_main.c",
         "-o", "/tmp/oevm_img_grant", *OEVM_EXTRA_CFLAGS, "-lm"],
        cwd=OEVM, check=True)

def run_b(debug):
    env = dict(os.environ)
    if debug: env["OEVM_DEBUG"] = "1"
    else: env.pop("OEVM_DEBUG", None)
    return subprocess.run(["/tmp/oevm_img_grant", "/tmp/supervisor_grant.oimg"],
                          cwd=OEVM, env=env,
                          capture_output=True, text=True).stdout

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # --- Path A ---
    build_a()
    on_a, off_a = run_a(True), run_a(False)
    assert "dbg:ready" in on_a, f"expected dbg:ready with OEVM_DEBUG=1, got:\n{on_a}"
    assert "app-ran" in on_a, f"app did not run under supervisor:\n{on_a}"
    assert "dbg:ready" not in off_a, f"dbg:ready leaked with gate off:\n{off_a}"
    assert "app-ran" in off_a, f"app did not run with gate off:\n{off_a}"
    print("test_supervisor_grant: PASSED (Path A)")

    # --- Path B ---
    build_b()
    on_b, off_b = run_b(True), run_b(False)
    assert "dbg:ready" in on_b, f"[B] expected dbg:ready with OEVM_DEBUG=1, got:\n{on_b}"
    assert "app-ran" in on_b, f"[B] app did not run under supervisor:\n{on_b}"
    assert "dbg:ready" not in off_b, f"[B] dbg:ready leaked with gate off:\n{off_b}"
    assert "app-ran" in off_b, f"[B] app did not run with gate off:\n{off_b}"
    print("test_supervisor_grant: PASSED (Path B)")

if __name__ == "__main__":
    main()
