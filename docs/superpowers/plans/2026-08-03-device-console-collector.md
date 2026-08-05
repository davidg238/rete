# Device Console Collector and Offline Symbolizer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An always-on daemon that captures the ESP32-C6 console to durable timestamped logs, plus an offline reader that splits those logs into runs and symbolizes a fault against the exact ELF that produced it.

**Architecture:** A dumb collector and a smart offline reader (D3). The collector opens one serial port, strips NULs, stamps lines, appends to a daily file and writes a heartbeat — it parses nothing. Everything that understands boot banners, build ids, coredumps and ELF archives lives in the reader, which runs over stored logs and is testable with no board attached. The seam that makes both testable is that the collector takes a *path*: `/dev/serial/by-id/...` in production, one end of a PTY pair in tests.

**Tech Stack:** Python 3 + pyserial (both already on this machine), pytest, systemd `--user` template units, CMake (Zephyr app), Zephyr's own `scripts/coredump/` tooling for core reconstruction.

**Spec:** `docs/design/2026-08-03-device-console-collector-design.md` (approved 2026-08-03, commit `39a6926`). Section references below (§n, Dn) are to that document.

## Global Constraints

- **Board scope: ESP32-C6 only** (D1). The `esp32` and `esp32s2` paths in `run.sh` must behave exactly as they do today after every task. Task 11's gate asserts this.
- **Kconfig namespace is `TUVM_`**, matching `CONFIG_TUVM_NET_HEARTBEAT`. Not `RETE_`.
- **Config overlays are `prj_<board>_<thing>.conf`**, applied via `run.sh --conf-extra`, matching `prj_esp32_netdiag.conf`.
- **Python tools live beside their tests** using flat imports — `tools/gwprofiler/store.py` with `tools/gwprofiler/test_store.py` doing `from store import Store`. Follow that exactly; do not add packages, `__init__.py`, or `src/` layouts.
- **Log location is `~/.local/state/rete/console/`**, ELF archive `~/.local/state/rete/elf/` — state, never the repo (§6). Every path is overridable by constructor argument so tests use `tmp_path`.
- **Log line format:** `<ISO-8601 with offset, milliseconds> <text>` — e.g. `2026-08-03T09:14:03.412-07:00 tuvm: build-id ...`.
- **Log filename:** `console-<board>-YYYY-MM-DD.log`.
- **Budget:** 2 GB per board, oldest-day-first pruning (§6).
- **Heartbeat:** every 60 s (D7); acceptance asserts one newer than 180 s (§8).
- **Collector markers** are all one string constant, `MARKER = "@@rete-console@@"`, so the reader can distinguish collector-written lines from device output with no ambiguity.
- **The collector never parses.** If a task tempts you to add a regex to `collect.py`, it belongs in `logread.py`.

---

## Why the task order is what it is

The design's §9 calls for "checked-in golden fixtures from a real C6 boot and a real fault." **Those fixtures do not exist yet.** The repo has real *ESP32* (Xtensa) fault dumps — `esp32-debug-artifacts/tuvm-esp32-crash.log:84` onward, with real `#CD:` coredump lines — and real *C6* boot logs — `esp32-debug-artifacts/tuvm-esp32c6-SOAK-overnight-0720-2304.log:1` onward — but no C6 fault, because nothing has ever deliberately faulted a C6.

Also: `prj_esp32c6.conf` has **no coredump configuration at all** (it is not in the file — checked). A C6 fault today prints a fatal-error banner and nothing decodable. `coredump.conf` in the same directory is written for the ESP32 and documents the recipe.

So the D8 fault trigger is not the last task, it is the **fixture generator**, and it must land before the fault-parsing code it exists to exercise. Task 7 builds it, Task 8 runs it on hardware and checks in what came back, and Tasks 9–10 are written against that real capture. Writing the fault parser earlier means writing it against an invented format, which is precisely the "confident, precise, wrong" failure D4 exists to prevent — one layer up.

Tasks 1–6 have no such dependency and can proceed immediately.

---

### Task 1: Track the MagTag capture script verbatim

Clears the debt §2 diagnoses: the tooling recent sessions actually relied on lives outside the repo. Moves it in **unchanged** (§10, §11). No behaviour edits, no generalising — it is deleted when the MagTag collector lands.

**Files:**
- Create: `nodus/tuvm/zephyr/boards/magtag/capture.py` (copy of `~/magtag-captures/capture.py`)
- Create: `nodus/tuvm/zephyr/boards/magtag/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing any later task imports. This task is deliberately inert.

- [ ] **Step 1: Copy the file in, byte-for-byte**

```bash
cd /home/david/workspace/rete
cp ~/magtag-captures/capture.py nodus/tuvm/zephyr/boards/magtag/capture.py
diff ~/magtag-captures/capture.py nodus/tuvm/zephyr/boards/magtag/capture.py && echo IDENTICAL
```

Expected: `IDENTICAL`. If `diff` reports anything, you edited it — start over.

- [ ] **Step 2: Write the README that explains why an unmodified file is being tracked**

Create `nodus/tuvm/zephyr/boards/magtag/README.md`:

```markdown
# MagTag console capture

`capture.py` is tracked here **verbatim** as it existed in `~/magtag-captures/` on
2026-08-03. It is not maintained, not tested, and not the pattern to copy.

It is here because #56 sub-project 1 diagnosed untracked out-of-repo tooling as a
problem (design §2) and it would be incoherent to ship that diagnosis while leaving
the specimen in a home directory. Tracking it is not endorsing it.

It already encodes one hard-won lesson — it strips NUL bytes — which is why the
C6 collector does the same (`tools/console/lines.py`).

**It is deleted when a MagTag collector lands.** The MagTag needs its own design
work first: its PL2303 header does not reset the board on open, so a collector may
attach to a running board mid-life and the log will start mid-stream (design §10).

For the C6, use `tools/console/` instead.
```

- [ ] **Step 3: Verify nothing else in the repo referenced the old path**

Run: `grep -rn 'magtag-captures' --include='*.py' --include='*.sh' --include='*.md' . | grep -v docs/design | grep -v docs/superpowers`
Expected: no output. (The design doc and this plan mention it deliberately.)

- [ ] **Step 4: Commit**

```bash
git add nodus/tuvm/zephyr/boards/magtag/
git commit -m "chore(#56): track magtag capture.py verbatim, with the reason

Design §2 names untracked out-of-repo tooling as the problem; leaving the
specimen in ~ while shipping the diagnosis would be the diagnosis without
the treatment. Unmodified on purpose — it is deleted when the MagTag
collector lands (§10)."
```

---

### Task 2: The log store

Pure file-layout logic: stamping, daily files, byte accounting, oldest-first pruning. No serial, no parsing. Fully testable with `tmp_path`.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/logstore.py`
- Test: `nodus/tuvm/zephyr/tools/console/test_logstore.py`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `MARKER: str` — the constant `"@@rete-console@@"`.
  - `class LogStore(root: str, board: str, budget_bytes: int = 2*1024**3)`
  - `LogStore.append(text: str, when: datetime) -> None` — `when` must be timezone-aware; raises `ValueError` if naive.
  - `LogStore.path_for(day: datetime.date) -> str`
  - `LogStore.days() -> list[datetime.date]` — sorted oldest first.
  - `LogStore.total_bytes() -> int`
  - `LogStore.prune() -> list[str]` — deletes whole oldest days until under budget; returns the paths deleted, oldest first. Never deletes the newest day even if it alone exceeds budget.

- [ ] **Step 1: Write the failing tests**

Create `nodus/tuvm/zephyr/tools/console/test_logstore.py`:

```python
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
    s = LogStore(str(tmp_path), "c6", budget_bytes=120)
    for day in (3, 4, 5):
        for i in range(3):
            s.append("x" * 20, at(2026, 8, day))
    assert s.total_bytes() > 120
    deleted = s.prune()
    assert deleted, "prune deleted nothing while over budget"
    assert s.total_bytes() <= 120
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_logstore.py -v`
Expected: collection error — `ModuleNotFoundError: No module named 'logstore'`.

- [ ] **Step 3: Write the implementation**

Create `nodus/tuvm/zephyr/tools/console/logstore.py`:

