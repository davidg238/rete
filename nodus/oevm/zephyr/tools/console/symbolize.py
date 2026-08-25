#!/usr/bin/env python3
"""Resolve a panic against the ELF that produced it — or refuse (design D4).

Symbolizing a pre-reflash panic against the current build dir yields
confident, precise, wrong locations. That is worse than no answer, because it
looks like one. So there is no fallback: the run names a build id, the archive
either has it or the tool stops.

The one exception (D4a) must be typed by a human and is echoed onto every line
of output, so the assumption travels with the answer into whatever issue
comment it gets pasted into.

Core reconstruction is Zephyr's own scripts/coredump/, which supports RISC-V on
both sides (arch/riscv/core/coredump.c device-side, gdbstubs/arch/risc_v.py
host-side).
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile

DEFAULT_ROOT = os.path.expanduser("~/.local/state/rete/elf")
ZEPHYR_BASE = os.environ.get("ZEPHYR_BASE", os.path.expanduser("~/zephyr-esp32/zephyr"))


class Refused(Exception):
    """No trustworthy ELF. Deliberately not a fallback."""


class SymbolizeFailed(Exception):
    """The ELF was found but the tooling could not produce a backtrace."""


def _zephyr_python():
    """The coredump scripts import pyelftools, which lives in the Zephyr venv,
    not in the system interpreter. Guessing wrong here fails with a bare
    ModuleNotFoundError that says nothing about why."""
    override = os.environ.get("RETE_ZEPHYR_PYTHON")
    if override:
        return override
    for cand in (os.path.expanduser("~/zephyr-esp32/.venv/bin/python"),
                 os.path.expanduser("~/zephyrproject/.venv/bin/python")):
        if os.path.exists(cand):
            return cand
    return sys.executable


def _gdb():
    """A host gdb will happily produce plausible, wrong output for a RISC-V
    core, so this never silently falls back to `gdb`."""
    override = os.environ.get("RETE_GDB")
    if override:
        return override
    found = shutil.which("riscv64-zephyr-elf-gdb")
    if found:
        return found
    for cand in sorted(glob.glob(os.path.expanduser(
            "~/zephyr-sdk-*/riscv64-zephyr-elf/bin/riscv64-zephyr-elf-gdb")), reverse=True):
        return cand
    raise SymbolizeFailed(
        "riscv64-zephyr-elf-gdb not found on PATH or under ~/zephyr-sdk-*.\n"
        "Set RETE_GDB. Refusing to fall back to the host gdb: it would produce "
        "output that looks like an answer and is not.")


def archive_path(build_id, root=None):
    root = root or DEFAULT_ROOT
    elf = os.path.join(root, build_id, "zephyr.elf")
    if not os.path.exists(elf):
        raise Refused(
            "no archived ELF for build id %r under %s.\n"
            "Refusing rather than guessing: the current build dir would give\n"
            "precise, confident, wrong source locations.\n"
            "If you know which build this was, pass --assume-build <key>.\n"
            "%s may tell you what was flashed last, but read it yourself —\n"
            "this tool will not." % (build_id, root,
                                     os.path.join(root, "last-flashed-<board>")))
    return elf


def _core_from_panic(panic, workdir):
    """Zephyr's own parser turns the #CD: hex back into a core file. Rebuilt
    from this panic's lines rather than handed the whole log, so a log holding
    two panics does not concatenate them into one unusable core."""
    log = os.path.join(workdir, "panic.log")
    core = os.path.join(workdir, "core.bin")
    with open(log, "w") as fh:
        fh.write("#CD:BEGIN#\n")
        for chunk in panic.cd_lines:
            fh.write("#CD:%s\n" % chunk)
        fh.write("#CD:END#\n")
    parser = os.path.join(ZEPHYR_BASE, "scripts", "coredump",
                          "coredump_serial_log_parser.py")
    proc = subprocess.run([_zephyr_python(), parser, log, core],
                          capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(core):
        raise SymbolizeFailed("coredump_serial_log_parser failed:\n%s%s"
                              % (proc.stdout, proc.stderr))
    return core


def symbolize_panic(run, panic, assume_build=None, root=None, workdir=None):
    key = assume_build or run.build_id
    if not key:
        raise Refused(
            "this run never printed a build id — it faulted before the banner,\n"
            "so nothing in the log says which binary it was running.\n"
            "Pass --assume-build <key> if you know. Nothing will infer it for you.")
    elf = archive_path(key, root=root)

    # Kept, not cleaned up: a core worth symbolizing is worth still having when
    # the backtrace raises the next question.
    workdir = workdir or tempfile.mkdtemp(prefix="rete-symbolize-")
    os.makedirs(workdir, exist_ok=True)
    core = _core_from_panic(panic, workdir)

    # --pipe, not a TCP port. The stub does listen(1)+accept() exactly once, so
    # any readiness probe that opens a connection consumes the one gdb needs and
    # the session dies in packet errors. Piping also means no fixed port to
    # collide with a second symbolize running alongside.
    #
    # The stub raises UnboundLocalError on `gdbserver.close()` during --pipe
    # teardown (upstream bug: the variable only exists in the socket path). It
    # fires AFTER the backtrace has been delivered, and lands on stderr, which
    # is why stderr is dropped below rather than reported.
    server = os.path.join(ZEPHYR_BASE, "scripts", "coredump", "coredump_gdbserver.py")
    remote = "target remote | %s %s --pipe %s %s" % (_zephyr_python(), server, elf, core)
    out = subprocess.run(
        [_gdb(), elf, "-batch", "-ex", remote, "-ex", "bt"],
        capture_output=True, text=True, timeout=120).stdout

    if not out.strip():
        raise SymbolizeFailed("gdb produced no backtrace for %s" % key)

    prefix = ("[ASSUMED build-id %s] " % key) if assume_build else ""
    return "\n".join(prefix + line for line in out.splitlines())
