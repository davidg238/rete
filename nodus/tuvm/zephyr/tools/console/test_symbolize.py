import os
import pytest
import logread
import symbolize

HERE = os.path.dirname(__file__)
FAULT = os.path.join(HERE, "fixtures", "c6-fault.log")


def fault_run():
    with open(FAULT) as fh:
        entries = [logread.parse_line(l.rstrip("\n")) for l in fh if l.strip()]
    return logread.split_runs(entries)[0]


def test_missing_archive_is_refused(tmp_path):
    run = fault_run()
    panic = logread.find_panics(run)[0]
    with pytest.raises(symbolize.Refused):
        symbolize.symbolize_panic(run, panic, root=str(tmp_path))


def test_refusal_names_the_key_it_looked_for(tmp_path):
    run = fault_run()
    panic = logread.find_panics(run)[0]
    with pytest.raises(symbolize.Refused) as exc:
        symbolize.symbolize_panic(run, panic, root=str(tmp_path))
    assert run.build_id in str(exc.value)


def test_a_run_with_no_build_id_is_refused(tmp_path):
    run = fault_run()
    run.build_id = None                      # a fault before the banner printed
    panic = logread.find_panics(run)[0]
    with pytest.raises(symbolize.Refused):
        symbolize.symbolize_panic(run, panic, root=str(tmp_path))


def test_assume_build_is_the_only_way_past_a_missing_build_id(tmp_path):
    run = fault_run()
    run.build_id = None
    panic = logread.find_panics(run)[0]
    (tmp_path / "KEY").mkdir()
    (tmp_path / "KEY" / "zephyr.elf").write_bytes(b"not really an elf")
    # gets past the refusal and fails later, on the ELF itself, not on the key
    with pytest.raises(symbolize.SymbolizeFailed):
        symbolize.symbolize_panic(run, panic, assume_build="KEY",
                                  root=str(tmp_path), workdir=str(tmp_path / "wd"))


def test_last_flashed_pointer_is_never_consulted_automatically(tmp_path):
    # D4a: if the reader read this file itself, the refusal would be a
    # fallback wearing a disguise
    (tmp_path / "last-flashed-c6").write_text("SOMEKEY\n")
    (tmp_path / "SOMEKEY").mkdir()
    (tmp_path / "SOMEKEY" / "zephyr.elf").write_bytes(b"x")
    run = fault_run()
    run.build_id = None
    panic = logread.find_panics(run)[0]
    with pytest.raises(symbolize.Refused):
        symbolize.symbolize_panic(run, panic, root=str(tmp_path))


def test_archive_path_returns_the_elf_when_present(tmp_path):
    (tmp_path / "K").mkdir()
    (tmp_path / "K" / "zephyr.elf").write_bytes(b"x")
    assert symbolize.archive_path("K", root=str(tmp_path)).endswith("K/zephyr.elf")


def test_the_real_fault_resolves_to_the_line_that_caused_it(tmp_path):
    """The acceptance criterion of #56 sub-project 1 (design §9).

    Skips only if the archived ELF has been pruned — never because the
    toolchain is missing, since a silently-skipped acceptance test is the
    thing §9 warns about.
    """
    run = fault_run()
    try:
        symbolize.archive_path(run.build_id)
    except symbolize.Refused:
        pytest.skip("archived ELF for %s has been pruned; reflash to regenerate"
                    % run.build_id)
    panic = logread.find_panics(run)[0]
    out = symbolize.symbolize_panic(run, panic, workdir=str(tmp_path))
    # The FILE and LINE come from the archived binary's DWARF and are stable for
    # as long as that ELF exists. That is what this test may assert.
    assert "src/main.c:1688" in out
    # It may NOT assert the source TEXT. gdb renders line 1688 of whatever
    # main.c is on disk NOW, not of the file that was compiled — so any later
    # edit above line 1688 silently changes the quoted line while the number
    # stays right. Asserting `tuvm_fault_target` here turned red on 2026-08-04
    # for a reason that had nothing to do with symbolizing: ~90 lines were added
    # to main.c for the RTC alarm seam.
    # This is the ELF-refusal hazard (design D4) one layer down: the refusal
    # guards the BINARY against being wrong, and nothing guards the SOURCE. A
    # correct line number quoting an unrelated line is exactly the confident
    # wrong answer D4 exists to prevent. Filed rather than papered over —
    # symbolize should say when the tree has moved since the build.


def test_assumed_output_carries_the_assumption_on_every_line(tmp_path):
    run = fault_run()
    key = run.build_id
    try:
        symbolize.archive_path(key)
    except symbolize.Refused:
        pytest.skip("archived ELF for %s has been pruned" % key)
    run.build_id = None
    panic = logread.find_panics(run)[0]
    out = symbolize.symbolize_panic(run, panic, assume_build=key,
                                    workdir=str(tmp_path))
    lines = [l for l in out.splitlines() if l.strip()]
    assert lines, "no output to check"
    assert all(l.startswith("[ASSUMED build-id %s]" % key) for l in lines)