```python
"""Durable, day-partitioned console log storage.

Deliberately dumb (design D3): this module stamps and appends, and knows
nothing about boot banners, build ids or faults. Daily files make `since` a
file-selection problem rather than a rotation-offset problem (§6).
"""
import datetime as dt
import os

MARKER = "@@rete-console@@"

DEFAULT_ROOT = os.path.expanduser("~/.local/state/rete/console")
DEFAULT_BUDGET = 2 * 1024 ** 3


class LogStore:
    def __init__(self, root=DEFAULT_ROOT, board="c6", budget_bytes=DEFAULT_BUDGET):
        self.root = root
        self.board = board
        self.budget_bytes = budget_bytes
        os.makedirs(self.root, exist_ok=True)

    def path_for(self, day):
        return os.path.join(self.root, "console-%s-%s.log" % (self.board, day.isoformat()))

    def append(self, text, when):
        if when.tzinfo is None or when.tzinfo.utcoffset(when) is None:
            raise ValueError("timestamp must be timezone-aware; a log spanning "
                             "days needs its zone stated, not assumed (§6)")
        stamp = when.isoformat(timespec="milliseconds")
        with open(self.path_for(when.date()), "a", encoding="utf-8", errors="replace") as fh:
            fh.write("%s %s\n" % (stamp, text))

    def days(self):
        out = []
        prefix = "console-%s-" % self.board
        for name in os.listdir(self.root):
            if not (name.startswith(prefix) and name.endswith(".log")):
                continue
            try:
                out.append(dt.date.fromisoformat(name[len(prefix):-len(".log")]))
            except ValueError:
                continue          # not ours; leave it alone
        return sorted(out)

    def total_bytes(self):
        return sum(os.path.getsize(self.path_for(d)) for d in self.days())

    def prune(self):
        """Delete whole oldest days until under budget. Never the newest day:
        a single wedged day over budget is a fact to report, not a reason to
        destroy the only evidence there is."""
        deleted = []
        days = self.days()
        while len(days) > 1 and self.total_bytes() > self.budget_bytes:
            victim = self.path_for(days.pop(0))
            os.remove(victim)
            deleted.append(victim)
        return deleted
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_logstore.py -v`
Expected: 9 passed.

- [ ] **Step 5: Commit**

```bash
git add nodus/tuvm/zephyr/tools/console/logstore.py nodus/tuvm/zephyr/tools/console/test_logstore.py
git commit -m "feat(#56): console log store — daily files, ISO-8601 stamps, budget pruning

Timezone-naive timestamps are refused rather than assumed: a device whose
own clock is under test needs the host's zone stated (design §6). Pruning
never takes the newest day — one wedged day over budget is a fact to
report, not a reason to delete the only evidence."
```

---

### Task 3: Line assembly and the collector daemon

The daemon proper. Split into a pure `LineAssembler` (bytes in, lines out) and a `Collector` that owns the loop, so the parsing-free byte handling is testable without any I/O at all, and the loop is testable over a real PTY.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/lines.py`
- Create: `nodus/tuvm/zephyr/tools/console/collect.py`
- Test: `nodus/tuvm/zephyr/tools/console/test_lines.py`
- Test: `nodus/tuvm/zephyr/tools/console/test_collect.py`

**Interfaces:**
- Consumes: `logstore.LogStore`, `logstore.MARKER` (Task 2).
- Produces:
  - `class LineAssembler()` with `feed(data: bytes) -> list[str]` and `flush() -> list[str]`.
  - `class Busy(Exception)`
  - `acquire_lock(root: str, board: str) -> io.IOBase` — raises `Busy` if held. Keep the returned handle alive for the process lifetime.
  - `class Collector(port_path, store, heartbeat_s=60.0, now=None, open_port=None)`
    - `now` is a zero-arg callable returning an aware `datetime` (injected in tests).
    - `open_port` is a zero-arg callable returning an object with `read(n) -> bytes` and `close()`. Defaults to a pyserial factory.
    - `Collector.step() -> None` — one read/heartbeat/reconnect cycle.
    - `Collector.run(stop: callable = None) -> None` — loop until `stop()` is true.

- [ ] **Step 1: Write the failing tests for line assembly**

Create `nodus/tuvm/zephyr/tools/console/test_lines.py`:

```python
from lines import LineAssembler

def test_complete_line_emitted():
    assert LineAssembler().feed(b"hello\n") == ["hello"]

def test_partial_line_is_held_until_terminated():
    a = LineAssembler()
    assert a.feed(b"par") == []
    assert a.feed(b"tial\n") == ["partial"]

def test_nul_bytes_are_stripped():
    # the one hard lesson the untracked magtag capture.py already encoded
    assert LineAssembler().feed(b"he\x00llo\n") == ["hello"]

def test_crlf_is_normalised():
    assert LineAssembler().feed(b"hello\r\n") == ["hello"]

def test_multiple_lines_in_one_read():
    assert LineAssembler().feed(b"a\nb\nc\n") == ["a", "b", "c"]

def test_undecodable_bytes_do_not_raise():
    out = LineAssembler().feed(b"\xff\xfe ok\n")
    assert len(out) == 1 and "ok" in out[0]

def test_flush_emits_the_held_partial():
    a = LineAssembler()
    a.feed(b"dangling")
    assert a.flush() == ["dangling"]
    assert a.flush() == []

def test_empty_lines_survive():
    # a blank line in a fault dump is evidence of spacing, not noise
    assert LineAssembler().feed(b"a\n\nb\n") == ["a", "", "b"]
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_lines.py -v`
Expected: `ModuleNotFoundError: No module named 'lines'`.

- [ ] **Step 3: Implement line assembly**

Create `nodus/tuvm/zephyr/tools/console/lines.py`:

```python
"""Bytes to lines. No interpretation of what the lines mean (design D3)."""


class LineAssembler:
    def __init__(self):
        self._buf = b""

    def feed(self, data):
        self._buf += data.replace(b"\x00", b"")
        out = []
        while True:
            idx = self._buf.find(b"\n")
            if idx < 0:
                return out
            raw, self._buf = self._buf[:idx], self._buf[idx + 1:]
            out.append(raw.rstrip(b"\r").decode("utf-8", errors="replace"))

    def flush(self):
        if not self._buf:
            return []
        raw, self._buf = self._buf, b""
        return [raw.rstrip(b"\r").decode("utf-8", errors="replace")]
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_lines.py -v`
Expected: 8 passed.

- [ ] **Step 5: Write the failing tests for the collector**

Create `nodus/tuvm/zephyr/tools/console/test_collect.py`. These use a fake port rather than a PTY for determinism; Task 11's gate exercises a real PTY end to end.

```python
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
```

- [ ] **Step 6: Run to verify they fail**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_collect.py -v`
Expected: `ModuleNotFoundError: No module named 'collect'`.

- [ ] **Step 7: Implement the collector**

Create `nodus/tuvm/zephyr/tools/console/collect.py`:

```python
#!/usr/bin/env python3
"""Persistent console collector — one process, one port, no parsing.

Design D3: a parser bug in the reader costs an answer; a parser bug in a
collector costs the evidence. So this program stamps bytes and writes them
down, and that is all it does.

Design D7: the heartbeat is written whether or not the device says anything,
because a log that stops looks identical to a board that went quiet. Every
downstream check asserts a *recent heartbeat present*, never an absence.
"""
import argparse
import datetime as dt
import errno
import fcntl
import os
import sys
import time

from lines import LineAssembler
from logstore import LogStore, MARKER, DEFAULT_ROOT, DEFAULT_BUDGET


class Busy(Exception):
    pass


def acquire_lock(root, board):
    os.makedirs(root, exist_ok=True)
    fh = open(os.path.join(root, "%s.lock" % board), "a+")
    try:
        fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        if exc.errno not in (errno.EACCES, errno.EAGAIN):
            raise
        fh.seek(0)
        holder = fh.read().strip() or "unknown pid"
        fh.close()
        raise Busy("another collector holds %s (%s). Two readers would split "
                   "the stream (design D6)." % (board, holder))
    fh.seek(0)
    fh.truncate()
    fh.write("pid=%d\n" % os.getpid())
    fh.flush()
    return fh


def _default_opener(port_path):
    def opener():
        import serial
        return serial.Serial(port_path, 115200, timeout=0.2)
    return opener


class Collector:
    def __init__(self, port_path, store, heartbeat_s=60.0, now=None, open_port=None):
        self.port_path = port_path
        self.store = store
        self.heartbeat_s = heartbeat_s
        self.now = now or (lambda: dt.datetime.now(dt.timezone.utc).astimezone())
        self.open_port = open_port or _default_opener(port_path)
        self.asm = LineAssembler()
        self.port = None
        self.started = False
        self._last_beat = None
        self._lost = False

    # -- writing -------------------------------------------------------
    def _write(self, text):
        self.store.append(text, self.now())

    def _mark(self, text):
        self._write("%s %s" % (MARKER, text))

    # -- the cycle -----------------------------------------------------
    def step(self):
        if not self.started:
            self._mark("collector started pid=%d port=%s" % (os.getpid(), self.port_path))
            self._last_beat = self.now()
            self.started = True

        if self.port is None:
            try:
                self.port = self.open_port()
            except Exception as exc:
                self._beat()
                if not self._lost:
                    self._mark("port lost: %s" % exc)
                    self._lost = True
                return
            if self._lost:
                self._mark("port reacquired")
                self._lost = False

        try:
            data = self.port.read(4096)
        except Exception as exc:
            self._mark("port lost: %s" % exc)
            self._lost = True
            try:
                self.port.close()
            except Exception:
                pass
            self.port = None
            self._beat()
            return

        for line in self.asm.feed(data):
            self._write(line)
        self._beat()

    def _beat(self):
        now = self.now()
        if (now - self._last_beat).total_seconds() >= self.heartbeat_s:
            self._mark("heartbeat")
            self._last_beat = now

    def run(self, stop=None):
        while not (stop and stop()):
            self.step()
            if self.port is None:
                time.sleep(1.0)     # do not spin on a missing port


def main(argv=None):
    ap = argparse.ArgumentParser(description="Persistent console collector (#56)")
    ap.add_argument("port", help="/dev/serial/by-id/... path, or a PTY slave in tests")
    ap.add_argument("--board", default="c6")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--budget-bytes", type=int, default=DEFAULT_BUDGET)
    ap.add_argument("--heartbeat", type=float, default=60.0)
    args = ap.parse_args(argv)

    store = LogStore(args.root, args.board, args.budget_bytes)
    try:
        lock = acquire_lock(args.root, args.board)
    except Busy as exc:
        print("refusing to start: %s" % exc, file=sys.stderr)
        return 3

    collector = Collector(args.port, store, heartbeat_s=args.heartbeat)
    try:
        collector.run()
    except KeyboardInterrupt:
        collector._mark("collector stopped")
    finally:
        for path in store.prune():
            collector._mark("pruned %s" % path)
        lock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 8: Run to verify they pass**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_collect.py test_lines.py test_logstore.py -v`
