import os
import sys
import re
import shutil
import subprocess
from pathlib import Path

# Colors for terminal output
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_RESET = "\033[0m"

# The oevm VM (the "bedrock") lives in a sibling repo. Override with $OEVM_DIR.
OEVM_DIR = Path(os.environ.get("OEVM_DIR") or (Path(__file__).resolve().parent.parent.parent / "oevm"))

# Repo root, for gates that live outside nodus/ (review/tests/t0-gates/). Derived from
# THIS file's location, not from OEVM_DIR — OEVM_DIR may be pointed at another tree.
REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent

# OEVM_PATH controls which execution paths are exercised.
#   (unset / "B")  — run Path B only (.oimg / image-VM workflow) — DEFAULT.
#                    Path B builds the image VM once, then per test only
#                    compiles .nsl -> .oimg and runs it. Path A instead did a
#                    full-VM gcc rebuild (incl. the ~13k-line solidified_image.c)
#                    per test — ~3.4s each over ~209 tests, ~12 min of the run.
#                    B is the routine gate; use "A"/"AB" to validate the
#                    solidified_image.c path that actually ships on ESP32.
#   "A"            — run Path A only (solidified_image.c workflow)
#   "AB"           — run both Path A and Path B (full pre-device/release gate)
OEVM_PATH = os.environ.get("OEVM_PATH", "B").upper()
OEVM_EXTRA_CFLAGS = os.environ.get("OEVM_EXTRA_CFLAGS", "").split()
# Per-target backend: the *_backend.h seam impls live in src/backend/$OEVM_TARGET/.
OEVM_TARGET = os.environ.get("OEVM_TARGET", "posix")
OEVM_BACKEND = [f"src/backend/{OEVM_TARGET}/platform_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/serial_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/udp_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/pulse_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/timer_{OEVM_TARGET}.c",
                # tuvm#61: the job-latency microsecond clock seam. Under the DEFAULT
                # virtual clock it scales timer_backend_millis() to microseconds, so
                # advanceMillis: moves the span clock by an exact amount and every
                # bucket assertion in 19_latency stays deterministic.
                f"src/backend/{OEVM_TARGET}/span_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/http_{OEVM_TARGET}.c",
                f"src/backend/{OEVM_TARGET}/sensor_{OEVM_TARGET}.c",
                # rete#25: the image-push seam. Every OEVM_TARGET has a backend —
                # ota_zephyr.c exists and is deliberately inert (no flash map yet).
                f"src/backend/{OEVM_TARGET}/ota_{OEVM_TARGET}.c"]

# OEVM_DUMP_DIR: differential harness knob. When set, every test's raw program stdout
# (the bytes the .nsl program printed — what gets compared against expectations) is
# written to $OEVM_DUMP_DIR/<relpath>.<PATH>.out (one file per test per path A/B).
# Pure observation — does NOT change pass/fail logic or lowering. Cheap when unset.
OEVM_DUMP_DIR = os.environ.get("OEVM_DUMP_DIR")

# OEVM_BUILD_DIR (#252): where Path B artifacts (dump_layout, layout.json, the
# image VM, per-test .oimg files) are written. Default stays /tmp — the paths
# the "run SOLO" rule guards; point it elsewhere and a scratch iterate loop can
# never collide with a concurrent suite run.
OEVM_BUILD_DIR = Path(os.environ.get("OEVM_BUILD_DIR") or "/tmp")
OEVM_BUILD_DIR.mkdir(parents=True, exist_ok=True)

# Module-level cache for Path B one-time builds.
# None = not yet attempted; True = success; False = failed (hard error)
_pathb_layout_built = None   # None | True
_pathb_imgvm_built = None    # None | True
_pathb_build_error = None    # str | None — set on first failure, blocks all Path B


def _build_pathb_once():
    """Build layout.json and the image VM binary (each exactly once per run).
    Sets module globals. Returns (ok: bool, error_msg: str|None)."""
    global _pathb_layout_built, _pathb_imgvm_built, _pathb_build_error

    # Already attempted
    if _pathb_build_error is not None:
        return False, _pathb_build_error
    if _pathb_layout_built and _pathb_imgvm_built:
        return True, None

    oevm_dir = str(OEVM_DIR)

    # Step 1: build dump_layout tool and produce layout.json
    if not _pathb_layout_built:
        proc = subprocess.run(
            ["gcc", "-std=c99", "-Isrc", "tools/dump_layout.c", "-o", str(OEVM_BUILD_DIR / "oevm_dump_layout")],
            cwd=oevm_dir, capture_output=True, text=True
        )
        if proc.returncode != 0:
            _pathb_build_error = f"Path B: failed to build dump_layout:\n{proc.stderr or proc.stdout}"
            return False, _pathb_build_error

        proc2 = subprocess.run(
            [str(OEVM_BUILD_DIR / "oevm_dump_layout")],
            capture_output=True, text=True
        )
        if proc2.returncode != 0:
            _pathb_build_error = f"Path B: dump_layout failed:\n{proc2.stderr or proc2.stdout}"
            return False, _pathb_build_error

        with open(OEVM_BUILD_DIR / "oevm_layout.json", "w") as f:
            f.write(proc2.stdout)

        _pathb_layout_built = True

    # Step 2: build the image VM binary
    if not _pathb_imgvm_built:
        proc = subprocess.run(
            [
                "gcc", "-Wall", "-std=c99", "-Isrc",
                "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
                "src/serial.c",
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c",
                *OEVM_BACKEND,
                "src/oimg_loader.c", "src/kernel_boot.c",
                "oevm_image_main.c",
                "-o", str(OEVM_BUILD_DIR / "oevm_img"),
                *OEVM_EXTRA_CFLAGS,
                "-lm",
            ],
            cwd=oevm_dir, capture_output=True, text=True
        )
        if proc.returncode != 0:
            _pathb_build_error = f"Path B: failed to build image VM:\n{proc.stderr or proc.stdout}"
            return False, _pathb_build_error

        _pathb_imgvm_built = True

    return True, None


# ---- The C unit test registry (rete#275) -----------------------------------
#
# EVERY C unit test is registered below, individually, and this suite is the ONLY
# thing that runs them. `nodus/oevm/tools/run_c_tests.sh` — a second aggregate
# runner that carried its own copy of these link sets — was DELETED on 2026-08-29.
# It had sat red because nothing invoked it (four tests aborting on rete#226's
# prim_id, undetected), which is rete#261's shape exactly; and a test registered
# in TWO places is a test that gets forgotten in one of them (test_observer_core
# had to be added to both).
#
# THE LINK SETS, moved here from that script. They are mutually exclusive in
# exactly one file — solidified_image.c XOR kernel_boot.c, whose static Vtable_*
# definitions overlap in 13 symbols. oimg_loader.c belongs to both. The same
# split is the OEVM_MODEL knob in nodus/oevm/zephyr/CMakeLists.txt.
_ENG_COMMON = ["src/platform.c", "src/serial.c", "src/udp.c", "src/http.c",
               "src/http_core.c", "src/mirror.c", "src/space.c", "src/handles.c",
               "src/ota.c", "src/span.c", *OEVM_BACKEND]
_ENG_CORE = ["src/vm.c", "src/gc.c", "src/dispatch.c", "src/oimg_loader.c"]
ENG_A = [*_ENG_CORE, *_ENG_COMMON, "src/solidified_image.c"]
ENG_B = [*_ENG_CORE, *_ENG_COMMON, "src/kernel_boot.c"]

# Backend-only link sets: these tests stub their own seams and must NOT drag in
# the VM (see each test's own header for why).
_BE = f"src/backend/{OEVM_TARGET}"

