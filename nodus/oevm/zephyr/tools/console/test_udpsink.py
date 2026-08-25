import datetime as dt
import socket
import udpsink
from logstore import LogStore, MARKER


class Clock:
    def __init__(self):
        self.t = dt.datetime(2026, 8, 4, 15, 30, 0, tzinfo=dt.timezone.utc)

    def __call__(self):
        return self.t

    def advance(self, s):
        self.t += dt.timedelta(seconds=s)


def body(store, day=dt.date(2026, 8, 4)):
    return open(store.path_for(day)).read()


def loopback_pair(port):
    srv = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    srv.bind(("::1", 0))
    cli = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    return srv, cli, srv.getsockname()[:2]


def test_a_datagram_becomes_a_stamped_line(tmp_path):
    s = LogStore(str(tmp_path), "c6")
    srv, cli, addr = loopback_pair(0)
    cli.sendto(b"bell slot 31 at 15:30:00 - 7 bells\n", addr)
    n = [0]

    def stop():
        n[0] += 1
        return n[0] > 2

    udpsink.serve(s, 0, now=Clock(), stop=stop, sock=srv)
    text = body(s)
    assert "bell slot 31 at 15:30:00 - 7 bells" in text
    assert "udpsink started" in text
    srv.close(); cli.close()


def test_the_sender_is_recorded_once(tmp_path):
    # Which node is talking is evidence, and a line per datagram would bury the log.
    s = LogStore(str(tmp_path), "c6")
    srv, cli, addr = loopback_pair(0)
    cli.sendto(b"one\n", addr)
    cli.sendto(b"two\n", addr)
    n = [0]

    def stop():
        n[0] += 1
        return n[0] > 3

    udpsink.serve(s, 0, now=Clock(), stop=stop, sock=srv)
    text = body(s)
    assert text.count("%s peer " % MARKER) == 1
    assert "one" in text and "two" in text
    srv.close(); cli.close()


def test_heartbeat_states_the_socket_is_up(tmp_path):
    # Same contract as collect.py, so check.py can gate this store too.
    s = LogStore(str(tmp_path), "c6")
    srv, cli, addr = loopback_pair(0)
    clock = Clock()
    n = [0]

    def stop():
        n[0] += 1
        clock.advance(40)
        return n[0] > 3

    udpsink.serve(s, 0, heartbeat_s=60.0, now=clock, stop=stop, sock=srv)
    assert "%s heartbeat port=up" % MARKER in body(s)
    srv.close(); cli.close()


def test_a_second_sink_is_refused(tmp_path):
    # SO_REUSEADDR would happily let two sinks bind one port; the store's lock is
    # what stops them. Witnessed on the gateway before this existed.
    from logstore import acquire_lock, Busy
    import pytest
    first = acquire_lock(str(tmp_path), "c6")
    assert udpsink.main(["--root", str(tmp_path), "--board", "c6"]) == 3
    first.close()
