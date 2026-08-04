import datetime as dt
import pytest
from collect import Collector, acquire_lock, Busy
from logstore import LogStore, MARKER

TZ = dt.timezone.utc


class FakePort:
    """Hands back queued chunks, then raises to simulate the port going away."""

    def __init__(self, chunks, fail_after=None):
        self.chunks = list(chunks)
        self.fail_after = fail_after
        self.reads = 0
        self.closed = False

    def read(self, n):
        self.reads += 1
        if self.fail_after is not None and self.reads > self.fail_after:
            raise OSError("port went away")
        return self.chunks.pop(0) if self.chunks else b""

    def close(self):
        self.closed = True


class Clock:
    def __init__(self):
        self.t = dt.datetime(2026, 8, 3, 12, 0, 0, tzinfo=TZ)

    def __call__(self):
        return self.t

    def advance(self, seconds):
        self.t += dt.timedelta(seconds=seconds)


def body(store, day=dt.date(2026, 8, 3)):
    return open(store.path_for(day)).read()


def test_device_lines_reach_the_log(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    port = FakePort([b"boot ok\n"])
    c = Collector("/dev/fake", s, now=Clock(), open_port=lambda: port)
    c.step()
    assert "boot ok" in body(s)


def test_startup_marker_names_the_port(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    c = Collector("/dev/fake", s, now=Clock(), open_port=lambda: FakePort([b""]))
    c.step()
    assert MARKER in body(s) and "/dev/fake" in body(s)


def test_heartbeat_written_when_device_is_silent(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    clock = Clock()
    c = Collector("/dev/fake", s, heartbeat_s=60.0, now=clock,
                  open_port=lambda: FakePort([b"", b"", b""]))
    c.step()
    clock.advance(61)
    c.step()
    assert body(s).count("%s heartbeat" % MARKER) == 1


def test_heartbeat_not_written_early(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    clock = Clock()
    c = Collector("/dev/fake", s, heartbeat_s=60.0, now=clock,
                  open_port=lambda: FakePort([b"", b""]))
    c.step()
    clock.advance(10)
    c.step()
    assert "heartbeat" not in body(s)


def test_port_loss_and_reacquire_are_recorded_as_markers(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    ports = [FakePort([b"before\n"], fail_after=1), FakePort([b"after\n"])]
    c = Collector("/dev/fake", s, now=Clock(), open_port=lambda: ports.pop(0))
    c.step()          # reads "before", then the next read raises
    c.step()
    c.step()
    text = body(s)
    assert "%s port lost" % MARKER in text
    assert "%s port reacquired" % MARKER in text
    assert "after" in text


def test_a_gap_is_a_recorded_fact_not_an_inferred_one(tmp_path):
    # §8: a gap must never have to be inferred from a timestamp jump
    s = LogStore(str(tmp_path), "c6")
    c = Collector("/dev/fake", s, now=Clock(),
                  open_port=lambda: FakePort([b"x\n"], fail_after=1))
    c.step()
    c.step()
    assert "port lost" in body(s)


def test_second_instance_is_refused(tmp_path):
    first = acquire_lock(str(tmp_path), "c6")
    with pytest.raises(Busy):
        acquire_lock(str(tmp_path), "c6")
    first.close()


def test_lock_is_released_on_close(tmp_path):
    acquire_lock(str(tmp_path), "c6").close()
    acquire_lock(str(tmp_path), "c6").close()   # must not raise


def test_the_heartbeat_states_the_port_is_up(tmp_path):
    # #147: health is evidence of INPUT, so the beat says what the port is doing.
    s = LogStore(str(tmp_path), "c6")
    clock = Clock()
    c = Collector("/dev/fake", s, heartbeat_s=60.0, now=clock,
                  open_port=lambda: FakePort([b"", b""]))
    c.step()
    clock.advance(61)
    c.step()
    assert "%s heartbeat port=up" % MARKER in body(s)


def test_the_heartbeat_states_the_port_is_down_while_it_stays_down(tmp_path):
    # The transition marker is written once; the beat restates the state, so a
    # collector that has been orphaned since yesterday still says so today.
    s = LogStore(str(tmp_path), "c6")
    clock = Clock()

    def refuse():
        raise OSError("no such device")

    c = Collector("/dev/fake", s, heartbeat_s=60.0, now=clock, open_port=refuse)
    c.step()
    clock.advance(61)
    c.step()
    clock.advance(61)
    c.step()
    text = body(s)
    assert text.count("%s port lost" % MARKER) == 1
    assert text.count("%s heartbeat port=down" % MARKER) == 2