Expected: 25 passed.

- [ ] **Step 9: Commit**

```bash
git add nodus/tuvm/zephyr/tools/console/lines.py nodus/tuvm/zephyr/tools/console/collect.py \
        nodus/tuvm/zephyr/tools/console/test_lines.py nodus/tuvm/zephyr/tools/console/test_collect.py
git commit -m "feat(#56): console collector daemon — heartbeat, gap markers, one-handle lock

Parses nothing (design D3). NUL stripping is inherited from the untracked
magtag capture.py, which is where that lesson was learned. Port loss and
reacquire are written down as markers rather than left to be inferred from
a timestamp jump (§8), and a second instance exits 3 rather than silently
splitting the stream."
```

---

### Task 4: The systemd template unit

D2: always-on, `systemd --user`, `Linger=yes` already set for this user. A **template** unit so a second board is a second instantiation rather than a second file — that template is the D1 seam.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/rete-console@.service`
- Create: `nodus/tuvm/zephyr/tools/console/README.md`

**Interfaces:**
- Consumes: `collect.py` (Task 3) via `ExecStart`.
- Produces: the unit name `rete-console@<board>.service`, which Task 11's gate starts and stops by name.

- [ ] **Step 1: Write the unit**

Create `nodus/tuvm/zephyr/tools/console/rete-console@.service`:

```ini
# Persistent console collector, one instance per board (#56, design D2/D1).
#   systemctl --user enable --now rete-console@c6
# %i is the board name and selects both the port and the log file prefix.
[Unit]
Description=rete console collector (%i)
Documentation=file:%h/workspace/rete/docs/design/2026-08-03-device-console-collector-design.md

[Service]
Type=simple
Environment=RETE_CONSOLE_PORT_c6=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_ac2dce6f529ced118f43fd6be259fb3e-if00-port0
ExecStart=/usr/bin/python3 %h/workspace/rete/nodus/tuvm/zephyr/tools/console/collect.py ${RETE_CONSOLE_PORT_%i} --board %i
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
```

- [ ] **Step 2: Verify the unit parses**

```bash
mkdir -p ~/.config/systemd/user
cp nodus/tuvm/zephyr/tools/console/rete-console@.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemd-analyze --user verify rete-console@c6.service
```
Expected: no output (silence is a pass). Warnings about the `Documentation=` path are acceptable; errors are not.

- [ ] **Step 3: Confirm the by-id path in the unit is the console bridge, not the flash port**

```bash
ls -l /dev/serial/by-id/
```
Expected: the `CP2102N` symlink resolves to a `ttyUSB*` node, and the `Espressif_USB_JTAG` symlink resolves to a `ttyACM*` node (design §3). If the CP2102N serial hash differs from the one in the unit, correct the unit to match this bench — do not generalise the glob here; `run.sh`'s `pick_port` is where ambiguity is handled loudly.

- [ ] **Step 4: Start it and confirm it collects**

```bash
systemctl --user enable --now rete-console@c6
sleep 5
systemctl --user is-active rete-console@c6
tail -3 ~/.local/state/rete/console/console-c6-$(date +%F).log
```
Expected: `active`, and a log containing the `@@rete-console@@ collector started` marker.

- [ ] **Step 5: Write the README**

Create `nodus/tuvm/zephyr/tools/console/README.md`:

```markdown
# Console collector and offline reader (#56)

`collect.py` holds the C6 console port and writes everything it sees to
`~/.local/state/rete/console/console-<board>-YYYY-MM-DD.log`. `logread.py`
reads those logs offline. The collector parses nothing; the reader parses
everything (design D3).

## Install

    cp rete-console@.service ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user enable --now rete-console@c6

`Linger=yes` is already set for this user, so it starts at boot and survives
logout. Check it with `systemctl --user status rete-console@c6`.

## Why the collector holds the port permanently