# name, link set, extra cflags, skip reason (None = run it).
# A row with a skip reason still PRINTS, because a test that silently stopped
# running is the failure mode this registry exists to prevent.
C_UNIT_TESTS = [
    # --- Model A (solidified_image.c) ---
    ("test_arena",            ENG_A, [], None),
    ("test_float_arena",      ENG_A, [], None),
    ("test_coalesce",         ENG_A, ["-DOEVM_HEAP_SIZE=8192"], None),
    # G3/M2: heap telemetry counters (live_bytes/high_water/largest_free_block/
    # gc_cycles) — the fragmentation early-warning API.
    ("test_heap_stats",       ENG_A, ["-DOEVM_HEAP_SIZE=65536"], None),
    # rete#190 sub-project A: the C-allocation seam's counters.
    ("test_cheap_stats",      ENG_A, ["-DOEVM_HEAP_SIZE=65536"], None),
    # rete#215: the retained panic note's validity rule — a block of garbage must
    # not decode as a report. Retention itself is a TARGET property (tests/target/).
    ("test_reboot_note",      ENG_A, ["-DOEVM_HEAP_SIZE=65536"], None),
    # rete#118: the node id's one formatting rule — golden (header only, links nothing).
    ("test_link_addr",        ENG_A, [], None),
    # rete#190 stage 2: Symbol name bytes inline — 200 Symbols must cost the C heap
    # zero bytes (the leak that crashed three C6 boards), and the bytes must be a GC LEAF.
    ("test_symbol_inline",    ENG_A, ["-DOEVM_HEAP_SIZE=65536"], None),
    ("test_method_cache",     ENG_A, ["-DOEVM_CACHE_STATS"], None),
    # oevm#14 regression proof.
    ("test_debugger",         ENG_A, [], None),
    # rete#165: the method-profile gate.
    ("test_mprof_gate",       ENG_A, ["-DOEVM_METHOD_PROFILE"], None),
    ("test_mprof_alloc",      ENG_A, ["-DOEVM_METHOD_PROFILE"], None),
    # rete#226 — a Symbol carries its own prim id, so nothing looks one up. This
    # replaced the #222 cache gate: there is no table to fill. What is asserted is
    # the invariant that made the cache unnecessary — every Symbol knows its id
    # before anyone asks, and one that reached a send undecided would present as a
    # primitive silently ceasing to be intercepted. rete#275 is that hazard biting
    # four tests that open-coded a Symbol instead of calling symbol_new.
    ("test_symbol_prim_id",   ENG_A, [], None),
    # de-recursify deferred-lift Phase 1: heap Contexts for escaping captures.
    # rete#275: LIVE tests that no runner had EVER invoked — neither this suite nor
    # the deleted script. Both pass.
    ("test_context",          ENG_A, [], None),
    ("test_ctx_ops",          ENG_A, [], None),
    ("test_oevm",             ENG_A, [], None),
    ("test_gc_mark_iterative", ENG_A, ["-DOEVM_HEAP_SIZE=33554432", "-lpthread"], None),
    ("test_float_gc",         ENG_A, [], None),
    # tuvm#15: float-arena OOM under a small embedded malloc pool. --wrap=malloc caps
    # slab allocations below the BUDGET (so the budget trigger never fires), proving
    # float_alloc reclaims-on-malloc-failure instead of aborting.
    ("test_float_oom",        ENG_A, ["-DOEVM_FLOAT_ARENA_SLAB_CELLS=512",
                                      "-DOEVM_FLOAT_ARENA_SLAB_BUDGET=8",
                                      "-Wl,--wrap=malloc"], None),
    # rete#190: the seam counts a failed allocation and returns NULL, never aborts.
    ("test_cheap_oom",        ENG_A, ["-Wl,--wrap=malloc"], None),
    ("test_gc_trigger",       ENG_A, ["-DOEVM_HEAP_SIZE=65536"], None),
    # oevm#36: the vestigial access-control subsystem.
    ("test_access",           ENG_A, [], None),
    ("test_enclosures",       ENG_A, [], None),
    # perf/op-cmp-at: hand-assembled fast-path opcodes (OP_LT..OP_GE, OP_AT/OP_AT_PUT).
    ("test_op_fast",          ENG_A, [], None),

    # --- Model B (kernel_boot.c) ---
    ("test_int_compare",      ENG_B, [], None),
    ("test_millis",           ENG_B, [], None),
    ("test_platform",         ENG_B, [], None),
    # Milestone B Task 3: RawBytes leaf storage.
    ("test_raw_bytes",        ENG_B, [], None),
    # Milestone C Task 1: the channel-space C substrate. Small capacity overrides
    # make the boundary-fill/overflow properties fast and deterministic.
    ("test_space",            ENG_B, ["-DOEVM_SPACE_MAX_CHANNELS=8",
                                      "-DOEVM_SPACE_QUEUE_DEPTH=4"], None),
    # porta space-sync (plan 2): the per-space monotonic counter (design D5), the
    # per-channel last_seq, and the prefix-scoped changed-since scan.
    ("test_space_seq",        ENG_B, ["-DOEVM_SPACE_MAX_CHANNELS=16",
                                      "-DOEVM_SPACE_QUEUE_DEPTH=8"], None),
    # M6 C-boundary handle table: JNI-global-ref-style GC roots for C code.
    ("test_handles",          ENG_B, ["-DOEVM_HANDLE_TABLE_MAX=4"], None),
    # The ISR->channel pulse seam, end-to-end through the handle table and the
    # space queue. NOTE: this is the POSIX ring — the target seam is not covered
    # here, which is what tests/target/ exists for.
    ("test_pulse",            ENG_B, ["-DOEVM_PULSE_MAILBOX_DEPTH=4"], None),
    # Rung 3 Task 2: the CBOR integer leaf primitive `cborEncodeInt:` (prim 97).
    ("test_cbor",             ENG_B, [], None),
    ("test_kernel_boot",      ENG_B, [], None),

    # --- Backend-only: link ONLY the posix seam file, per each test's header ---
    ("test_serial",           [f"{_BE}/serial_{OEVM_TARGET}.c"], [], None),
    ("test_udp",              [f"{_BE}/udp_{OEVM_TARGET}.c"], [], None),
    # tuvm#61: http_core.c stamps the two C-owned spans, so span.c + the posix
    # microsecond clock join the link set. The test supplies its own
    # timer_backend_millis stub.
    ("test_http",             ["src/http_core.c", f"{_BE}/http_{OEVM_TARGET}.c",
                               "src/span.c", f"{_BE}/span_{OEVM_TARGET}.c"], [], None),
    # tuvm#61: job-latency span slots. span.c has no VM dependencies at all, which
    # is the point — any context may bump a slot.
    ("test_span",             ["src/span.c", f"{_BE}/span_{OEVM_TARGET}.c"], [], None),
    # rete#25: the C ota seam. ota.c depends on nothing but its two seams, BOTH
    # stubbed in-file, so the block-fetch loop, the resume arithmetic and the
    # file-local SHA-256 are proven without a socket, a filesystem or a porta.
    ("ota_seam_test",         ["src/ota.c", f"{_BE}/platform_{OEVM_TARGET}.c"], [], None),

    # --- Header-only: no link set at all (offsetof / pure-decision asserts) ---
    ("test_frame_abi",        [], [], None),
    ("test_layout",           [], [], None),
    # rete#215/#269 — THE WEDGE VERDICT, on the host. The decision lived only in
    # zephyr/src/observer.c, which no host gate compiles, so #269 found it rebooting
    # a HEALTHY node from a board and every earlier bug in it cost a board cycle too.
    # It sweeps 30 015 (delivered tick, empty window, sampler) triples for aliasing
    # and carries the pre-#269 predicate in-file, requiring THAT to false-fire — so
    # this gate has teeth rather than being a tautology written after the fix.
    ("test_observer_core",    [], ["-Wextra"], None),

    # --- Pre-existing breakage, excluded rather than silently dropped ---
    ("test_solidified",       ENG_A, [],
     "stale per-test fixture: hard-codes CounterVtable/Symbol_increment from an old "
     "per-test fixture the current generated image no longer provides; tracked in "
     "nodus/oevm/docs/archive/ROADMAP.md P2 for a dedicated fixture"),
    ("test_oimg",             ENG_A, [],
     "needs stub globals for gc.c's grown trace surface: designed to link gc.c "
     "standalone via its own stub globals, but gc.c now also requires StringVtable/"
     "StackVtable/ContextVtable/g_active_stack/debugger_hook/method_cache_flush/"
     "g_boot_inflight_receiver, which the test's stub block predates"),
]


