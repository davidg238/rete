import datetime as dt
import check
from logstore import LogStore, MARKER


def test_recent_heartbeat_passes(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("%s heartbeat" % MARKER, now)
    assert check.check_health(str(tmp_path), "c6", now=now) == []


def test_stale_heartbeat_fails(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("%s heartbeat" % MARKER, now - dt.timedelta(seconds=400))
    problems = check.check_health(str(tmp_path), "c6", now=now)
    assert problems and "heartbeat" in problems[0]


def test_no_heartbeat_at_all_fails(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("some device noise", now)
    assert check.check_health(str(tmp_path), "c6", now=now)


def test_empty_log_dir_fails(tmp_path):
    # a positive assertion: we require evidence of life, never absence of trouble
    assert check.check_health(str(tmp_path), "c6")


def test_threshold_is_three_missed_beats(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("%s heartbeat" % MARKER, now - dt.timedelta(seconds=170))
    assert check.check_health(str(tmp_path), "c6", now=now) == []


def test_a_just_restarted_collector_is_healthy_before_its_first_beat(tmp_path):
    # Regression: the first heartbeat is 60s after start, so heartbeat-only
    # health reported a demonstrably-working collector as down for a minute
    # after every restart. Found by stopping and restarting the real unit.
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("%s heartbeat" % MARKER, now - dt.timedelta(seconds=400))
    s.append("%s collector started pid=1 port=/dev/x" % MARKER,
             now - dt.timedelta(seconds=5))
    assert check.check_health(str(tmp_path), "c6", now=now) == []


def test_a_stale_startup_marker_still_fails(tmp_path):
    # The restart allowance must not become a permanent pass: a collector that
    # started and immediately wedged goes red once its marker ages out.
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("%s collector started pid=1 port=/dev/x" % MARKER,
             now - dt.timedelta(seconds=400))
    assert check.check_health(str(tmp_path), "c6", now=now)


def test_device_traffic_alone_is_not_health(tmp_path):
    # A chatty board proves the BOARD is alive, not that the collector is
    # writing. Only the collector's own marker proves the collector.
    s = LogStore(str(tmp_path), "c6")
    now = dt.datetime.now(dt.timezone.utc).astimezone()
    s.append("%s heartbeat" % MARKER, now - dt.timedelta(seconds=400))
    s.append("TIMER OK", now)
    assert check.check_health(str(tmp_path), "c6", now=now)