On the C6 there is no conflict: flashing goes over the native USB JTAG port
(`ttyACM*`) and the console is a separate CP2102N bridge (`ttyUSB*`) that stays
enumerated across target resets (design §3, #57). Holding it costs nothing and
means a panic that fires before you thought to look is still captured — the
exact failure that lost the first AMG8833 panic.

This is **C6-only**. The MagTag and the nRF dongle both need design work first
(§10), and the plain esp32's CP210x port is both flash port and console, so a
permanent holder there would fight `--flash`.

## Everyday use

    ./logread.py tail -n 50
    ./logread.py runs
    ./logread.py panics
    ./logread.py symbolize --run 3

`--capture N` on `run.sh --board esp32c6` is now a read of this log, not a
serial open. It shows the boot you just triggered even if it happened before
you asked.
```

- [ ] **Step 6: Commit**

```bash
git add nodus/tuvm/zephyr/tools/console/rete-console@.service nodus/tuvm/zephyr/tools/console/README.md
git commit -m "feat(#56): systemd --user template unit for the console collector

A template rather than a unit file so a second board is a second
instantiation (design D1/D2). The by-id path is the CP2102N console
bridge, not the JTAG flash port — the split #57 chose is what makes an
always-on collector possible here at all."
```

---

### Task 5: Build id bake and boot banner

§5.4. CMake bakes `git describe --always --dirty` plus a build timestamp into a generated TU, forced to rebuild every build, and the firmware prints it before the VM starts. This is #74's mechanism minus its nsl-side accessor.

D5 is settled: **not** GNU build-id. That was tried on hardware and rejected — a `SHT_NOTE` input section makes GNU ld synthesise a `PT_NOTE` over the whole rodata region and the Espressif image builder mishandles it (§7). Do not re-attempt it.

**Files:**
- Create: `nodus/tuvm/zephyr/cmake/gen_build_id.cmake`
- Modify: `nodus/tuvm/zephyr/CMakeLists.txt`
- Modify: `nodus/tuvm/zephyr/src/main.c:1643` (inside `main()`, before `kernel_boot()`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `extern const char tuvm_build_id[];` — the key, format `<describe>-<YYYYMMDDTHHMMSS>`.
  - Console line: `tuvm: build-id <key>` — Task 6 archives under this key, Task 9's reader splits runs on it.
  - `${CMAKE_CURRENT_BINARY_DIR}/tuvm_build_id.txt` — the same key on disk, so `run.sh` reads it rather than recomputing a timestamp that would differ.

- [ ] **Step 1: Write the generator script**

Create `nodus/tuvm/zephyr/cmake/gen_build_id.cmake`:

```cmake
# Bakes the build id: git describe + a build timestamp. Run at BUILD time (not
# configure time) so the timestamp is the build's, and re-run every build.
# Writes two artefacts with identical content: a C TU for the firmware to print,
# and a .txt for run.sh to read when archiving the ELF. run.sh must never
# recompute the timestamp — it would differ, and the key would not match.
execute_process(
  COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${SRC}
  OUTPUT_VARIABLE DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT DESCRIBE)
  set(DESCRIBE "nogit")
endif()
string(TIMESTAMP STAMP "%Y%m%dT%H%M%S")
set(BUILD_ID "${DESCRIBE}-${STAMP}")

file(WRITE ${OUT_C}
     "/* generated by cmake/gen_build_id.cmake — do not edit, do not commit */\n"
     "const char tuvm_build_id[] = \"${BUILD_ID}\";\n")
file(WRITE ${OUT_TXT} "${BUILD_ID}\n")
```

- [ ] **Step 2: Wire it into the build**

In `nodus/tuvm/zephyr/CMakeLists.txt`, after the `target_sources(app PRIVATE ...)` block that begins with `src/main.c`, add:

```cmake
# Build id (#56 §5.4, part of #74). OUTPUT lists a file the command never
# creates, which is what forces the command to re-run on every build — the
# timestamp must be this build's, not the first build's.
set(TUVM_BUILD_ID_C   ${CMAKE_CURRENT_BINARY_DIR}/tuvm_build_id.c)
set(TUVM_BUILD_ID_TXT ${CMAKE_CURRENT_BINARY_DIR}/tuvm_build_id.txt)
add_custom_command(
  OUTPUT ${TUVM_BUILD_ID_C} ${TUVM_BUILD_ID_TXT} ${CMAKE_CURRENT_BINARY_DIR}/build_id.always
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT_C=${TUVM_BUILD_ID_C}
          -DOUT_TXT=${TUVM_BUILD_ID_TXT}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
  COMMENT "Baking tuvm build id")
add_custom_target(tuvm_build_id_gen DEPENDS ${TUVM_BUILD_ID_C})
target_sources(app PRIVATE ${TUVM_BUILD_ID_C})
add_dependencies(app tuvm_build_id_gen)
```

- [ ] **Step 3: Print it before the VM starts**

In `nodus/tuvm/zephyr/src/main.c`, add near the other externs above `main()`:

```c
/* Baked by cmake/gen_build_id.cmake, regenerated every build (#56 §5.4). */
extern const char tuvm_build_id[];
```

Then inside `main()`, immediately after the `#if DT_NODE_HAS_COMPAT(...) k_sleep(K_SECONDS(3)); #endif` block and **before** `gc_additional_roots_hook = oevm_roots_callback;`, add:

```c
    /* Before anything that can die: a VM that faults on the first instruction
       must still leave a log segment naming the binary it faulted in (#56 §5.4).
       printk, not printf — the fault path gags newlib stdio. */
    printk("tuvm: build-id %s\n", tuvm_build_id);
```

The `k_sleep` above it is deliberate: on USB-CDC-console boards the banner must land after enumeration or it is lost, which is the same "Issue A" mitigation the surrounding comment describes.

- [ ] **Step 4: Build and verify the key is in the image**

```bash
cd nodus/tuvm/zephyr
./run.sh --board esp32c6 --pristine
cat build-esp32c6-B/tuvm_build_id.txt
strings build-esp32c6-B/zephyr/zephyr.elf | grep -c "$(cat build-esp32c6-B/tuvm_build_id.txt)"
```
Expected: a key like `dda1682-20260803T174501`, and a `strings` count of at least 1 — the key is a rodata literal, so it is recoverable from the ELF (§7 established this, and it is why the archive is repairable).

- [ ] **Step 5: Verify it is regenerated on an incremental build**

```bash
sleep 2
./run.sh --board esp32c6
cat build-esp32c6-B/tuvm_build_id.txt
```
Expected: a **different** timestamp from Step 4. If it is identical, the `.always` OUTPUT trick did not take and the archive would silently key two different binaries to one id — stop and fix before continuing.

- [ ] **Step 6: Commit**

```bash
git add nodus/tuvm/zephyr/cmake/gen_build_id.cmake nodus/tuvm/zephyr/CMakeLists.txt nodus/tuvm/zephyr/src/main.c
git commit -m "feat(#56,#74): bake a build id and print it before the VM starts

git describe + build timestamp, regenerated every build, written both as a
C literal for the firmware and as a .txt for run.sh so the two never
disagree about the key. Printed before kernel_boot() so a VM that dies
immediately still leaves a log segment naming its binary (design §5.4).

Not GNU build-id: that was tried on hardware and rejected — any SHT_NOTE
input section makes GNU ld synthesise a PT_NOTE over the whole rodata
region and the Espressif image builder mishandles it, producing a board
that boots and then emits garbage (design §7). #74 stays open for the
nsl-side accessor."
```

---

### Task 6: `run.sh` — console port, ELF archive, and `--capture` as a log read

Three changes, all C6-only (D1/D6). The `esp32` and `esp32s2` paths must be untouched.

**Files:**
- Modify: `nodus/tuvm/zephyr/run.sh` — the C6 arm of the `case` at `:196-200`, the flash block at `:302-307`, and the capture block at `:309-312`.
- Create: `nodus/tuvm/zephyr/tools/console/archive.sh`

**Interfaces:**
- Consumes: `build-esp32c6-B/tuvm_build_id.txt` (Task 5), `logread.py tail` (Task 9 — see the step ordering note below).
- Produces:
  - `~/.local/state/rete/elf/<build-id>/zephyr.elf`
  - `~/.local/state/rete/elf/<build-id>/zephyr.bin.sha256`
  - `~/.local/state/rete/elf/last-flashed-c6` — one line, the key just flashed.

**Ordering note:** `--capture` cannot become a log read until `logread.py` exists (Task 9). Do Steps 1–5 now; Step 6 is marked and revisited after Task 9. Until then `--capture` keeps its current behaviour on all three boards, which is a working state to leave the tree in.

- [ ] **Step 1: Name the console port for the C6**

In `run.sh`, in the `esp32c6)` arm, immediately after the existing `pick_port '/dev/serial/by-id/*Espressif*' /dev/ttyACM0` line at `:200`, add:

```bash
    # The console is a DIFFERENT physical port from the flash port (#57, #56 §3):
    # flash goes over the native USB JTAG bridge above, console over the CP2102N,
    # which stays enumerated across target resets. Naming it here is what lets the
    # collector and --flash coexist. Same zero/one/many discipline as pick_port:
    # this bench has more than one CP210x device and guessing is what clobbered
    # the jag board on 2026-07-16.
    CONSOLE_PORT="${CONSOLE_PORT:-}"
    if [ -z "$CONSOLE_PORT" ]; then
      cmatches=$(ls /dev/serial/by-id/*CP2102N* 2>/dev/null || true)
      cn=$(printf '%s\n' "$cmatches" | grep -c . || true)
      if [ "$cn" -gt 1 ]; then
        echo "more than one CP2102N console candidate:" >&2
        printf '%s\n' "$cmatches" >&2
        echo "Set CONSOLE_PORT explicitly and retry." >&2
        exit 2
      elif [ "$cn" -eq 1 ]; then
        CONSOLE_PORT="$cmatches"
      else
        CONSOLE_PORT=/dev/ttyUSB0
      fi
    fi
```

- [ ] **Step 2: Verify port resolution without touching the board**

```bash
cd nodus/tuvm/zephyr
bash -n run.sh && echo "SYNTAX OK"
ls /dev/serial/by-id/*CP2102N* 2>/dev/null
```
Expected: `SYNTAX OK`, and exactly one CP2102N symlink listed. If more than one appears, the abort path above is correct behaviour — set `CONSOLE_PORT` for subsequent steps.

- [ ] **Step 3: Write the archive helper**

Create `nodus/tuvm/zephyr/tools/console/archive.sh`:

```bash
#!/usr/bin/env bash
# Archive the ELF that was just flashed, keyed by its build id (#56 D4/§5.3).
# Called from run.sh as part of --flash, never as a separate step: an archive
# you have to remember to update is an archive that is wrong when it matters.
# Usage: archive.sh <build-dir> <board>
set -euo pipefail
BUILD_DIR="$1"; BOARD="$2"
ROOT="${RETE_ELF_ROOT:-$HOME/.local/state/rete/elf}"

ID_TXT="$(find "$BUILD_DIR" -name tuvm_build_id.txt -print -quit)"
[ -n "$ID_TXT" ] || { echo "archive: no tuvm_build_id.txt under $BUILD_DIR" >&2; exit 1; }
BUILD_ID="$(cat "$ID_TXT")"

DEST="$ROOT/$BUILD_ID"
mkdir -p "$DEST"
cp "$BUILD_DIR/zephyr/zephyr.elf" "$DEST/zephyr.elf"
sha256sum "$BUILD_DIR/zephyr/zephyr.bin" | awk '{print $1}' > "$DEST/zephyr.bin.sha256"

# A human-facing note only. The reader must NEVER consult this automatically:
# if it did, D4's refusal would be a fallback wearing a disguise, and a board
# flashed twice since the fault would hand back a confident wrong answer.
echo "$BUILD_ID" > "$ROOT/last-flashed-$BOARD"
echo "[archive] $BUILD_ID -> $DEST"
```

Then: `chmod +x nodus/tuvm/zephyr/tools/console/archive.sh`

- [ ] **Step 4: Call it from the flash path**

In `run.sh`, inside the `if` that performs the flash (the block ending at `:307`), after the `"$ESPTOOL" ... write_flash ...` invocation and still inside the `if`, add:

```bash
  if [ "$BOARD_ID" = esp32c6 ]; then
    "$ZDIR/tools/console/archive.sh" "$ZDIR/$BUILD_DIR" c6
  fi
```

- [ ] **Step 5: Flash and verify the archive**

```bash
cd nodus/tuvm/zephyr
./run.sh --board esp32c6 --flash
ID=$(cat build-esp32c6-B/tuvm_build_id.txt)
ls -la ~/.local/state/rete/elf/"$ID"/
cat ~/.local/state/rete/elf/last-flashed-c6
```
Expected: `zephyr.elf` and `zephyr.bin.sha256` present under the key, and `last-flashed-c6` holding the same key.

Then confirm the banner reached the console — the collector from Task 4 should already have it:

```bash
grep 'build-id' ~/.local/state/rete/console/console-c6-$(date +%F).log | tail -2
```
Expected: a line ending in the same key. **This is the first end-to-end proof of the whole design** — the board named its own binary, and the host filed that binary under the same string, with no human step between them.

- [ ] **Step 6: (AFTER TASK 9) Redefine `--capture` for the C6 only**

Replace the capture block at `run.sh:309-312`:

```bash
if [ "${CAPTURE:-0}" != 0 ]; then
  if [ "$BOARD_ID" = esp32c6 ]; then
    # D6: the collector owns the port, so --capture is a READ of what it
    # already has. This gains rather than loses — it shows the boot you just
    # triggered even if it fired before you asked, which is the exact failure
    # that lost the first AMG8833 panic.
    echo "[watch] last ${CAPTURE}s from the collector log"
    python3 "$ZDIR/tools/console/logread.py" since "-${CAPTURE}s"
  else
    # esp32 / esp32s2 keep the direct serial read, including its DTR/RTS reset
    # pulse (capture_coredump.py:9), which is a no-op on the C6 but real here.
    # They get a collector when they get a design (#56 §10).
    echo "[watch] ${CAPTURE}s"
    python3 "$ZDIR/capture_coredump.py" "$CAPTURE"
  fi
fi
```

- [ ] **Step 7: Commit**

```bash
git add nodus/tuvm/zephyr/run.sh nodus/tuvm/zephyr/tools/console/archive.sh
git commit -m "feat(#56): name the C6 console port, archive the ELF on every flash

The console port existed nowhere in run.sh — it was a hardcode in
capture_coredump.py:5. It is named here, with the same zero/one/many
discipline pick_port uses, because this bench has more than one CP210x.

Archiving happens as part of --flash rather than as a step someone has to
remember (design §5.3), keyed by the build id the firmware itself prints,
read from tuvm_build_id.txt so host and device cannot disagree. The
last-flashed pointer is human-facing only.

esp32 and esp32s2 paths are untouched (D1)."
```

---

### Task 7: The deliberate fault trigger

D8. A prim that faults on purpose, compiled in only under a Kconfig defaulting to `n`. It exists so the symbolizing path is exercised by something other than the incident it was built to explain — and, per the ordering note at the top of this plan, it is the **only** way to produce the real C6 fault capture Tasks 9–10 need as a fixture.

**Files:**
- Modify: `nodus/tuvm/zephyr/Kconfig`
- Create: `nodus/tuvm/zephyr/prj_esp32c6_faultgate.conf`
- Modify: `nodus/tuvm/zephyr/src/main.c`

**Interfaces:**
- Consumes: nothing.
- Produces: `CONFIG_TUVM_FAULT_TRIGGER`; a boot-time fault at a known source line when set.

- [ ] **Step 1: Add the Kconfig symbol**

In `nodus/tuvm/zephyr/Kconfig`, before the final `source "Kconfig.zephyr"`, add:

```
# #56 D8. Default n so it is not merely unreachable in a normal build but
# absent from it. Turned on by prj_esp32c6_faultgate.conf (run.sh --conf-extra),
# which also enables the coredump backend — a fault with no #CD: lines is a
# banner, not evidence. Same shape as TUVM_NET_HEARTBEAT above.
config TUVM_FAULT_TRIGGER
	bool "Fault on purpose at boot, to exercise the symbolizing path (#56)"
	default n
	help
	  Dereferences a null pointer from a known source line shortly after boot.
	  Exists so `logread.py symbolize` is proven by a scripted gate rather than
	  by the next real incident. If this is set in a build you did not
	  deliberately configure, that build is wrong.
```

- [ ] **Step 2: Add the overlay**

Create `nodus/tuvm/zephyr/prj_esp32c6_faultgate.conf`:

```
# The one C6 build that faults on purpose (#56 D8/§9).
#   ./run.sh --board esp32c6 --conf-extra prj_esp32c6_faultgate.conf --flash
# Never the default build. prj_esp32c6.conf has no coredump config at all, so
# the backend is enabled here too: without it a fault prints a fatal-error
# banner and nothing decodable. MEMORY_DUMP_MIN keeps the serial dump short —
# registers plus the faulting thread's stack, which is what a backtrace needs.
# Decode offline with zephyr/scripts/coredump/, same as coredump.conf documents
# for the ESP32.
CONFIG_TUVM_FAULT_TRIGGER=y
CONFIG_DEBUG_COREDUMP=y
CONFIG_DEBUG_COREDUMP_BACKEND_LOGGING=y
CONFIG_DEBUG_COREDUMP_MEMORY_DUMP_MIN=y
```

- [ ] **Step 3: Add the trigger**

In `nodus/tuvm/zephyr/src/main.c`, immediately after the `printk("tuvm: build-id %s\n", tuvm_build_id);` line added in Task 5, add:

```c
#ifdef CONFIG_TUVM_FAULT_TRIGGER
    /* #56 D8: fault on purpose, from a line whose number is the thing under
       test. `volatile` so the compiler cannot decide this is UB it may delete.
       Placed after the build-id banner so the run is attributable — a fault
       whose log segment does not name a binary is exactly what D4 refuses. */
    printk("tuvm: faulting on purpose (CONFIG_TUVM_FAULT_TRIGGER)\n");
    {
        volatile int *tuvm_fault_target = (volatile int *)0;
        *tuvm_fault_target = 1;
    }
#endif
```

Record the file and line of the `*tuvm_fault_target = 1;` statement — Task 10's assertion is that symbolizing resolves to it.

- [ ] **Step 4: Verify the default build does NOT contain it**

```bash
cd nodus/tuvm/zephyr
./run.sh --board esp32c6 --pristine
grep -c 'CONFIG_TUVM_FAULT_TRIGGER=y' build-esp32c6-B/zephyr/.config || echo "ABSENT (correct)"
grep -c 'faulting on purpose' build-esp32c6-B/zephyr/zephyr.map || echo "not linked (correct)"
```
Expected: `ABSENT (correct)`. A default build that contains the trigger is a failed task.

- [ ] **Step 5: Build the fault image and verify it IS in that one**

```bash
./run.sh --board esp32c6 --conf-extra prj_esp32c6_faultgate.conf --pristine
grep 'CONFIG_TUVM_FAULT_TRIGGER' build-esp32c6-B/zephyr/.config
grep 'CONFIG_DEBUG_COREDUMP=' build-esp32c6-B/zephyr/.config
```
Expected: both `=y`.

- [ ] **Step 6: Commit**

```bash
git add nodus/tuvm/zephyr/Kconfig nodus/tuvm/zephyr/prj_esp32c6_faultgate.conf nodus/tuvm/zephyr/src/main.c
git commit -m "feat(#56): deliberate fault trigger, Kconfig-gated, default n

Design D8. Absent from a normal build, not merely unreachable in one. The
overlay also enables the coredump backend, which prj_esp32c6.conf does not
have at all — without it a C6 fault prints a banner and nothing decodable.

This lands before the fault parser it exercises because it is the only way
to produce a real C6 fault capture, and the alternative is writing a parser
against an invented format."
```

---

### Task 8: Capture the real fixtures on hardware

The fixtures §9 requires do not exist. This task produces them. It is a hardware task with no code, and it gates Tasks 9 and 10.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/fixtures/c6-boot.log`
- Create: `nodus/tuvm/zephyr/tools/console/fixtures/c6-fault.log`
- Create: `nodus/tuvm/zephyr/tools/console/fixtures/README.md`

**Interfaces:**
- Consumes: Tasks 4, 5, 6, 7 — collector running, banner printing, archive filling, fault image buildable.
- Produces: two real logs in the collector's own on-disk format, which Tasks 9 and 10 parse.

- [ ] **Step 1: Confirm the collector is up and the log is live**

```bash
systemctl --user is-active rete-console@c6
tail -2 ~/.local/state/rete/console/console-c6-$(date +%F).log
```
Expected: `active`, and a heartbeat or device line stamped within the last 60 s.

- [ ] **Step 2: Flash a normal build and capture a clean boot**

```bash
cd nodus/tuvm/zephyr
./run.sh --board esp32c6 --pristine --flash
sleep 20
```

- [ ] **Step 3: Flash the fault build and capture a real fault**

```bash
./run.sh --board esp32c6 --conf-extra prj_esp32c6_faultgate.conf --pristine --flash
sleep 30
```

- [ ] **Step 4: Extract both fixtures from the collector log**

```bash
mkdir -p tools/console/fixtures
LOG=~/.local/state/rete/console/console-c6-$(date +%F).log
grep -n 'build-id' "$LOG" | tail -4     # find the two run boundaries
```

Using the line numbers from that output, slice the clean-boot run into `tools/console/fixtures/c6-boot.log` and the faulting run into `tools/console/fixtures/c6-fault.log`, **preserving the collector's stamps and markers verbatim**. Do not hand-edit the content. Each fixture must begin at its `tuvm: build-id` line and run to the end of that run.

- [ ] **Step 5: Verify the fault fixture actually contains a decodable dump**

```bash
grep -c '#CD:' tools/console/fixtures/c6-fault.log
grep -c 'build-id' tools/console/fixtures/c6-fault.log
```
Expected: a `#CD:` count well above zero and exactly one `build-id`. **If the `#CD:` count is zero, stop.** The fault reached the console but the coredump backend did not produce a dump, and no amount of host-side code will fix that — revisit Task 7's overlay before continuing. A driver failing silently for want of config is a documented way this bench lies (§9).

- [ ] **Step 6: Verify the archive has the matching ELF**

```bash
ID=$(grep -m1 'build-id' tools/console/fixtures/c6-fault.log | sed 's/.*build-id //')
ls ~/.local/state/rete/elf/"$ID"/zephyr.elf
```
Expected: the file exists. If it does not, the archive step in Task 6 did not fire on this flash — fix that before continuing, because Task 10 has nothing to symbolize against.

- [ ] **Step 7: Write the fixtures README and commit**

Create `nodus/tuvm/zephyr/tools/console/fixtures/README.md`:

```markdown
# Golden fixtures

Real captures from the bench C6, in the collector's own on-disk format —
stamps and `@@rete-console@@` markers included. Not hand-written, not
trimmed beyond run boundaries.

- `c6-boot.log` — a clean boot of a default build.
- `c6-fault.log` — a boot of a `prj_esp32c6_faultgate.conf` build, faulting
  on purpose, with the `#CD:` coredump lines that follow.

They exist because the reader's parsers rot when upstream Zephyr changes a
banner or a fault format, and a fixture is the only thing that notices
(design §8). Regenerate them from hardware — never edit them to make a test
pass. A test that only passes against an edited fixture is measuring the
edit.

The matching ELF for `c6-fault.log` is in the archive under the build id on
its `tuvm: build-id` line. If it has been pruned, the fault fixture can still
be parsed; only `symbolize` needs the ELF.
```

```bash
git add nodus/tuvm/zephyr/tools/console/fixtures/
git commit -m "test(#56): real C6 boot and fault captures as golden fixtures

Produced by the D8 trigger on hardware, in the collector's own format. The
repo had real ESP32 fault dumps and real C6 boot logs but never a C6 fault,
because nothing had ever faulted one on purpose."
```

---

### Task 9: The offline reader

§5.2. All parsing lives here. Written against the Task 8 fixtures, so every regex is answerable by a real capture rather than by a guess.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/logread.py`
- Test: `nodus/tuvm/zephyr/tools/console/test_logread.py`

**Interfaces:**
- Consumes: `logstore.LogStore`, `logstore.MARKER` (Task 2); the fixtures (Task 8).
- Produces:
  - `parse_line(line: str) -> tuple[datetime, str]` — raises `ValueError` on an unstamped line.
  - `class Run` with `.build_id: str | None`, `.start: datetime`, `.lines: list[tuple[datetime, str]]`, `.index: int`.
  - `split_runs(entries) -> list[Run]`
  - `find_panics(run) -> list[Panic]`, `class Panic` with `.start: datetime`, `.cd_lines: list[str]` (hex payloads, `#CD:` prefix and ANSI already stripped).
  - CLI: `logread.py tail [-n N]`, `since <spec>`, `runs`, `panics`.
  - `since` accepts both an ISO-8601 timestamp and a relative `-<N>s` / `-<N>m` form — `run.sh --capture` passes `-<N>s`.

- [ ] **Step 1: Write the failing tests against the real fixtures**

Create `nodus/tuvm/zephyr/tools/console/test_logread.py`:

```python
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


def test_run_carries_the_build_id_the_device_printed():
    run = logread.split_runs(entries(BOOT))[0]
    assert run.build_id and run.build_id != "unknown"


def test_fault_fixture_has_a_panic():
    run = logread.split_runs(entries(FAULT))[0]
    assert logread.find_panics(run)


def test_panic_carries_coredump_payload():
    run = logread.split_runs(entries(FAULT))[0]
    panic = logread.find_panics(run)[0]
    assert panic.cd_lines
    assert all(c in "0123456789abcdefABCDEF" for c in "".join(panic.cd_lines))


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


def test_since_accepts_an_iso_timestamp():
    got = logread.resolve_since("2026-08-03T09:00:00-07:00")
    assert got.hour == 9 and got.tzinfo is not None
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_logread.py -v`
Expected: `ModuleNotFoundError: No module named 'logread'`.

- [ ] **Step 3: Inspect the fixtures before writing a single regex**

```bash
cd nodus/tuvm/zephyr/tools/console
grep -n 'build-id\|Booting Zephyr\|ESP-ROM' fixtures/c6-boot.log | head
grep -n 'FATAL ERROR\|#CD:BEGIN\|#CD:END' fixtures/c6-fault.log | head
```

Write the run-boundary and panic regexes to match **what you see here**, not what this plan guesses. If the fixture disagrees with anything below, the fixture wins.

- [ ] **Step 4: Implement the reader**

Create `nodus/tuvm/zephyr/tools/console/logread.py`:

```python
#!/usr/bin/env python3
"""Offline reader over the collector's logs. All parsing lives here (design D3).

Re-runnable over history and testable with no board, which is the whole point
of keeping the collector dumb.
"""
import argparse
import datetime as dt
import os
import re
import sys

from logstore import LogStore, MARKER, DEFAULT_ROOT

STAMP_RE = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2}) (.*)$")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
BUILD_ID_RE = re.compile(r"tuvm: build-id (\S+)")
BOOT_RE = re.compile(r"ESP-ROM:esp32c6|\*\*\* Booting Zephyr OS")
FATAL_RE = re.compile(r"ZEPHYR FATAL ERROR")
CD_RE = re.compile(r"#CD:([0-9a-fA-F]*)")
CD_BEGIN_RE = re.compile(r"#CD:BEGIN#")
CD_END_RE = re.compile(r"#CD:END#")


class NoRuns(Exception):
    """A non-empty log that yielded no runs. An error, not an empty success."""


def parse_line(line):
    m = STAMP_RE.match(line)
    if not m:
        raise ValueError("unstamped line (not written by this collector): %r" % line[:60])
    return dt.datetime.fromisoformat(m.group(1)), m.group(2)


def resolve_since(spec, now=None):
    now = now or dt.datetime.now(dt.timezone.utc).astimezone()
    m = re.fullmatch(r"-(\d+)([sm])", spec)
    if m:
        n = int(m.group(1))
        delta = dt.timedelta(seconds=n) if m.group(2) == "s" else dt.timedelta(minutes=n)
        return now - delta
    got = dt.datetime.fromisoformat(spec)
    if got.tzinfo is None:
        got = got.replace(tzinfo=now.tzinfo)
    return got


class Panic:
    def __init__(self, start, cd_lines):
        self.start = start
        self.cd_lines = cd_lines


class Run:
    def __init__(self, index, start, build_id, lines):
        self.index = index
        self.start = start
        self.build_id = build_id
        self.lines = lines

    def __repr__(self):
        return "<Run %d %s %s %d lines>" % (
            self.index, self.start.isoformat(timespec="seconds"),
            self.build_id or "NO BUILD ID", len(self.lines))


def split_runs(entries, strict=False):
    """Split at the boot banner. The build id is attached from the tuvm line
    that follows it — a run that faulted before printing one keeps build_id
    None, which is what makes D4 refuse rather than guess."""
    runs, cur = [], None
    for when, text in entries:
        clean = ANSI_RE.sub("", text)
        if BOOT_RE.search(clean):
            cur = Run(len(runs) + 1, when, None, [])
            runs.append(cur)
        if cur is None:
            continue
        cur.lines.append((when, clean))
        if cur.build_id is None:
            m = BUILD_ID_RE.search(clean)
            if m:
                cur.build_id = m.group(1)
    if strict and entries and not runs:
        raise NoRuns("%d lines, zero runs — the banner pattern no longer "
                     "matches what the device prints" % len(entries))
    return runs


def find_panics(run):
    panics, cur, collecting = [], None, False
    for when, text in run.lines:
        if FATAL_RE.search(text):
            cur = Panic(when, [])
            panics.append(cur)
        if CD_BEGIN_RE.search(text):
            collecting = True
            if cur is None:
                cur = Panic(when, [])
                panics.append(cur)
            continue
        if CD_END_RE.search(text):
            collecting = False
            continue
        if collecting and cur is not None:
            m = CD_RE.search(text)
            if m and m.group(1):
                cur.cd_lines.append(m.group(1))
    return panics


def load(root, board, since=None):
    store = LogStore(root, board)
    out = []
    for day in store.days():
        if since and day < since.date():
            continue
        with open(store.path_for(day)) as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                try:
                    when, text = parse_line(line)
                except ValueError:
                    continue
                if since and when < since:
                    continue
                out.append((when, text))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Offline console log reader (#56)")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--board", default="c6")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_tail = sub.add_parser("tail"); p_tail.add_argument("-n", type=int, default=20)
    p_since = sub.add_parser("since"); p_since.add_argument("spec")
    sub.add_parser("runs")
    sub.add_parser("panics")
    args = ap.parse_args(argv)

    if args.cmd == "tail":
        for when, text in load(args.root, args.board)[-args.n:]:
            print("%s %s" % (when.isoformat(timespec="milliseconds"), text))
        return 0
    if args.cmd == "since":
        for when, text in load(args.root, args.board, resolve_since(args.spec)):
            print("%s %s" % (when.isoformat(timespec="milliseconds"), text))
        return 0

    entries = load(args.root, args.board)
    try:
        runs = split_runs(entries, strict=True)
    except NoRuns as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 4

    if args.cmd == "runs":
        for run in runs:
            print(run)
        return 0
    if args.cmd == "panics":
        found = 0
        for run in runs:
            for panic in find_panics(run):
                found += 1
                print("run %d  %s  build-id %s  %d coredump lines" % (
                    run.index, panic.start.isoformat(timespec="seconds"),
                    run.build_id or "NONE — symbolize will refuse", len(panic.cd_lines)))
        if not found:
            print("no panics in %d runs" % len(runs))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 5: Run the tests**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_logread.py -v`
Expected: 11 passed. If a fixture-driven test fails, fix the **regex**, never the fixture.

- [ ] **Step 6: Exercise the CLI against the live log**

```bash
cd nodus/tuvm/zephyr/tools/console
./logread.py runs
./logread.py panics
./logread.py since -300s | tail -5
```
Expected: `runs` lists real boots with build ids; `panics` names at least the deliberate one from Task 8.

- [ ] **Step 7: Complete Task 6 Step 6 now that `logread.py` exists**

Go back and apply Task 6 Step 6 (the `--capture` redefinition), then:

```bash
cd nodus/tuvm/zephyr
bash -n run.sh && echo "SYNTAX OK"
./run.sh --board esp32c6 --capture 30
```
Expected: the last 30 s of the collector log, printed without opening the serial port.

Then prove the other boards did not change:

```bash
grep -n 'capture_coredump.py' run.sh
```
Expected: still called, inside the non-C6 branch.

- [ ] **Step 8: Commit**

```bash
git add nodus/tuvm/zephyr/tools/console/logread.py nodus/tuvm/zephyr/tools/console/test_logread.py nodus/tuvm/zephyr/run.sh
git commit -m "feat(#56): offline log reader — runs, panics, tail, since

Every pattern here is answerable by a real capture in fixtures/, not by a
guess about what the device prints. Zero runs in a non-empty log is an
error (exit 4), not an empty success — #110's lesson applied to the tool
built to prevent the next #110.

A run that faulted before printing its build id keeps build_id None, which
is what lets D4 refuse instead of guessing.

--capture on the C6 is now a read of the collector log (D6); esp32 and
esp32s2 keep capture_coredump.py and its reset pulse."
```

---

### Task 10: Symbolize, and the D4 refusal

§5.2, D4, D4a. Resolves a panic against **that run's** archived ELF, refuses on a miss, and offers exactly one named escape hatch.

Uses Zephyr's own `scripts/coredump/coredump_serial_log_parser.py` to turn `#CD:` lines into a core file — RISC-V is supported both device-side (`arch/riscv/core/coredump.c`) and host-side (`gdbstubs/arch/risc_v.py`), both checked.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/symbolize.py`
- Modify: `nodus/tuvm/zephyr/tools/console/logread.py` (add the `symbolize` subcommand)
- Test: `nodus/tuvm/zephyr/tools/console/test_symbolize.py`

**Interfaces:**
- Consumes: `logread.Run`, `logread.Panic`, `logread.find_panics` (Task 9); the archive from Task 6.
- Produces:
  - `class Refused(Exception)`
  - `archive_path(build_id, root=None) -> str` — raises `Refused` if absent.
  - `symbolize_panic(run, panic, assume_build=None, root=None, workdir=None) -> str`
  - CLI: `logread.py symbolize [--run N] [--assume-build KEY]`

- [ ] **Step 1: Write the failing tests**

Create `nodus/tuvm/zephyr/tools/console/test_symbolize.py`:

```python
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
    with pytest.raises(Exception) as exc:
        symbolize.symbolize_panic(run, panic, assume_build="KEY", root=str(tmp_path))
    assert not isinstance(exc.value, symbolize.Refused)


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
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_symbolize.py -v`
Expected: `ModuleNotFoundError: No module named 'symbolize'`.

- [ ] **Step 3: Implement**

Create `nodus/tuvm/zephyr/tools/console/symbolize.py`:

```python
#!/usr/bin/env python3
"""Resolve a panic against the ELF that produced it — or refuse (design D4).

Symbolizing a pre-reflash panic against the current build dir yields
confident, precise, wrong locations. That is worse than no answer, because it
looks like one. So there is no fallback: the run names a build id, the archive
either has it or the tool stops.

The one exception (D4a) must be typed by a human and is echoed into every line
of output, so the assumption travels with the answer.
"""
import os
import subprocess
import sys
import tempfile
import time

DEFAULT_ROOT = os.path.expanduser("~/.local/state/rete/elf")
ZEPHYR_BASE = os.environ.get("ZEPHYR_BASE", os.path.expanduser("~/zephyr-esp32/zephyr"))


class Refused(Exception):
    """No trustworthy ELF. Deliberately not a fallback."""


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
            "this tool will not." % (build_id, root, os.path.join(root, "last-flashed-<board>")))
    return elf


def _core_from_panic(panic, workdir):
    """Zephyr's own parser turns the #CD: hex back into a core file."""
    log = os.path.join(workdir, "panic.log")
    core = os.path.join(workdir, "core.bin")
    with open(log, "w") as fh:
        fh.write("#CD:BEGIN#\n")
        for chunk in panic.cd_lines:
            fh.write("#CD:%s\n" % chunk)
        fh.write("#CD:END#\n")
    parser = os.path.join(ZEPHYR_BASE, "scripts", "coredump", "coredump_serial_log_parser.py")
    subprocess.run([sys.executable, parser, log, core], check=True)
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
    core = _core_from_panic(panic, workdir)

    gdb = os.environ.get("RETE_GDB", "riscv64-zephyr-elf-gdb")
    server = os.path.join(ZEPHYR_BASE, "scripts", "coredump", "coredump_gdbserver.py")
    proc = subprocess.Popen([sys.executable, server, elf, core],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)      # let the stub bind :1234 before gdb dials it
        out = subprocess.run(
            [gdb, elf, "-batch",
             "-ex", "target remote localhost:1234",
             "-ex", "bt"],
            capture_output=True, text=True, timeout=60).stdout
    finally:
        proc.terminate()
        proc.wait(timeout=10)

    prefix = ("[ASSUMED build-id %s] " % key) if assume_build else ""
    return "\n".join(prefix + line for line in out.splitlines())
```

- [ ] **Step 4: Add the CLI subcommand**

In `logread.py`'s `main()`, add to the subparsers:

```python
    p_sym = sub.add_parser("symbolize")
    p_sym.add_argument("--run", type=int, default=None, help="run index; default is the last run with a panic")
    p_sym.add_argument("--assume-build", default=None,
                       help="name the build id yourself when the run never printed one (D4a)")
```

and, before the final `return 2`:

```python
    if args.cmd == "symbolize":
        import symbolize as sym
        targets = [r for r in runs if (args.run is None or r.index == args.run)]
        for run in reversed(targets):
            panics = find_panics(run)
            if not panics:
                continue
            try:
                print(sym.symbolize_panic(run, panics[-1], assume_build=args.assume_build))
            except sym.Refused as exc:
                print("refused: %s" % exc, file=sys.stderr)
                return 5
            return 0
        print("no panic to symbolize", file=sys.stderr)
        return 1
```

- [ ] **Step 5: Run the tests**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest -v`
Expected: all tests pass across all five test files.

- [ ] **Step 6: Symbolize the real fault**

```bash
cd nodus/tuvm/zephyr/tools/console
./logread.py symbolize
```
Expected: a backtrace naming `src/main.c` at the `*tuvm_fault_target = 1;` line recorded in Task 7 Step 3. **This is the acceptance criterion of the whole sub-project.**

If `riscv64-zephyr-elf-gdb` is not on `PATH`, set `RETE_GDB` to the Zephyr SDK's copy rather than falling back to the host `gdb` — a host gdb will produce output that looks plausible and is wrong.

- [ ] **Step 7: Prove the refusal fires**

```bash
./logread.py symbolize --assume-build definitely-not-a-real-key ; echo "exit=$?"
```
Expected: a refusal naming the key, `exit=5`. A refusal that does not fire is the failure D4 exists to prevent.

- [ ] **Step 8: Commit**

```bash
git add nodus/tuvm/zephyr/tools/console/symbolize.py nodus/tuvm/zephyr/tools/console/test_symbolize.py nodus/tuvm/zephyr/tools/console/logread.py
git commit -m "feat(#56): symbolize against the archived ELF, or refuse

Design D4. No fallback to build-*/zephyr/zephyr.elf, ever: a confident
wrong source location is worse than the CORRUPTED backtrace #56 complained
about, because it looks like an answer.

D4a is the one exception — --assume-build, typed by a human, prefixed onto
every line of output so the assumption travels with it. The
last-flashed pointer is never read by this tool; a test asserts that,
because a reader that consulted it would make the refusal a fallback in
disguise.

Core reconstruction is Zephyr's own scripts/coredump/, which supports
RISC-V on both sides."
```

---

### Task 11: The acceptance gate, and falsifying it

§9's last paragraph: the gate must go red when the collector is stopped. A check that passes with the daemon down is measuring nothing.

**Files:**
- Create: `nodus/tuvm/zephyr/tools/console/check.py`
- Test: `nodus/tuvm/zephyr/tools/console/test_check.py`

**Interfaces:**
- Consumes: everything above.
- Produces: `check.py` exiting 0 (healthy) or non-zero (with a reason on stderr).

- [ ] **Step 1: Write the failing tests**

Create `nodus/tuvm/zephyr/tools/console/test_check.py`:

```python
import datetime as dt
import pytest
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
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest test_check.py -v`
Expected: `ModuleNotFoundError: No module named 'check'`.

- [ ] **Step 3: Implement**

Create `nodus/tuvm/zephyr/tools/console/check.py`:

```python
#!/usr/bin/env python3
"""Is the collector actually collecting? (#56 §8, D7)

Asserts a RECENT HEARTBEAT IS PRESENT. Never asserts the absence of an error
message: a log that stops looks exactly like a board that went quiet, and only
a positive signal tells them apart.

180 s = three missed 60 s beats, so one slow write is not a false alarm.
"""
import argparse
import datetime as dt
import sys

from logstore import LogStore, MARKER, DEFAULT_ROOT

MAX_AGE_S = 180.0


def check_health(root, board, now=None, max_age_s=MAX_AGE_S):
    now = now or dt.datetime.now(dt.timezone.utc).astimezone()
    store = LogStore(root, board)
    days = store.days()
    if not days:
        return ["no log files at all under %s — the collector has never run" % root]

    import logread
    latest = None
    with open(store.path_for(days[-1])) as fh:
        for line in fh:
            try:
                when, text = logread.parse_line(line.rstrip("\n"))
            except ValueError:
                continue
            if text.startswith("%s heartbeat" % MARKER):
                latest = when
    if latest is None:
        return ["no heartbeat in %s — the collector is not writing" % store.path_for(days[-1])]

    age = (now - latest).total_seconds()
    if age > max_age_s:
        return ["last heartbeat is %.0fs old (limit %.0fs) — the collector is "
                "down or wedged" % (age, max_age_s)]
    return []


def main(argv=None):
    ap = argparse.ArgumentParser(description="Collector health check (#56)")
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--board", default="c6")
    args = ap.parse_args(argv)
    problems = check_health(args.root, args.board)
    for p in problems:
        print("FAIL: %s" % p, file=sys.stderr)
    if not problems:
        print("ok: collector alive")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests**

Run: `cd nodus/tuvm/zephyr/tools/console && python3 -m pytest -v`
Expected: all pass.

- [ ] **Step 5: Falsify the gate on the real system**

This is the step the design singles out. Run it exactly.

```bash
cd nodus/tuvm/zephyr/tools/console
./check.py ; echo "with collector running: exit=$?"

systemctl --user stop rete-console@c6
sleep 190
./check.py ; echo "with collector stopped: exit=$?"

systemctl --user start rete-console@c6
sleep 65
./check.py ; echo "restarted: exit=$?"
```
Expected: `exit=0`, then `exit=1`, then `exit=0`. **If the middle one is 0, the gate is measuring nothing** — fix it before committing. That is the whole point of this step.

- [ ] **Step 6: Confirm the non-C6 boards are still untouched**

```bash
cd nodus/tuvm/zephyr
grep -n 'capture_coredump.py' run.sh
git diff master -- run.sh | grep -c 'esp32s2' || true
```
Expected: `capture_coredump.py` still invoked for the non-C6 branch, and no behavioural change to the `esp32`/`esp32s2` arms beyond the branch that routes around them.

- [ ] **Step 7: Full suite, then commit**

```bash
cd nodus/tuvm/zephyr/tools/console && python3 -m pytest -v
```
Expected: every test in all six files passes. Paste the summary line into the commit.

```bash
git add nodus/tuvm/zephyr/tools/console/check.py nodus/tuvm/zephyr/tools/console/test_check.py
git commit -m "feat(#56): collector health gate, and proof it can fail

Asserts a heartbeat newer than 180s -- three missed beats. A positive
assertion, never the absence of an error: a stopped log and a quiet board
are indistinguishable without one.

Falsified on the real system before committing: stopped the unit, waited
out the threshold, confirmed the check goes red, restarted, confirmed
green. A check that passes with the daemon stopped is measuring nothing
(design §9)."
```

---

## Closing out

After Task 11, before opening a PR:

- [ ] `docs/design/2026-08-03-device-console-collector-design.md` §11 claims it **closes #87**. Delete `nodus/tuvm/zephyr/capture_coredump.py` only if nothing references it — but `run.sh` still calls it for `esp32`/`esp32s2`, so **#87 does not close in this sub-project**. Post that correction on #87 rather than closing it: its hardcoded port is now only reachable from the two boards that have no collector, which is a smaller claim than the design makes. Do not quietly leave the design doc's §11 wrong.
- [ ] Comment on #74 that the CMake bake and boot banner have landed and it stays open for the nsl-side accessor.
- [ ] Comment on #56 that sub-project 1 is complete and (3) and (4) are unblocked.