def run_c_unit_tests(oevm_dir):
    """Build + run every C unit test in C_UNIT_TESTS.

    rete#275 — THIS IS THE ONLY C UNIT TEST RUNNER. It replaced
    nodus/oevm/tools/run_c_tests.sh, which nothing invoked and which was therefore
    red without anyone learning it (rete#261's shape). Adding a C test is now one
    row above, in one file.

    EVERY test runs and every failure is reported: this deliberately does NOT stop
    at the first one. The old shell script's `fail=1` accumulator did the same, and
    the four aborts rete#275 fixed were only visible because of it — a short-circuit
    would have shown one and hidden three.
    """
    # OEVM_BUILD=asan|ubsan — sanitizer coverage for the ISR-adjacent C (mailbox
    # ring, handle table). Carried over VERBATIM from the deleted run_c_tests.sh,
    # because review/tests/t0-gates/sanitizers.md makes it standing policy: "the C
    # unit tests run under the same two configs". Deleting the script was allowed
    # to remove a MECHANISM; it was not allowed to remove this CAPABILITY (D8).
    # RESOLVED 2026-08-29 (rete#277) in favour of the DOC. sanitizers.md said
    # -fno-sanitize-recover=all; the deleted script always passed =undefined, and
    # this registry preserved the script. Measured before changing it: with
    # -fsanitize=undefined as the only enabled sanitizer the two spellings are
    # IDENTICAL (a signed-overflow probe exits 1 under both, 0 with recovery), so
    # this is a no-op today. It is =all now because that is the behaviour the
    # policy asks for -- "any UB is a hard failure" -- and the two only diverge the
    # day someone enables a sanitizer outside the `undefined` group, which is
    # exactly when a silently recovering check would be worst. Two documents of
    # record disagreed; they agree now, and the one that moved was the code.
    sanitize = os.environ.get("OEVM_BUILD", "")
    if sanitize == "asan":
        san_flags = ["-fsanitize=address", "-fno-omit-frame-pointer", "-g"]
    elif sanitize == "ubsan":
        san_flags = ["-fsanitize=undefined", "-fno-sanitize-recover=all", "-g"]
    elif sanitize == "":
        san_flags = []
    else:
        print(f"{COLOR_RED}C unit tests: unknown OEVM_BUILD={sanitize!r} "
              f"(want asan|ubsan){COLOR_RESET}")
        return False

    failures, skips, passes = [], [], 0
    for name, link, cflags, skip in C_UNIT_TESTS:
        if skip:
            print(f"{COLOR_YELLOW}C unit test {name}: SKIP{COLOR_RESET} (pre-existing: {skip})")
            skips.append(name)
            continue
        out = f"/tmp/oevm_ct_{name}"
        # Order mirrors the link line in each test's own header: the test TU first,
        # then its link set, then per-test flags (so -Wl,--wrap lands after the
        # objects it rewrites), then -lm for anything that pulled in the VM.
        cmd = ["gcc", "-Wall", "-std=c99", *san_flags, "-Isrc",
               f"tests/{name}.c", *link, *cflags]
        if link:
            cmd.append("-lm")
        cmd += ["-o", out]
        build = subprocess.run(cmd, cwd=str(oevm_dir), capture_output=True, text=True)
        if build.returncode != 0:
            print(f"{COLOR_RED}C unit test {name}: BUILD FAILED{COLOR_RESET}\n" + build.stderr)
            failures.append(name)
            continue
        try:
            run = subprocess.run([out], cwd=str(oevm_dir), capture_output=True,
                                 text=True, timeout=300)
        except subprocess.TimeoutExpired:
            print(f"{COLOR_RED}C unit test {name}: TIMEOUT (300s){COLOR_RESET}")
            failures.append(name)
            continue
        if run.returncode != 0:
            print(f"{COLOR_RED}C unit test {name}: FAILED{COLOR_RESET}\n" + run.stdout + run.stderr)
            failures.append(name)
            continue
        passes += 1
    if failures:
        print(f"{COLOR_RED}C unit tests: {len(failures)} FAILED{COLOR_RESET} "
              f"({', '.join(failures)}) — {passes} passed, {len(skips)} skipped")
    else:
        print(f"{COLOR_GREEN}C unit tests: {passes} passed{COLOR_RESET}, "
              f"{len(skips)} skipped, 0 failed")
    return not failures


def run_boot_gc_asan_gate() -> bool:
    """Regression gate for oevm#21 (boot-time GC roots incomplete).

    During run_interpreter's lazy exec-context bootstrap (a Stack object +
    current_frame, no Fiber wrapper — Milestone A Task 6), the boot ENTRY INSTANCE
    (the `self` passed to the first send, e.g. Kernel>>init_platform:) is live but
    held only in a C local — rooted solely by the conservative C-stack scan. ASan's
    fake-stack (detect_stack_use_after_return=1, the ASan DEFAULT) relocates that
    local off the machine stack, so under OEVM_GC_STRESS=1 a collection during the
    bootstrap sweeps the entry instance and reuses its slot for g_active_stack —
    after which `self` aliases the Stack object and the entry method's slot-store
    corrupts the live stack frame (SEGV in scan_vm_stack on the next GC).

    Build a tiny appliance WITH AddressSanitizer (fake-stack ON) under GC_STRESS and
    require the expected output. Requires the entry instance to be a PRECISE root.
    Skips cleanly (PASS) if the compiler lacks -fsanitize=address support."""
    src = Path(__file__).parent.parent.parent / "tests" / "04_somns_basic" / "01_method_call.nsl"
    if not src.exists():
        print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} (missing {src})")
        return False
    tmp_c = OEVM_DIR / "_boot_gc_asan.c"
    runner = OEVM_DIR / "oevm_boot_gc_asan"
    try:
        comp = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "ns_compile_oevm.py"),
             str(src), str(tmp_c)], capture_output=True, text=True)
        if comp.returncode != 0:
            print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} (compile)")
            print("  " + (comp.stderr or comp.stdout).strip()[-500:])
            return False
        build = subprocess.run(
            ["gcc", "-g", "-fsanitize=address", "-Wall", "-std=c99", "-Isrc", "-DOEVM_APPLIANCE",
             "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
             "src/serial.c",
             "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c", *OEVM_BACKEND,
             str(tmp_c), "src/oevm_main.c", "-o", str(runner), "-lm"],
            cwd=str(OEVM_DIR), capture_output=True, text=True)
        if build.returncode != 0:
            # No ASan toolchain available — skip rather than fail the suite.
            print(f"{COLOR_YELLOW}boot-gc-asan: SKIP{COLOR_RESET} (no -fsanitize=address)")
            return True
        # detect_stack_use_after_return defaults ON; set it explicitly so the gate
        # reproduces the fake-stack condition regardless of any inherited ASAN_OPTIONS.
        env = {**os.environ, "OEVM_GC_STRESS": "1",
               "ASAN_OPTIONS": "detect_stack_use_after_return=1"}
        try:
            run = subprocess.run([str(runner)], cwd=str(OEVM_DIR),
                                 capture_output=True, text=True, timeout=120, env=env)
        except subprocess.TimeoutExpired:
            print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} (timeout)")
            return False
        actual = [l.strip() for l in run.stdout.strip().splitlines() if l.strip()]
        if run.returncode != 0 or actual != ["42"]:
            print(f"{COLOR_RED}boot-gc-asan: FAIL{COLOR_RESET} "
                  f"(exit={run.returncode}, output={actual})")
            print("  " + (run.stderr or "").strip()[-500:])
            return False
        print(f"{COLOR_GREEN}boot-gc-asan: PASSED{COLOR_RESET}")
        return True
    finally:
        for p in (tmp_c, runner):
            try:
                p.unlink()
            except FileNotFoundError:
                pass


def run_pytest_gate(label: str, rel_dir: str, ignore=()) -> str:
    """Run a pytest suite living outside nodus/nsl as a subprocess gate.

    #165: the pytest suites under `nodus/oevm/tools/` and
    `nodus/oevm/zephyr/tools/console/` were referenced by nothing — no gate, no
    CI, no script. A tool could break with all three CLAUDE.md gates green. A test that is never run is worse than
    no test, because the suite still reads green and the coverage is now
    believed rather than merely absent.

    cwd is the suite's own directory: these files import their siblings by bare
    name (`import logstore`, `import collect`), which is
    what makes them runnable by hand from where they live.

    Returns "PASS", "SKIP" (pytest is not installed — a real environment gap,
    coloured like every other skip so it cannot read as a pass) or "FAIL"."""
    d = REPO_ROOT / rel_dir
    # #252: a single test FILE is a legal target — cwd stays its directory (so
    # sibling bare-name imports keep working) and pytest collects only it. Used
    # for nodus/nsl/tools, where a directory-wide collect would re-run the
    # heavy integration gates (grant, debugger, deepsleep) a second time.
    if d.is_file():
        cwd, target = d.parent, [d.name]
    elif d.is_dir():
        # rete#228: `ignore` names files pytest must NOT collect from a dir
        # target — for suites whose heavy members are run by their own gate
        # (the nsl/tools integration harnesses) and would otherwise run twice.
        # A dir target with ignores beats an explicit file list because a NEW
        # test file is then collected the day it lands, instead of joining the
        # orphans this gate exists to abolish.
        cwd, target = d, [f"--ignore={name}" for name in ignore]
    else:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (no such path: {d})")
        return "FAIL"
    try:
        proc = subprocess.run(
            [sys.executable, "-m", "pytest", "-q", "--no-header", *target],
            cwd=str(cwd), capture_output=True, text=True, timeout=600)
    except Exception as e:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 4 and "No module named pytest" in out:
        print(f"{COLOR_YELLOW}{label}: SKIP{COLOR_RESET} (pytest is not installed)")
        return "SKIP"
    if "No module named pytest" in out:
        print(f"{COLOR_YELLOW}{label}: SKIP{COLOR_RESET} (pytest is not installed)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    # The pass COUNT is printed, not just the word: "0 passed" exits 0 under
    # some pytest configurations, and a gate that cannot tell "everything
    # passed" from "nothing ran" is the shape this whole change exists to fix.
    tail = [ln for ln in out.strip().splitlines()
            if " passed" in ln or "no tests ran" in ln]
    if not tail or "no tests ran" in tail[-1]:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (exit 0, but pytest "
              f"collected nothing — a green gate over an empty suite)")
        return "FAIL"
    print(f"{COLOR_GREEN}{label}: PASSED{COLOR_RESET} ({tail[-1].strip()})")
    return "PASS"


def run_integration_gate(label: str, script_name: str) -> bool:
    """Run a tools/<script_name> integration harness as a subprocess gate.

    These scripts assume cwd == the ns-oevm ROOT (this tools/ dir's parent), and
    they clobber ../oevm/src/solidified_image.c during their builds (the caller is
    responsible for restoring it afterward). A non-zero exit OR an exception marks
    the gate FAILED without crashing the suite runner; on failure we print enough
    captured output to diagnose."""
    nsroot = Path(__file__).resolve().parent.parent
    script = nsroot / "tools" / script_name
    try:
        proc = subprocess.run(
            [sys.executable, str(script)],
            cwd=str(nsroot), capture_output=True, text=True, timeout=300)
    except Exception as e:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (runner error: {e})")
        return False
    if proc.returncode != 0:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET}")
        out = (proc.stdout or "") + (proc.stderr or "")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return False
    print(f"{COLOR_GREEN}{label}: PASSED{COLOR_RESET}")
    return True


