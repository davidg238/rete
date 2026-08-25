import datetime as dt
import os
import pytest
import logread

HERE = os.path.dirname(__file__)
BOOT = os.path.join(HERE, "fixtures", "c6-boot.log")
FAULT = os.path.join(HERE, "fixtures", "c6-fault.log")


def entries(path):
    with open(path) as fh:
        return [logread.parse_line(l.rstrip("\n")) for l in fh if l.strip()]


def test_parse_line_splits_stamp_from_text():
    when, text = logread.parse_line("2026-08-03T09:14:03.412-07:00 hello world")
    assert text == "hello world"
    assert when.year == 2026 and when.tzinfo is not None


def test_unstamped_line_is_an_error_not_a_shrug():
    with pytest.raises(ValueError):
        logread.parse_line("no stamp here")


def test_boot_fixture_yields_exactly_one_run():
    runs = logread.split_runs(entries(BOOT))
    assert len(runs) == 1


def test_one_physical_boot_is_one_run_despite_two_banners():
    # Regression: a C6 boot prints ESP-ROM AND, ~30 lines later, *** Booting
    # Zephyr OS. Treating either as a run start split every boot in two and put
    # the fault in a different run from the build id naming its binary.
    text = "\n".join(l for _, l in
                     logread.split_runs(entries(BOOT))[0].lines)
    assert "ESP-ROM" in text and "Booting Zephyr OS" in text
    assert len(logread.split_runs(entries(BOOT))) == 1


def test_kernel_banner_alone_still_opens_a_run():
    # Attaching to an already-running board: no ROM line will ever be seen, so
    # the kernel banner has to be able to start a run (§10's MagTag case).
    now = dt.datetime.now(dt.timezone.utc)
    runs = logread.split_runs([
        (now, "*** Booting Zephyr OS build v4.2.0 ***"),
        (now, "oevm: build-id somekey-20260803T000000"),
    ])
    assert len(runs) == 1 and runs[0].build_id == "somekey-20260803T000000"


def test_run_carries_the_build_id_the_device_printed():
    run = logread.split_runs(entries(BOOT))[0]
    assert run.build_id == "porta-v1-final-779-gc76a6d5-dirty-20260803T201928"


def test_fault_fixture_has_a_panic():
    run = logread.split_runs(entries(FAULT))[0]
    assert logread.find_panics(run)


def test_panic_carries_coredump_payload():
    run = logread.split_runs(entries(FAULT))[0]
    panic = logread.find_panics(run)[0]
    assert panic.cd_lines
    assert all(c in "0123456789abcdefABCDEF" for c in "".join(panic.cd_lines))


def test_panic_payload_excludes_the_begin_and_end_markers():
    run = logread.split_runs(entries(FAULT))[0]
    panic = logread.find_panics(run)[0]
    assert not any("BEGIN" in c or "END" in c for c in panic.cd_lines)


def test_clean_boot_has_no_panic():
    run = logread.split_runs(entries(BOOT))[0]
    assert logread.find_panics(run) == []


def test_zero_runs_in_a_nonempty_log_is_an_error():
    # §8: this is #110's lesson applied to the tool built to prevent the next #110
    junk = [(dt.datetime.now(dt.timezone.utc), "nothing that looks like a boot")]
    with pytest.raises(logread.NoRuns):
        logread.split_runs(junk, strict=True)


def test_empty_log_is_an_empty_success_not_an_error():
    assert logread.split_runs([], strict=True) == []


def test_since_accepts_a_relative_spec():
    now = dt.datetime(2026, 8, 3, 12, 0, 0, tzinfo=dt.timezone.utc)
    assert logread.resolve_since("-30s", now=now) == now - dt.timedelta(seconds=30)
    assert logread.resolve_since("-5m", now=now) == now - dt.timedelta(minutes=5)


def test_cli_since_accepts_a_negative_spec_after_a_separator(tmp_path, capsys):
    # This is exactly the call run.sh --capture makes on the C6. Without the
    # `--`, argparse reads -180s as an option flag and the subcommand dies.
    from logstore import LogStore
    s = LogStore(str(tmp_path), "c6")
    s.append("recent line", dt.datetime.now(dt.timezone.utc).astimezone())
    rc = logread.main(["--root", str(tmp_path), "--board", "c6", "since", "--", "-180s"])
    assert rc == 0
    assert "recent line" in capsys.readouterr().out


def test_since_accepts_an_iso_timestamp():
    got = logread.resolve_since("2026-08-03T09:00:00-07:00")
    assert got.hour == 9 and got.tzinfo is not None
