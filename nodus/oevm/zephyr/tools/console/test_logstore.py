import datetime as dt
import pytest
from logstore import LogStore, MARKER

TZ = dt.timezone(dt.timedelta(hours=-7))


def at(y, m, d, hh=0, mm=0, ss=0, ms=0):
    return dt.datetime(y, m, d, hh, mm, ss, ms * 1000, tzinfo=TZ)


def test_line_is_stamped_iso8601_with_offset_and_millis(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    s.append("hello", at(2026, 8, 3, 9, 14, 3, 412))
    body = open(s.path_for(dt.date(2026, 8, 3))).read()
    assert body == "2026-08-03T09:14:03.412-07:00 hello\n"


def test_filename_carries_board_and_day(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    assert s.path_for(dt.date(2026, 8, 3)).endswith("console-c6-2026-08-03.log")


def test_lines_append_rather_than_truncate(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    s.append("one", at(2026, 8, 3))
    s.append("two", at(2026, 8, 3))
    assert open(s.path_for(dt.date(2026, 8, 3))).read().count("\n") == 2


def test_a_new_day_starts_a_new_file(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    s.append("mon", at(2026, 8, 3))
    s.append("tue", at(2026, 8, 4))
    assert s.days() == [dt.date(2026, 8, 3), dt.date(2026, 8, 4)]


def test_naive_datetime_is_refused(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    with pytest.raises(ValueError):
        s.append("x", dt.datetime(2026, 8, 3, 9, 0, 0))


def test_prune_deletes_oldest_days_until_under_budget(tmp_path):
    # A day here is 3 lines x 51 bytes (30-char stamp + 20 payload + newline)
    # = 153. The budget must leave room for at least one whole day, or the
    # "never delete the only day" rule fires first and this measures that
    # instead.
    s = LogStore(str(tmp_path), "c6", budget_bytes=200)
    for day in (3, 4, 5):
        for i in range(3):
            s.append("x" * 20, at(2026, 8, day))
    assert s.total_bytes() > 200
    deleted = s.prune()
    assert deleted, "prune deleted nothing while over budget"
    assert s.total_bytes() <= 200
    assert dt.date(2026, 8, 3) not in s.days()
    assert dt.date(2026, 8, 5) in s.days()


def test_prune_never_deletes_the_only_day(tmp_path):
    s = LogStore(str(tmp_path), "c6", budget_bytes=10)
    s.append("x" * 500, at(2026, 8, 3))
    assert s.prune() == []
    assert s.days() == [dt.date(2026, 8, 3)]


def test_prune_is_a_noop_under_budget(tmp_path):
    s = LogStore(str(tmp_path), "c6", budget_bytes=10_000)
    s.append("small", at(2026, 8, 3))
    assert s.prune() == []


def test_marker_constant_is_stable(tmp_path):
    assert MARKER == "@@rete-console@@"