def run_custos_debug_gate() -> str:
    """Gate `custos debug` against a real VM (rete#339): tools/test_custos_debug.py, the
    person's front end to the in-image debugger, by source line, pause included.

    Needs Go, and answers SKIP without it (exit 77) for porta-cli's reason: the debugger's
    front end is a custos verb, and making the nsl suite hard-fail without a Go toolchain
    would couple the node half to the tools half at the gate. A SKIP here is still a
    finding, not a fixture (#177). Returns "PASS", "SKIP" or "FAIL"."""
    script = Path(__file__).resolve().parent / "test_custos_debug.py"
    if not script.exists():
        print(f"{COLOR_RED}custos-debug: FAIL{COLOR_RESET} (gate script not found at {script})")
        return "FAIL"
    try:
        proc = subprocess.run([sys.executable, str(script)], cwd=str(script.parent.parent),
                              capture_output=True, text=True, timeout=300)
    except Exception as e:
        print(f"{COLOR_RED}custos-debug: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    if proc.returncode == 77:
        print(f"custos-debug: {COLOR_YELLOW}SKIP{COLOR_RESET} (no go toolchain — "
              f"custos debug is the debugger's front end)")
        return "SKIP"
    if proc.returncode != 0:
        out = ((proc.stdout or "") + (proc.stderr or "")).strip()
        print(f"{COLOR_RED}custos-debug: FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}custos-debug: PASSED{COLOR_RESET}")
    return "PASS"


def run_tutorial_gate(page_name: str, label: str, needs: tuple = ()) -> str:
    """Gate a TUTORIAL (#79): docs/tutorials/<page> is executed, not trusted.

    porta/docs/GETTING-STARTED.md walked an outsider through verbs that had been deleted
    for weeks, and nothing said so — a tutorial is the one document nobody who knows the
    system re-reads, so it rots first and silently. docs/tutorials/gate.sh extracts every
    ```bash gate block from the page and runs them in order in a fresh directory (its own
    canary first: a failing block must fail). Pages use run-one.py on Path B only, so they
    share this run's OEVM_BUILD_DIR artefacts and clobber nothing tracked.

    A new tutorial is one more call to this function. Returns "PASS" or "FAIL" — a missing
    page is a FAILURE, never a skip.

    `needs` is the NARROW exception: executables the page cannot run without, checked by
    name so the skip states its own cause. 03-porta.md and 04-debugger.md use it, for `go`
    (porta, and `custos debug`). Making the nsl suite hard-fail without a Go toolchain
    would couple the halves of this repo at the gate — the peers are coupled through
    `protocol/` and nowhere else — and
    tests/porta_sync_host.sh set the precedent for this exact dependency. It stays narrow
    on purpose: a SKIP here is a finding, not a fixture (#177)."""
    root = OEVM_DIR.parent.parent
    script = root / "docs" / "tutorials" / "gate.sh"
    page = root / "docs" / "tutorials" / page_name
    if not script.exists() or not page.exists():
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (missing {script if not script.exists() else page})")
        return "FAIL"
    for tool in needs:
        if shutil.which(tool) is None:
            print(f"{COLOR_YELLOW}{label}: SKIP{COLOR_RESET} ({tool} is not on PATH — "
                  f"{page_name} builds a Go peer)")
            return "SKIP"
    try:
        proc = subprocess.run(["bash", str(script), str(page)],
                              cwd=str(root), capture_output=True, text=True, timeout=600)
    except Exception as e:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = ((proc.stdout or "") + (proc.stderr or "")).strip()
    if proc.returncode != 0:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}{label}: PASSED{COLOR_RESET}")
    return "PASS"


def run_tutorial_board_gate(page_name: str = "02-board.md",
                            label: str = "tutorial-board") -> str:
    """Gate the ONE checkable fact on the board tutorial (#79).

    docs/tutorials/02-board.md is prose and says so in its own banner: its walkthrough needs
    a devkit, a Zephyr toolchain and a mesh, so unlike 01-host.md and 03-porta.md it carries
    no ```bash gate blocks and cannot. A gate that degraded to a SKIP without hardware is how
    a gate reports success for a run that never happened, which is why tests/target/run.sh
    refuses to be part of this suite either.

    But ONE claim on that page is a fact about this repository rather than about a board:
    §7's table of target scenarios. It is also the claim most likely to go stale, and to do
    it silently — a twelfth .env lands and the page goes on confidently listing eleven. This
    suite already derives the scenario COUNT from that same glob for its `target gates:`
    line, so without this check the page can contradict the suite's own output and nobody
    finds out.

    NAMES ONLY, deliberately. Not the descriptions: a stale description is a judgement call,
    a missing scenario is a bug, and a gate that tries to score prose is a gate that gets
    disabled. This does not make the page executed and does not soften its banner.

    A row counts only when its first cell is entirely one code span, so the page's other
    tables cannot contribute. If the table is renamed or reformatted the parse yields the
    empty set and this FAILS naming every scenario as undocumented — vacancy must not read
    as agreement (#216's shape). Returns "PASS" or "FAIL"; a missing page is a FAILURE."""
    root = OEVM_DIR.parent.parent
    page = root / "docs" / "tutorials" / page_name
    target_dir = OEVM_DIR / "tests" / "target"
    if not page.exists() or not target_dir.is_dir():
        missing = page if not page.exists() else target_dir
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} (missing {missing})")
        return "FAIL"

    on_disk = {f.stem for f in target_dir.glob("*.env")}
    documented = {m.group(1) for line in page.read_text().splitlines()
                  if (m := re.match(r"^\|\s*`([a-z0-9-]+)`\s*\|", line))}

    undocumented = sorted(on_disk - documented)
    phantom = sorted(documented - on_disk)
    if undocumented or phantom:
        print(f"{COLOR_RED}{label}: FAIL{COLOR_RESET} "
              f"({page_name} \u00a77 disagrees with {target_dir.name}/*.env)")
        for name in undocumented:
            print(f"  scenario on disk but NOT on the page: {name}")
        for name in phantom:
            print(f"  named on the page but NO manifest:    {name}")
        return "FAIL"
    print(f"{COLOR_GREEN}{label}: PASSED{COLOR_RESET} ({len(on_disk)} scenarios)")
    return "PASS"


def run_porta_cli_gate() -> str:
    """Gate porta's CLI REFERENCE against the binary's own account of itself (#279).

    porta/docs/CLI.md documented `scan`, `ping`, `device set` and `container install` for
    four weeks after the commit that deleted them. A reference page rots the way a tutorial
    does — read by outsiders, re-read by nobody — but its claims are not commands anyone
    runs, so gate.sh's answer (execute the page) is not available here. cobra's is: the
    binary already knows every command and every flag it has.

    porta/docs/cli-gate.sh builds porta, walks `--help` recursively, and compares NAMES in
    both directions: a command or flag the binary declares must be on the page, and a
    `--flag` the page shows anywhere — transcripts included — must exist. Not the prose: a
    stale sentence is a judgement call and a gate that grades English gets disabled. It
    falsifies itself twice before clearing anything (an empty page must FAIL, and the real
    page plus one invented flag must FAIL), because vacancy must not read as agreement.

    Needs Go, and answers SKIP without it (exit 77) for tutorial-porta's reason: making the
    nsl suite hard-fail without a Go toolchain would couple the two halves of this repo at
    the gate, and they are coupled over the wire and nowhere else. A SKIP here is still a
    finding, not a fixture (#177). Returns "PASS", "SKIP" or "FAIL"; a missing script is a
    FAILURE, never a skip."""
    root = OEVM_DIR.parent.parent
    script = root / "porta" / "docs" / "cli-gate.sh"
    if not script.exists():
        print(f"{COLOR_RED}porta-cli: FAIL{COLOR_RESET} (gate script not found at {script})")
        return "FAIL"
    try:
        proc = subprocess.run(["bash", str(script)],
                              cwd=str(root), capture_output=True, text=True, timeout=300)
    except Exception as e:
        print(f"{COLOR_RED}porta-cli: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = ((proc.stdout or "") + (proc.stderr or "")).strip()
    if proc.returncode == 77:
        print(f"porta-cli: {COLOR_YELLOW}SKIP{COLOR_RESET} (no go toolchain — "
              f"CLI.md is checked against a binary this host cannot build)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}porta-cli: FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.splitlines()[-30:]))
        return "FAIL"
    detail = out.splitlines()[-1].replace("porta-cli: PASS", "").strip() if out else ""
    print(f"{COLOR_GREEN}porta-cli: PASSED{COLOR_RESET} {detail}")
    return "PASS"


def run_cmake_parse_gate() -> str:
    """Gate the BUILD FILES, which until rete#226 nothing on the host read at all.

    #226 deleted both PRIM_CACHE_SIZE blocks from zephyr/CMakeLists.txt by removing each
    `if(...)` and its body but not the `endif()`. The file stopped parsing — and stayed
    broken for a day while this suite printed 262/0 with every gate above PASS, because a
    CMakeLists is code that only a target build compiles, and a target build needs a board.

    nodus/oevm/tests/cmake_gate.sh parses every tracked listfile inside an `if(FALSE)`
    wrapper, so the body is parsed and never executed: no toolchain, no west venv, no
    ZEPHYR_BASE, ~12 ms a file. It runs its own canary first (a file with an orphan endif()
    MUST be reported broken) so a gate that has gone blind fails instead of clearing
    everything.

    It is a PARSE check and nothing more — an unknown Kconfig symbol, a target_sources
    naming a missing file, or a dependency Zephyr discards silently still need a real
    configure. Returns "PASS", "SKIP" (exit 77 — no cmake on this host) or "FAIL"."""
    script = OEVM_DIR / "tests" / "cmake_gate.sh"
    if not script.exists():
        # An in-repo gate that cannot be found is a FAIL, never a SKIP (same rule as
        # footprint above): a missing gate must not read as a passing one.
        print(f"{COLOR_RED}cmake-parse: FAIL{COLOR_RESET} (gate script not found at {script})")
        return "FAIL"
    try:
        proc = subprocess.run(["bash", str(script)],
                              cwd=str(OEVM_DIR), capture_output=True, text=True, timeout=120)
    except Exception as e:
        print(f"{COLOR_RED}cmake-parse: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = ((proc.stdout or "") + (proc.stderr or "")).strip()
    if proc.returncode == 77:
        print(f"cmake-parse: {COLOR_YELLOW}SKIP{COLOR_RESET} (no cmake on this host)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}cmake-parse: FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}{out.splitlines()[-1]}{COLOR_RESET}" if out else
          f"{COLOR_GREEN}cmake-parse: PASSED{COLOR_RESET}")
    return "PASS"


def run_portability_gate() -> str:
    """Gate the 32-bit (-m32) serialize+load+run path (oevm/tests/test_portability_32.sh).

    The .oimg header descriptor block is word-strided (W = oop_size), so the
    serializer/loader contract only holds if it is exercised at BOTH word sizes;
    a 64-bit-only suite cannot catch a W=4 layout divergence. Returns "PASS",
    "SKIP" (exit 77 — no gcc-multilib on this host), or "FAIL"."""
    script = OEVM_DIR / "tests" / "test_portability_32.sh"
    try:
        proc = subprocess.run(["bash", str(script)],
                              cwd=str(OEVM_DIR), capture_output=True, text=True, timeout=300)
    except Exception as e:
        print(f"{COLOR_RED}portability-32: FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    if proc.returncode == 77:
        print(f"portability-32: SKIP (no gcc-multilib)")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}portability-32: FAIL{COLOR_RESET}")
        out = (proc.stdout or "") + (proc.stderr or "")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}portability-32: PASSED{COLOR_RESET}")
    return "PASS"


def run_footprint_gate() -> str:
    """Gate T0.3 — the footprint ratchet (review/tests/t0-gates/ratchet.sh).

    Runs the T0.3 gate itself rather than footprint.sh (#143). footprint.sh now
    only MEASURES and BANKS; ratchet.sh is what applies the spec's +2% tolerance
    (review/test-suites-spec.md §T0.3) to host text, mcu text and mcu data+bss —
    RAM is a gate failure like text (addendum A2 / tuvm#5). Calling footprint.sh
    here meant the suite enforced a zero tolerance the spec never asked for while
    the actual T0.3 script had no caller at all and had never run.

    Returns "PASS", "SKIP" (exit 77 — no arm-zephyr-eabi, so the MCU half did not
    run), or "FAIL"."""
    script = REPO_ROOT / "review" / "tests" / "t0-gates" / "ratchet.sh"
    if not script.is_file():
        # An in-repo gate that cannot be found is a FAIL, never a SKIP: a missing
        # path is the restructure hazard from #143/#110, not an absent toolchain.
        print(f"{COLOR_RED}footprint(T0.3): FAIL{COLOR_RESET} (gate script not found at {script})")
        return "FAIL"
    try:
        proc = subprocess.run(
            ["bash", str(script)],
            cwd=str(REPO_ROOT), capture_output=True, text=True, timeout=600,
            # ratchet.sh resolves the tree it measures from its own location; honour
            # an OEVM_DIR override so a branch worktree still measures itself.
            env={**os.environ, "OEVM_DIR": str(OEVM_DIR)})
    except Exception as e:
        print(f"{COLOR_RED}footprint(T0.3): FAIL{COLOR_RESET} (runner error: {e})")
        return "FAIL"
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode == 77:
        # Yellow, and it says what did NOT run: an unrun gate must not read as a
        # passing one (tuvm#31 — this gate reported SKIP for weeks and scrolled past).
        print(f"{COLOR_YELLOW}footprint(T0.3): SKIP{COLOR_RESET} — the MCU half did NOT run "
              f"(no arm-zephyr-eabi on PATH; source ns-zephyr/tools/env.sh). Host half only.")
        return "SKIP"
    if proc.returncode != 0:
        print(f"{COLOR_RED}footprint(T0.3): FAIL{COLOR_RESET}")
        print("  " + "\n  ".join(out.strip().splitlines()[-30:]))
        return "FAIL"
    print(f"{COLOR_GREEN}footprint(T0.3): PASSED{COLOR_RESET}")
    for line in out.strip().splitlines():
        if line.startswith("ratchet ok:"):
            print(f"  {line}")
    return "PASS"


def parse_test_metadata(file_path: Path):
    content = file_path.read_text(encoding="utf-8")

    # Newspeak comments are (* ... *)
    comment_pattern = re.compile(r"\(\*(.*?)\*\)", re.DOTALL)
    match = comment_pattern.search(content)
    if not match:
        return {}

    comment_text = match.group(1)
    metadata = {
        "expect": [],
        "expect_compile_fail": None,
        "expect_runtime_fail": None,
        "skip_berry": False,
        "skip_oevm": False,
        "env": {},
    }

    lines = comment_text.splitlines()
    in_expect = False

    for line in lines:
        line = line.strip()
        if not line:
            continue

        if line.startswith("expect:"):
            in_expect = True
            rest = line[len("expect:"):].strip()
            if rest:
                metadata["expect"].append(rest)
        elif line.startswith("expect-compile-fail-line:"):
            # rete#139: pin WHICH line the compiler blames, now that rete#247
            # made CompileError carry file:line:col. Optional beside the
            # substring header; asserted as ".nsl:<N>:" in the diagnostic.
            metadata["expect_compile_fail_line"] = int(
                line[len("expect-compile-fail-line:"):].strip())
        elif line.startswith("expect-compile-fail:"):
            in_expect = False
            metadata["expect_compile_fail"] = line[len("expect-compile-fail:"):].strip()
        elif line.startswith("expect-runtime-fail:"):
            in_expect = False
            metadata["expect_runtime_fail"] = line[len("expect-runtime-fail:"):].strip()
        elif line.startswith("skip-berry:"):
            in_expect = False
            metadata["skip_berry"] = line[len("skip-berry:"):].strip().lower() == "true"
        elif line.startswith("skip-oevm:"):
            in_expect = False
            metadata["skip_oevm"] = line[len("skip-oevm:"):].strip().lower() == "true"
        elif line.startswith("skip-soup:"):
            in_expect = False
        elif line.startswith("env:"):
            # `env: KEY=VALUE` — one variable per line, applied ONLY to this
            # test's VM process (tuvm#88). Some seams are gated by getenv and
            # are otherwise untestable: OEVM_NO_DISPLAY makes the host claim it
            # has no panel, which is the only way to reach the
            # #displayUnavailable path from a host test. Setting such a variable
            # globally is not equivalent — it would leak into every other test
            # in the suite (14_display/01 and /02 would then assert against a
            # display that reports itself absent), so it has to be per-test.
            in_expect = False
            assignment = line[len("env:"):].strip()
            if "=" in assignment:
                key, _, value = assignment.partition("=")
                metadata["env"][key.strip()] = value.strip()
        elif in_expect:
            metadata["expect"].append(line)

    return metadata

def _no_core_dump():
    """preexec_fn for a VM run that is EXPECTED to abort (`expect-runtime-fail:`): no
    core file, and so no desktop crash notification, for a failure the test asked for.
    A run that aborts unexpectedly still dumps core. 1, not 0: with a piped
    core_pattern (systemd-coredump) the kernel still runs the helper at 0, and skips
    it only at exactly 1."""
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (1, 1))


def _check_runtime_fail(metadata: dict, err_msg: str, stdout: str) -> tuple[bool, str]:
    """Validate an `expect-runtime-fail:` test (Task 10b smallest-extension): the
    diagnostic (stderr, or stdout if stderr is empty) must CONTAIN the header's
    substring, and any preceding `expect:` lines must match the stdout emitted
    before the crash. Previously the harness treated ANY nonzero exit as a PASS
    regardless of the header's substring or preceding output — verified against
    07_no_nlr/04_stack_exhaustion.nsl, which needs both checks to be a meaningful
    proof of controlled (vs. C-level) termination. Returns (ok, detail)."""
    expected_substr = metadata["expect_runtime_fail"]
    if expected_substr not in err_msg:
        return False, (f"Runtime failed as expected, but diagnostic did not contain "
                        f"'{expected_substr}':\n{err_msg}")
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]
    if expected_lines:
        actual_lines = [line.strip() for line in stdout.strip().splitlines() if line.strip()]
        if actual_lines != expected_lines:
            return False, (f"Runtime failed with the expected diagnostic, but preceding "
                            f"output mismatched.\nExpected: {expected_lines}\nActual:   {actual_lines}")
    return True, f"Expected runtime failure: {err_msg.strip()}"


def _check_compile_fail(metadata: dict, err_msg: str) -> tuple[bool, str]:
    """Validate an `expect-compile-fail:` test (final-review smallest-extension,
    mirrors _check_runtime_fail): the compiler's stderr+stdout must CONTAIN the
    header's substring. Previously the harness treated ANY nonzero compile exit
    as a PASS regardless of the header's substring, so a test could declare the
    wrong diagnostic (or the compiler could fail for an unrelated reason) and
    still pass. Returns (ok, detail)."""
    expected_substr = metadata["expect_compile_fail"]
    if expected_substr not in err_msg:
        return False, (f"Compilation failed as expected, but diagnostic did not contain "
                        f"'{expected_substr}':\n{err_msg}")
    expected_line = metadata.get("expect_compile_fail_line")
    if expected_line is not None and f".nsl:{expected_line}:" not in err_msg:
        return False, (f"Compilation failed with the expected diagnostic, but it did not "
                        f"blame line {expected_line} (want '.nsl:{expected_line}:'):\n{err_msg}")
    return True, f"Expected compilation failure: {err_msg.strip()}"


def _dump_stdout(src_path: Path, path_label: str, stdout: str) -> None:
    """Write raw program stdout to $OEVM_DUMP_DIR/<relpath>.<path_label>.out.
    Called only when OEVM_DUMP_DIR is set; the dump does NOT affect pass/fail."""
    if not OEVM_DUMP_DIR:
        return
    test_dir = Path(__file__).parent.parent.parent / "tests"
    try:
        rel = src_path.relative_to(test_dir)
    except ValueError:
        rel = Path(src_path.name)
    out_path = Path(OEVM_DUMP_DIR) / (str(rel) + f".{path_label}.out")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(stdout, encoding="utf-8")


def run_test_file(src_path: Path) -> tuple[str, str]:
    metadata = parse_test_metadata(src_path)

    # Note: skip-berry is an ns-berry limitation; oevm attempts those tests.
    # Use skip-oevm to mark tests oevm genuinely cannot run yet.
    if metadata.get("skip_oevm"):
        return "SKIP", "Skipped (skip-oevm: true)"

    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "SKIP", "No expectations defined."

    # 1. Compile .nsl to src/solidified_image.c
    compiler_script = Path(__file__).parent / "ns_compile_oevm.py"
    target_c = OEVM_DIR / "src" / "solidified_image.c"

    proc_compile = subprocess.run(
        [sys.executable, str(compiler_script), str(src_path), str(target_c)],
        capture_output=True, text=True
    )

    if proc_compile.returncode != 0:
        err_msg = proc_compile.stderr or proc_compile.stdout
        if metadata.get("expect_compile_fail"):
            ok, detail = _check_compile_fail(metadata, err_msg)
            if not ok:
                return "FAIL", detail
            _dump_stdout(src_path, "A", "")
            return "PASS", detail
        return "FAIL", f"Compilation failed:\n{err_msg}"

    if metadata.get("expect_compile_fail"):
        return "FAIL", "Expected compilation failure but compiled successfully."

    # 2. Build the C runner
    oevm_dir = OEVM_DIR
    runner_bin = oevm_dir / "oevm_runner"

    # We clean the previous bin first
    if runner_bin.exists():
        runner_bin.unlink()

    cmd_build = [
        "gcc", "-Wall", "-Wextra", "-std=c99", "-Isrc",
        "-DOEVM_APPLIANCE",
        "src/vm.c", "src/gc.c", "src/dispatch.c", "src/platform.c", "src/space.c", "src/handles.c", "src/span.c",
        "src/serial.c",
        "src/udp.c", "src/http.c", "src/http_core.c", "src/mirror.c", "src/ota.c",
        *OEVM_BACKEND,
        "src/solidified_image.c", "src/oevm_main.c",
        "-o", "oevm_runner",
        *OEVM_EXTRA_CFLAGS,
        "-lm",  # VM float primitives (sqrt) need libm
    ]
    proc_build = subprocess.run(cmd_build, cwd=str(oevm_dir), capture_output=True, text=True)
    if proc_build.returncode != 0:
        return "FAIL", f"C Build failed:\n{proc_build.stderr or proc_build.stdout}"

    # 3. Run the compiled runner (30s timeout guards against infinite loops)
    try:
        proc_run = subprocess.run([str(runner_bin)], capture_output=True, text=True, timeout=30,
                                  env={**os.environ, **metadata.get("env", {})},
                                  preexec_fn=(_no_core_dump if metadata.get("expect_runtime_fail")
                                              else None))
    except subprocess.TimeoutExpired:
        return "FAIL", "Runtime timed out (>30s)"
    if proc_run.returncode != 0:
        err_msg = proc_run.stderr or proc_run.stdout
        if metadata.get("expect_runtime_fail"):
            ok, detail = _check_runtime_fail(metadata, err_msg, proc_run.stdout)
            if not ok:
                return "FAIL", detail
            _dump_stdout(src_path, "A", proc_run.stdout)
            return "PASS", detail
        return "FAIL", f"Runtime crashed:\n{err_msg}"

    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Expected runtime failure but ran successfully."

    # Dump raw stdout for differential harness (no-op when OEVM_DUMP_DIR unset).
    _dump_stdout(src_path, "A", proc_run.stdout)

    # Compare stdout lines
    actual_lines = [line.strip() for line in proc_run.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]

    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected: {expected_lines}\nActual:   {actual_lines}"

    return "PASS", "Output matches expectations."


def run_test_file_oimg(src_path: Path) -> tuple[str, str]:
    """Path B: compile to .oimg and run via the image VM."""
    metadata = parse_test_metadata(src_path)

    if metadata.get("skip_oevm"):
        return "SKIP", "Skipped (skip-oevm: true)"

    if not (metadata.get("expect") or metadata.get("expect_compile_fail") or metadata.get("expect_runtime_fail")):
        return "SKIP", "No expectations defined."

    # Ensure the one-time Path B builds are done
    ok, err = _build_pathb_once()
    if not ok:
        return "FAIL", err

    # Derive a unique name for the temp image file (parent dir + stem avoids
    # collisions when two tests in different directories share the same stem).
    safe_stem = re.sub(r"[^A-Za-z0-9_.-]", "_", src_path.stem)
    safe_parent = re.sub(r"[^A-Za-z0-9_.-]", "_", src_path.parent.name)
    oimg_path = str(OEVM_BUILD_DIR / f"{safe_parent}__{safe_stem}.oimg")

    # 1. Compile .nsl to .oimg
    compiler_script = Path(__file__).parent / "ns_compile_oevm.py"
    proc_compile = subprocess.run(
        [
            sys.executable, str(compiler_script),
            str(src_path), oimg_path,
            "--format=oimg",
            f"--layout={OEVM_BUILD_DIR / 'oevm_layout.json'}",
        ],
        capture_output=True, text=True
    )

    if proc_compile.returncode != 0:
        err_msg = proc_compile.stderr or proc_compile.stdout
        if metadata.get("expect_compile_fail"):
            ok, detail = _check_compile_fail(metadata, err_msg)
            if not ok:
                return "FAIL", detail
            return "PASS", detail
        return "FAIL", f"Compilation failed:\n{err_msg}"

    if metadata.get("expect_compile_fail"):
        return "FAIL", "Expected compilation failure but compiled successfully."

    # 2. Run image VM (30s timeout)
    try:
        proc_run = subprocess.run(
            [str(OEVM_BUILD_DIR / "oevm_img"), oimg_path],
            capture_output=True, text=True, timeout=30,
            env={**os.environ, **metadata.get("env", {})},
            preexec_fn=_no_core_dump if metadata.get("expect_runtime_fail") else None
        )
    except subprocess.TimeoutExpired:
        return "FAIL", "Runtime timed out (>30s)"

    if proc_run.returncode != 0:
        err_msg = proc_run.stderr or proc_run.stdout
        if metadata.get("expect_runtime_fail"):
            ok, detail = _check_runtime_fail(metadata, err_msg, proc_run.stdout)
            if not ok:
                return "FAIL", detail
            _dump_stdout(src_path, "B", proc_run.stdout)
            return "PASS", detail
        return "FAIL", f"Runtime crashed:\n{err_msg}"

    if metadata.get("expect_runtime_fail"):
        return "FAIL", "Expected runtime failure but ran successfully."

    # Dump raw stdout for differential harness (no-op when OEVM_DUMP_DIR unset).
    _dump_stdout(src_path, "B", proc_run.stdout)

    # Compare stdout lines
    actual_lines = [line.strip() for line in proc_run.stdout.strip().splitlines() if line.strip()]
    expected_lines = [line.strip() for line in metadata.get("expect", []) if line.strip()]

    if actual_lines != expected_lines:
        return "FAIL", f"Output mismatch.\nExpected: {expected_lines}\nActual:   {actual_lines}"

    return "PASS", "Output matches expectations."


def run_named_files(args):
    """#252 — single-file mode: run only the named .nsl files under the harness
    contract, skipping suite discovery and the aux gates. Files may live
    anywhere (a scratch program outside nodus/tests gets the same treatment).
    Path B only unless OEVM_PATH says otherwise — Path B is the seconds-long
    iterate loop this mode exists for. Returns the process exit code."""
    mode = os.environ.get("OEVM_PATH") or "B"
    files = []
    for a in args:
        p = Path(a).resolve()
        if not p.is_file():
            print(f"Error: no such file: {a}")
            return 2
        files.append(p)

    passed = failed = skipped = 0
    for f in files:
        print(f"Running {str(f):50} ... ", end="", flush=True)
        status_a, msg_a = ("PASS", "Path A skipped") if "A" not in mode \
            else run_test_file(f)
        status_b, msg_b = ("PASS", "Path B skipped") if "B" not in mode \
            else run_test_file_oimg(f)
        if "SKIP" in (status_a, status_b):
            skipped += 1
            print(f"{COLOR_YELLOW}SKIP{COLOR_RESET} ({msg_a if status_a == 'SKIP' else msg_b})")
        elif status_a == "PASS" and status_b == "PASS":
            passed += 1
            print(f"{COLOR_GREEN}PASS{COLOR_RESET}")
        else:
            failed += 1
            print(f"{COLOR_RED}FAIL{COLOR_RESET}")
            if status_a == "FAIL":
                print(f"  A:FAIL — {msg_a}")
            if status_b == "FAIL":
                print(f"  B:FAIL — {msg_b}")

    print(f"\n  Passed: {passed}  Failed: {failed}  Skipped: {skipped}"
          f"   (single-file mode: no suite gates ran)")
    return 1 if failed else 0


def main():
    if len(sys.argv) > 1:
        sys.exit(run_named_files(sys.argv[1:]))
    test_dir = Path(__file__).parent.parent.parent / "tests"
    if not test_dir.exists():
        print(f"Error: tests directory not found at {test_dir}")
        sys.exit(1)

    # Scan and sort test files
    test_files = sorted(list(test_dir.glob("**/*.nsl")))

    passed = 0
    failed = 0
    skipped = 0

    print(f"Discovered {len(test_files)} Newspeak conformance test files.\n")

    for f in test_files:
        rel_path = f.relative_to(test_dir.parent)
        print(f"Running {str(rel_path):50} ... ", end="", flush=True)

        try:
            # --- Path A ---
            if OEVM_PATH in ("A", "AB"):
                status_a, msg_a = run_test_file(f)
            else:
                # Path B only: synthesise a neutral result so branching below works
                status_a, msg_a = "PASS", "Path A skipped (OEVM_PATH=B)"

            # If Path A says SKIP (or we are A-only and Path A skipped),
            # skip the whole test immediately.
            if status_a == "SKIP":
                skipped += 1
                print(f"{COLOR_YELLOW}SKIP{COLOR_RESET} ({msg_a})")
                continue

            # --- Path B ---
            if OEVM_PATH in ("B", "AB"):
                try:
                    status_b, msg_b = run_test_file_oimg(f)
                except Exception as e:
                    status_b = "FAIL"
                    msg_b = f"Unexpected Path B error: {e}"
            else:
                # Path A only: synthesise a neutral result
                status_b, msg_b = "PASS", "Path B skipped (OEVM_PATH=A)"

            # --- Result combination ---
            # Explicit SKIP arm: if Path B independently returns SKIP (e.g. after
            # a future refactor removes the early-exit above), treat it as skipped
            # rather than letting it fall into the FAIL else-arm.
            if status_b == "SKIP":
                skipped += 1
                print(f"{COLOR_YELLOW}SKIP{COLOR_RESET} (B: {msg_b})")
            elif status_a == "PASS" and status_b == "PASS":
                passed += 1
                print(f"{COLOR_GREEN}PASS{COLOR_RESET}")
            elif status_a == "PASS" and status_b == "FAIL":
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}")
                print(f"  A:PASS  B:FAIL — {msg_b}")
            elif status_a == "FAIL" and status_b == "PASS":
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}")
                print(f"  A:FAIL  B:PASS — A: {msg_a}")
            else:
                # Both fail
                failed += 1
                print(f"{COLOR_RED}FAIL{COLOR_RESET}")
                print(f"  A:FAIL — {msg_a}")
                print(f"  B:FAIL — {msg_b}")

        except Exception as e:
            failed += 1
            print(f"{COLOR_RED}ERROR{COLOR_RESET}\n  Unexpected execution error: {e}")

    # Standalone C unit tests (test_debugger.c) — oevm#14 regression proof.
    # Restore the committed solidified image first: the conformance loop clobbers
    # src/solidified_image.c per-test, and the C unit test links against it.
    subprocess.run(
        ["git", "-C", str(OEVM_DIR), "checkout",
         "src/solidified_image.c"],
        capture_output=True, text=True)
    print()
    c_units_ok = run_c_unit_tests(OEVM_DIR)

    # Integration gates: the supervisor-grant proof and the in-image debugger
    # harness. Both build the target app and CLOBBER ../oevm/src/solidified_image.c;
    # test_debugger_integration self-restores via try/finally, but
    # test_supervisor_grant does NOT — so we restore the committed image again
    # after BOTH run, leaving ../oevm a clean tree.
    print()
    grant_ok = run_integration_gate("test_supervisor_grant", "test_supervisor_grant.py")
    dbg_int_ok = run_integration_gate("test_debugger_integration", "test_debugger_integration.py")
    # rete#339: the debugger's front end is a custos verb; its case against a real VM
    # left the integration harness so a host without Go SKIPs it by name, not the rest.
    custos_dbg = run_custos_debug_gate()   # "PASS" | "SKIP" | "FAIL"
    subprocess.run(
        ["git", "-C", str(OEVM_DIR), "checkout",
         "src/solidified_image.c"],
        capture_output=True, text=True)

    portability = run_portability_gate()   # "PASS" | "SKIP" | "FAIL"
    cmake_parse = run_cmake_parse_gate()    # "PASS" | "SKIP" | "FAIL"
    boot_gc_ok = run_boot_gc_asan_gate()
    # tuvm#94 stage 5: the deep-sleep node harness drives the REAL MagTagDeepSleepNode
    # across simulated sleeps via OEVM_RTC_FILE. It uses absolute paths + its own
    # _build_pathb_once(), so it does NOT clobber ../oevm/src/solidified_image.c the way
    # the grant/debugger gates do — safe here, after the checkout restore.
    deepsleep_ok = run_integration_gate("test_deepsleep_node", "test_deepsleep_node.py")
    footprint = run_footprint_gate()   # "PASS" | "SKIP" | "FAIL"
    # #165: the host-tool Python suites. They build nothing and touch no shared
    # /tmp path, so they are safe here; they were run by NOTHING until now.
    mprof_py = run_pytest_gate("mprof-tools-py", "nodus/oevm/tools")
    console_py = run_pytest_gate("console-py", "nodus/oevm/zephyr/tools/console")
    # #216: the target harness's OBSERVATION WINDOW — the assertion that a
    # scenario keeps being watched after its TAPs pass. The window needs no board:
    # it is logic over the collector's log, so it is falsifiable here, and the
    # board is then spent only on what genuinely needs one. This is the first
    # thing under tests/target/ that this suite can run, and it is exactly the
    # piece whose absence let a gate score PASS 3/3 against a node that died four
    # ticks later (#213).
    observe_py = run_pytest_gate("target-observe-py", "nodus/oevm/tests/target")
    # rete#228: the WHOLE nsl/tools directory, so a new test file there is
    # collected the day it lands instead of joining the orphans (eight files
    # sat unrun; two were red at HEAD, two more silently SKIPped on a path the
    # #79 consolidation deleted). The ignores are the three integration
    # harnesses run_integration_gate already owns above — they bake images and
    # clobber the tracked solidified_image.c, so collecting them here would run
    # them twice. This gate SUBSUMES the former single-file-py (#252) and
    # manifest-py (#243) file gates: their files are ordinary members of the
    # collect now (the manifest tests still bootstrap the cached zcbor venv,
    # ~/.cache/rete-manifest-venv, which needs network ONCE).
    nsl_tools_py = run_pytest_gate("nsl-tools-py", "nodus/nsl/tools",
                                   ignore=("test_supervisor_grant.py",
                                           "test_debugger_integration.py",
                                           "test_custos_debug.py",
                                           "test_deepsleep_node.py"))
    # rete#228: the protocol's own tool tests (manifestlib, the linter, the pin
    # tool) were wired to nothing — the contract's parsers gated only by hand.
    protocol_py = run_pytest_gate("protocol-py", "protocol/tools")
    # rete#257: gwprofiler's suites could not even collect (cbor2, absent on
    # the bench where the tool runs). codec.py is stdlib-only now, pinned
    # against node-generated goldens, so this collects everywhere.
    gwprof_py = run_pytest_gate("gwprofiler-py", "nodus/oevm/zephyr/tools/gwprofiler")
    # rete#79: the tutorials are EXECUTED by this suite, so they cannot rot silently the
    # way porta/docs/GETTING-STARTED.md did. Last, because they reuse this run's Path B
    # build in OEVM_BUILD_DIR.
    tutorial = run_tutorial_gate("01-host.md", "tutorial-host")            # "PASS" | "FAIL"
    # 03-porta.md runs BOTH halves of the repo against each other on loopback — a real
    # `porta serve` and a real node image — so it binds udp 16969/16970 and must not run
    # beside tests/porta_sync_host.sh, which the node's source-constant gwPort pins to the
    # same pair. That gate is standalone, so nothing here collides; two concurrent nsl
    # suites would, and were already forbidden for sharing /tmp paths.
    tutorial_porta = run_tutorial_gate("03-porta.md", "tutorial-porta", needs=("go",))
    # rete#311: the debugger's page for a person, executed like page 1 (it reuses the same
    # /tmp/oevm_img and layout). Its board half is prose. It builds custos (rete#339).
    tutorial_dbg = run_tutorial_gate("04-debugger.md", "tutorial-debugger", needs=("go",))
    # 02-board.md is PROSE — no bash blocks, and its banner says why. Only its scenario
    # table is checkable on a host, and only by name. See run_tutorial_board_gate.
    tutorial_board = run_tutorial_board_gate()
    # #279: the same rot, one document over. CLI.md is a REFERENCE, so it cannot be
    # executed the way a tutorial can — it is checked against the binary's own --help.
    porta_cli = run_porta_cli_gate()

    suite_ok = (failed == 0 and c_units_ok and grant_ok and dbg_int_ok
                and custos_dbg != "FAIL"
                and portability != "FAIL" and boot_gc_ok
                and footprint != "FAIL" and deepsleep_ok
                and mprof_py != "FAIL" and console_py != "FAIL"
                and observe_py != "FAIL" and cmake_parse != "FAIL"
                and nsl_tools_py != "FAIL" and protocol_py != "FAIL"
                and gwprof_py != "FAIL" and tutorial != "FAIL"
                and tutorial_porta != "FAIL" and tutorial_board != "FAIL"
                and tutorial_dbg != "FAIL"
                and porta_cli != "FAIL")
    print(f"\n{COLOR_GREEN if suite_ok else COLOR_RED}Test Execution Summary:{COLOR_RESET}")
    print(f"  Passed:  {passed}")
    print(f"  Failed:  {failed}")
    print(f"  Skipped: {skipped}")
    # A SKIP is coloured, because an aux gate that did not run must not read as one
    # that passed (tuvm#31, #63, #143): footprint sat at SKIP for weeks under a green
    # header while the real T0.3 numbers were three-quarters red.
    def _mark(status: str) -> str:
        if status == "SKIP":
            return f"{COLOR_YELLOW}SKIP{COLOR_RESET}"
        if status == "FAIL":
            return f"{COLOR_RED}FAIL{COLOR_RESET}"
        return status

    # rete#275: c-units leads the aux line. It already gated suite_ok, but it was
    # not NAMED here — and this line is the one CLAUDE.md tells the reader to check,
    # because it can be red while "Failed: 0" reads green (#63). An unnamed gate is
    # how run_c_tests.sh stayed red for an extended period without anyone learning it.
    print(f"  c-units: {_mark('PASS' if c_units_ok else 'FAIL')}  "
          f"grant: {_mark('PASS' if grant_ok else 'FAIL')}  "
          f"debugger-integration: {_mark('PASS' if dbg_int_ok else 'FAIL')}  "
          f"custos-debug: {_mark(custos_dbg)}  "
          f"portability-32: {_mark(portability)}  "
          f"boot-gc-asan: {_mark('PASS' if boot_gc_ok else 'FAIL')}  "
          f"deepsleep: {_mark('PASS' if deepsleep_ok else 'FAIL')}  "
          f"footprint(T0.3): {_mark(footprint)}\n"
          f"  mprof-tools-py: {_mark(mprof_py)}  "
          f"console-py: {_mark(console_py)}  "
          f"target-observe-py: {_mark(observe_py)}  "
          f"cmake-parse: {_mark(cmake_parse)}  "
          f"nsl-tools-py: {_mark(nsl_tools_py)}  "
          f"protocol-py: {_mark(protocol_py)}  "
          f"gwprofiler-py: {_mark(gwprof_py)}  "
          f"tutorial-host: {_mark(tutorial)}  "
          f"tutorial-porta: {_mark(tutorial_porta)}  "
          f"tutorial-board: {_mark(tutorial_board)}  "
          f"tutorial-debugger: {_mark(tutorial_dbg)}  "
          f"porta-cli: {_mark(porta_cli)}")

    # NOT a skip, and deliberately not coloured like one: a skip names a cause it
    # has not verified, and this names one that IS verified. timer_zephyr.c posts
    # through the pulse mailbox; timer_posix.c posts straight into the space via
    # space_post. Every test counted above ran on the latter, so this suite is
    # STRUCTURALLY silent about coalescing, shedding and stale handles — a green
    # header here says nothing whatever about them (#151, #56 step 4).
    #
    # The reader most likely to be misled by "N passed, 0 failed" is the one who
    # never opens review/tests/CLAIMS.md, and that reader is often an agent
    # reading this line and nothing else. So the line says it here.
    target_dir = Path(__file__).resolve().parents[2] / "oevm" / "tests" / "target"
    n_scen = len(list(target_dir.glob("*.env"))) if target_dir.is_dir() else 0
    print(f"  target gates: {COLOR_YELLOW}NOT RUN BY THIS SUITE{COLOR_RESET} — the posix timer "
          f"backend bypasses the pulse\n"
          f"    mailbox, so coalescing, shedding and stale handles are UNTESTED here.\n"
          f"    (target-observe-py above is the harness's OWN logic, not the seams — #216.)\n"
          f"    Run: nodus/oevm/tests/target/run.sh   ({n_scen} scenarios, needs a board)")

    if not suite_ok:
        sys.exit(1)

if __name__ == "__main__":
    main()
