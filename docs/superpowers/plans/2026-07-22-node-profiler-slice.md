# Node Profiler — porta-lite slice — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node pushes its #72 span-capture result as raw CBOR over UDP to gw85224-01, where a porta-lite program (`mirror.py`) stores it in porta's schema and `profweb.py` renders the span table — a browser never opens a socket to the node.

**Architecture:** Node side is one `NetBridge` wiring (out→`diag/spans/result`, in→`diag/spans/ctl`) plus an inbound pump in the event loop; the #72 capture facility is unchanged. Gw side is two programs over one SQLite store in porta's `space_cells`/`space_events` schema: `mirror.py` (binds the collector UDP port, transcodes CBOR, sends `ctl`, mirrors any path) and `profweb.py` (HTTP `:8086`, renders from the store, never touches a node).

**Tech Stack:** nsl (node), Python 3.11 stdlib + `cbor2` in a venv (gw), SQLite (stdlib), CBOR (RFC 8949, tags 27/39 — `cbor2` reads the node's dialect with zero custom code per `review/lib/cbor.md:24`).

## Global Constraints

- **Design of record:** `~/workspace/tuvm/docs/design/2026-07-22-node-profiler-slice-design.md`. Every task traces to a section there.
- **Node emits raw data only** — no formatting, no HTML, no JSON on the node. The gw does all rendering (design §1, §3).
- **`profweb.py` NEVER opens a socket to a node.** It reads only the store. This is what makes tuvm#73 structurally impossible on this path (design §3).
- **Never poll a node inside a capture window** — every request perturbs the spans (design §6).
- **nsl suite: Path B only.** Run single tests with `python3 ~/workspace/nsl-tuvm/tools/run-one.py <file.nsl> B` — Path A per-test gcc rebuild is too slow (standing rule). A SKIP proves nothing (returns before compiling).
- **nsl source extension is `.nsl`.** Node files are single-class.
- **Node entry gets `serial XOR net`, one second capability** (`src/oimg_loader.c:212-217`). The diag node declares `net:` exactly as `SoakNode` does; HTTP is a separate platform seam and coexists with UDP.
- **Deploy is a Model B `.oimg` push, not a flash** (design §5.1; tuvm#67). Build the image with `zephyr/run.sh`; do not `-g` a solidified image.
- **Out of scope:** build identity (tuvm#74 — `/hello` uses a hand-maintained version constant), run-diff/history UI, thermal/stats-as-CBOR, auth/DTLS/multi-node (design §7).

---

## File map

| path | repo | create/modify | responsibility |
|---|---|---|---|
| `nsl-tests/19_latency/08_capture_cbor_roundtrip.nsl` | nsl-tests | create | Task 1 — the result Record survives a CBOR round-trip via the same put→whenever path the bridge uses |
| `zephyr/ThermalHttpNode.nsl` | tuvm | modify | Task 2 — declare `net:`, bind diag socket, wire `NetBridge`, pump inbound |
| `zephyr/tools/gwprofiler/store.py` | tuvm | create | Task 3 — SQLite store, porta `space_cells`/`space_events` schema |
| `zephyr/tools/gwprofiler/codec.py` | tuvm | create | Task 3 — CBOR⇄algebra helpers over `cbor2` (Record=tag27, Symbol=tag39) |
| `zephyr/tools/gwprofiler/mirror.py` | tuvm | create | Task 4 — collector socket, transcode, write store, send ctl |
| `zephyr/tools/gwprofiler/profweb.py` | tuvm | create | Task 5 — HTTP :8086, render span table from the store |
| `zephyr/tools/gwprofiler/test_*.py` | tuvm | create | pytest for store/codec/mirror |
| `zephyr/tools/gwprofiler/README.md` | tuvm | create | deploy notes (venv, systemd, mirrors soak_web precedent) |

---

## Task 1: Retire the §4a risk — result Record survives a CBOR round-trip

**Why first:** design §4a flags "does `out whenever: ''` fire on a cell `put:`" as the one unproven assumption. It is in fact *already proven* by `nsl-tests/16_cells/04_whenever_on_put.nsl` ("space_put writes the slot and then posts the value onto the same ring a stream uses"). The genuinely unproven fact is the gw's real dependency: that the `#SpanCapture` result Record written by `put:` (`kernel.nsl:1266`) both (a) fires a `whenever: ''` reaction on `diag/spans/result` and (b) CBOR-encodes and decodes back to an equal Record. This test asserts the whole path with no socket and no hardware.

**Files:**
- Create: `~/workspace/nsl-tests/19_latency/08_capture_cbor_roundtrip.nsl`

**Interfaces:**
- Consumes: `platform SpanCapture onSpace:spans:`, `platform Spans new`, `Spans>>register:`, `Spans>>recordSlot:us:`, `platform Cbor` (`Cbor new encode:` → Bytes, `Cbor new decode:ifInvalid:` → message), channels `diag/spans/ctl` / `diag/spans/result`. All exist today (`kernel.nsl`, `review/lib/cbor.md`).
- Produces: nothing consumed by later tasks; it is a gate.

- [ ] **Step 1: Write the failing test**

Create `~/workspace/nsl-tests/19_latency/08_capture_cbor_roundtrip.nsl`. Model the wiring on `19_latency/07_span_capture.nsl` and the cbor round-trip on `14_cbor`. The reaction on `diag/spans/result` mimics `NetBridge>>start`'s `out whenever: '' do: [ :v | ... encode ... ]` (`kernel.nsl:1430-1431`) but encodes into a captured temp instead of a socket:

```
(*
expect:
fired true
encoded non-empty true
roundtrip n 2
roundtrip min 100
roundtrip max 200
done
*)
(* tuvm#72/#73 profiler slice, design §4a: the capture result written by `put:`
   must (a) fire a `whenever: ''` reaction on diag/spans/result — the same path
   NetBridge>>start uses — and (b) CBOR round-trip back to an equal Record, which
   is what the gw depends on. No socket: the reaction encodes into a temp. *)
class CaptureCborRoundtrip packageUsing: manifest = ( | seen ::= nil. enc ::= nil. | ) (
  public main: platform args: args = (
    | space spans a cap ctl res dec slots row |
    space:: platform space.
    spans:: platform Spans new.
    a::     spans register: #acquire.
    cap::   platform SpanCapture onSpace: space spans: spans.
    cap start.
    ctl:: space channel: 'diag/spans/ctl'.
    res:: space channel: 'diag/spans/result'.

    (* the bridge's outbound subscription, minus the socket *)
    res whenever: '' do: [ :v | seen:: v. enc:: platform Cbor new encode: v ].

    ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #running).
    space drain.
    spans recordSlot: a us: 100.
    spans recordSlot: a us: 200.
    ctl put: '' value: (platform Record tag: #SpanCtl with: #state -> #stopped).
    space drain.

    ('fired ' , (seen isNil not) printString) out.
    ('encoded non-empty ' , (enc isNil not and: [ enc size > 0 ]) printString) out.

    dec:: platform Cbor new decode: enc ifInvalid: [ :m | nil ].
    ('roundtrip n ' , (dec at: #n ifAbsent: [ -1 ]) printString) out.
    slots:: dec at: #slots ifAbsent: [ nil ].
    row:: slots at: a + 1.
    ('roundtrip min ' , (row at: #minUs ifAbsent: [ -1 ]) printString) out.
    ('roundtrip max ' , (row at: #maxUs ifAbsent: [ -1 ]) printString) out.
    'done' out.
  )
) : (
)
```

- [ ] **Step 2: Run it and confirm it passes on Path B**

Run: `python3 ~/workspace/nsl-tuvm/tools/run-one.py ~/workspace/nsl-tests/19_latency/08_capture_cbor_roundtrip.nsl B`
Expected: PASS (the `expect:` block matches actual output). If instead `fired false`, the §4a risk was real after all — STOP and escalate: make `diag/spans/result` a `post` at `kernel.nsl:1266` or add a peek-on-write to `NetBridge`, then re-run. If `roundtrip n -1`, the Record did not survive CBOR — inspect `Cbor` for `#SpanCapture`/tag-27 handling before proceeding.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/nsl-tests && git add 19_latency/08_capture_cbor_roundtrip.nsl
git commit -m "test(#72,#73): capture result survives CBOR round-trip via the put->whenever path

The profiler slice's node side is a NetBridge whose outbound subscription fires
on the capture result's put: and CBOR-encodes it. Assert both facts on host,
with no socket, so the device task builds on a proven seam. Design §4a.

Refs #72, #73"
```

---

## Task 2: Node — declare `net:`, wire the diag `NetBridge`, pump inbound

**Why:** design §4. The node gains a UDP diag channel while keeping its HTTP server. It is the ONLY node-side change and it adds no formatting.

**Files:**
- Modify: `~/workspace/tuvm/zephyr/ThermalHttpNode.nsl` — entry signature (`~:196`), composition root (`~:196-247`), event loop (`~:247`)

**Interfaces:**
- Consumes: `net udpBindPort:` (`kernel.nsl:1377`), `platform NetBridge socket:out:peer:port:in:` (`kernel.nsl:1446`), `NetBridge>>start` / `NetBridge>>pumpInbound` (`kernel.nsl:1429,1435`). The `#SpanCtl` record arriving on `diag/spans/ctl` drives the unchanged `SpanCapture` (already wired at `ThermalHttpNode.nsl` `capture:: platform SpanCapture ...`).
- Produces: on the wire — a `#SpanCapture` CBOR record sent to `gwAddr:diagResultPort` whenever a window closes; accepts `#SpanCtl` datagrams on the bound diag port.

- [ ] **Step 1: Change the entry to declare the `net:` capability**

The loader mints exactly one second capability (`serial XOR net`, `oimg_loader.c:212-217`). Change the entry from `main: platform args:` to the `net:`-declaring form, mirroring `SoakNode usingPlatform: platform net: net`. In `zephyr/ThermalHttpNode.nsl`, change the class/entry header and add the peer constants (LAN address of gw85224-01, confirmed reachable at `192.168.0.176`):

```
  gwAddr        = ( ^ '192.168.0.176' )   (* gw85224-01 LAN — design §5 *)
  diagBindPort  = ( ^ 54370 )             (* node receives ctl here *)
  diagResultPort = ( ^ 54371 )            (* gw receives result here *)
```

**RESOLVED convention (verified against the tree 2026-07-22, not the plan's original guess):**
ThermalHttpNode currently declares `class ThermalHttpNode packageUsing: manifest = () (...) : ( )`
— `packageUsing: manifest` is its INSTANCE constructor and the class-side is empty. The
net-capable nodes (`NetNode`, `NetBridgeNode`, `ThreadPingNode`) use the opposite split, and
that is the pattern to convert to:

```
class ThermalHttpNode usingPlatform: platform net: net = () (
   ... all existing instance methods; `net` is now an instance var reachable from main: ...
   public main: platform2 args: args = ( ... )   (* platform2 shadows; the class already holds platform + net *)
) : (
   public packageUsing: manifest = ( ^ self new )   (* class-side entry — moved here from the instance ctor slot *)
)
```

So the edit is THREE coordinated changes, not one: (a) the class header line 11
`packageUsing: manifest = ()` → `usingPlatform: platform net: net = ()`; (b) the empty
class-side `) : ( )` at EOF → `) : ( public packageUsing: manifest = ( ^ self new ) )`; (c)
inside `main:`, note `net` is now the class instance var — the loader mints it as the second
capability (`serial XOR net`, `oimg_loader.c:212-217`), and HTTP is unaffected because it rides
the `platform` seam, not the net cap. `args` is unused in the body (confirmed) so `main: platform2
args: args` keeps its shape. THIS EDIT IS UNVERIFIED UNTIL THE BUILD IN STEP 4 — it changes the
node's launch convention, and a wrong conversion yields a node that will not construct.

- [ ] **Step 2: Bind the diag socket and wire the bridge in the composition root**

After `capture start.` in `main:`, add (mirrors `SoakNode.nsl:37-41` + `kernel.nsl:1430`):

```
    | diagSock diagBridge |
    diagSock:: net udpBindPort: diagBindPort.
    diagSock isNil ifTrue: [ 'DIAG BIND FAILED' println ]
    ifFalse: [
      diagBridge:: platform NetBridge
        socket: diagSock
        out: (space channel: 'diag/spans/result')
        peer: gwAddr port: diagResultPort
        in: (space channel: 'diag/spans/ctl').
      diagBridge start ].
```

(`diagBridge` must be a temp held for the life of `main:` — like `capture` — or a collected bridge silently stops reacting; design §4 note. Declare it in `main:`'s temp list, not a fresh block temp.)

- [ ] **Step 3: Pump inbound in the event loop**

The bridge's inbound side is pump-based (`pumpInbound` calls `socket receive`, `kernel.nsl:1435`). Change the loop `[ true ] whileTrue: [ platform pumpEvents. space drain ]` to also pump the bridge each tick:

```
    [ true ] whileTrue: [
      platform pumpEvents.
      diagBridge isNil ifFalse: [ diagBridge pumpInbound ].
      space drain ].
```

Without this, `ctl` datagrams are silently never read (design §4 note). This is a pump, not a listener — no accept queue — so tuvm#73 immunity holds.

- [ ] **Step 4: Syntax-check the node from a scratch dir**

Node files are single-class and syntax-checked by compiling. Build the image (this also proves the entry's `net:` cap resolves):

Run: `cd ~/workspace/tuvm/zephyr && ./run.sh` for the esp32 bake step only (or the project's node-build invocation for ThermalHttpNode on Model B). Expected: `[bake] ThermalHttpNode (model B)` then `[blob] <N> bytes -> .incbin` with no compile error. A `net:`-resolution failure surfaces here.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl
git commit -m "feat(#72,#73): ThermalHttpNode pushes span captures over UDP/CBOR

Declare the net: capability (serial XOR net, one second cap) and wire a
NetBridge: out->diag/spans/result, in->diag/spans/ctl, peer gw85224-01. Pump
the bridge inbound each tick alongside pumpEvents. The #72 capture facility is
unchanged — this is its second client over a new transport. HTTP TCP serve and
UDP diag coexist (separate seams). Design §4.

Refs #72, #73"
```

---

## Task 3: Gw — the store and the codec (porta-lite kernel, pure library)

**Why:** design §5. `store.py` is porta's schema; `codec.py` isolates the CBOR dialect. Both are pure and unit-tested before any socket exists.

**Files:**
- Create: `~/workspace/tuvm/zephyr/tools/gwprofiler/store.py`
- Create: `~/workspace/tuvm/zephyr/tools/gwprofiler/codec.py`
- Test: `~/workspace/tuvm/zephyr/tools/gwprofiler/test_store.py`, `test_codec.py`

**Interfaces:**
- Produces (`store.py`): `Store(path)`; `Store.write_event(node_id, path, seq, value, ts, direction)`; `Store.upsert_cell(node_id, path, seq, value, ts)`; `Store.latest_cell(node_id, path) -> dict | None` (returns the decoded value dict, newest by seq).
- Produces (`codec.py`): `decode(raw: bytes) -> object` and `encode(obj) -> bytes` mapping tag-27 Records to `{'_tag': str, **fields}` dicts and tag-39 Symbols to `Sym(str)`; `span_ctl(state: str) -> bytes` (builds the `#SpanCtl` datagram the node accepts).

- [ ] **Step 1: Create the venv and confirm cbor2 (one-time, on gw and locally)**

`cbor2` is absent on gw and is the reference decoder for this dialect (`review/lib/cbor.md:24`). Create a venv beside the tools:

Run (locally for tests, and later on gw): `python3 -m venv ~/workspace/tuvm/zephyr/tools/gwprofiler/.venv && ~/workspace/tuvm/zephyr/tools/gwprofiler/.venv/bin/pip install cbor2 pytest`
Expected: `Successfully installed cbor2-...`. `sqlite3` is stdlib (confirmed on gw).

- [ ] **Step 2: Write the failing codec test**

Create `test_codec.py`. The node's dialect: Record = `CBORTag(27, [CBORTag(39, tagname), fields_map])`, Symbol = `CBORTag(39, text)`, map keys are symbols (`review/lib/cbor.md` mapping table; the ctl record is `#SpanCtl with: #state -> #running`).

```python
import cbor2
from codec import decode, encode, span_ctl, Sym

def test_decode_record_flattens_tag27():
    raw = cbor2.dumps(cbor2.CBORTag(27, [cbor2.CBORTag(39, "SpanCapture"),
                                         {cbor2.CBORTag(39, "n"): 2}]),
                      canonical=True)
    assert decode(raw) == {"_tag": "SpanCapture", "n": 2}

def test_span_ctl_roundtrips_to_a_running_record():
    obj = cbor2.loads(span_ctl("running"))
    assert obj.tag == 27
    assert obj.value[0] == cbor2.CBORTag(39, "SpanCtl")
    assert obj.value[1] == {cbor2.CBORTag(39, "state"): cbor2.CBORTag(39, "running")}
```

- [ ] **Step 3: Run it, verify it fails**

Run: `cd ~/workspace/tuvm/zephyr/tools/gwprofiler && .venv/bin/pytest test_codec.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'codec'`.

- [ ] **Step 4: Implement `codec.py`**

```python
"""CBOR <-> algebra for the node's dialect (RFC 8949, tags 27/39).
See ~/workspace/tuvm/review/lib/cbor.md: Record=tag27+[tag39(name),map],
Symbol=tag39+text. cbor2 reads this with zero custom code."""
import cbor2


class Sym(str):
    """A decoded symbol (tag 39). A str subclass so it renders plainly."""


def _lower(x):
    if isinstance(x, cbor2.CBORTag) and x.tag == 39:
        return Sym(x.value)
    if isinstance(x, cbor2.CBORTag) and x.tag == 27:
        name, fields = x.value
        out = {"_tag": str(_lower(name))}
        for k, v in fields.items():
            out[str(_lower(k))] = _lower(v)
        return out
    if isinstance(x, list):
        return [_lower(v) for v in x]
    if isinstance(x, dict):
        return {str(_lower(k)): _lower(v) for k, v in x.items()}
    return x


def decode(raw: bytes):
    return _lower(cbor2.loads(raw))


def encode(obj) -> bytes:
    # only the shapes the profiler sends are supported (ctl records)
    raise NotImplementedError("use span_ctl for the one record the gw emits")


def span_ctl(state: str) -> bytes:
    """The #SpanCtl datagram the node's SpanCapture accepts. Canonical so the
    node's deterministic decoder sees shortest-form, sorted keys (cbor.md rule 1)."""
    rec = cbor2.CBORTag(27, [cbor2.CBORTag(39, "SpanCtl"),
                             {cbor2.CBORTag(39, "state"): cbor2.CBORTag(39, state)}])
    return cbor2.dumps(rec, canonical=True)
```

- [ ] **Step 5: Run codec test, verify pass**

Run: `.venv/bin/pytest test_codec.py -v`
Expected: PASS (2 passed).

- [ ] **Step 6: Write the failing store test**

Create `test_store.py`:

```python
from store import Store

def test_latest_cell_returns_newest_by_seq(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    s.upsert_cell("n1", "diag/spans/result", 1, {"_tag": "SpanCapture", "n": 2}, 100.0)
    s.upsert_cell("n1", "diag/spans/result", 2, {"_tag": "SpanCapture", "n": 5}, 200.0)
    assert s.latest_cell("n1", "diag/spans/result")["n"] == 5

def test_events_are_append_only(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    s.write_event("n1", "diag/spans/result", 1, {"n": 2}, 100.0, "in")
    s.write_event("n1", "diag/spans/result", 2, {"n": 5}, 200.0, "in")
    assert s.event_count("n1") == 2

def test_missing_cell_is_none(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    assert s.latest_cell("n1", "diag/spans/result") is None
```

- [ ] **Step 7: Run it, verify it fails**

Run: `.venv/bin/pytest test_store.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'store'`.

- [ ] **Step 8: Implement `store.py`**

Value stored as JSON text (the decoded dict), keyed exactly like porta's `space_cells`/`space_events` (`protocol/PROTOCOL-NSL.md:349-352`); `node_id` is the source address until tuvm#74 (design §5, §7):

```python
"""porta-lite store: space_cells + space_events, SQLite (stdlib).
Schema mirrors protocol/PROTOCOL-NSL.md:349-352. value stored as JSON text
(the codec's decoded dict) rather than raw CBOR — the gw is the readable end."""
import json
import sqlite3


class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS space_events(
              node_id TEXT, path TEXT, seq INTEGER, value_json TEXT,
              ts REAL, direction TEXT);
            CREATE TABLE IF NOT EXISTS space_cells(
              node_id TEXT, path TEXT, seq INTEGER, value_json TEXT, ts REAL,
              PRIMARY KEY(node_id, path));
        """)
        self.db.commit()

    def write_event(self, node_id, path, seq, value, ts, direction):
        self.db.execute(
            "INSERT INTO space_events VALUES(?,?,?,?,?,?)",
            (node_id, path, seq, json.dumps(value), ts, direction))
        self.db.commit()

    def upsert_cell(self, node_id, path, seq, value, ts):
        self.db.execute(
            "INSERT INTO space_cells VALUES(?,?,?,?,?) "
            "ON CONFLICT(node_id, path) DO UPDATE SET "
            "seq=excluded.seq, value_json=excluded.value_json, ts=excluded.ts "
            "WHERE excluded.seq >= space_cells.seq",
            (node_id, path, seq, json.dumps(value), ts))
        self.db.commit()

    def latest_cell(self, node_id, path):
        row = self.db.execute(
            "SELECT value_json FROM space_cells WHERE node_id=? AND path=?",
            (node_id, path)).fetchone()
        return json.loads(row[0]) if row else None

    def event_count(self, node_id):
        return self.db.execute(
            "SELECT COUNT(*) FROM space_events WHERE node_id=?",
            (node_id,)).fetchone()[0]
```

- [ ] **Step 9: Run store test, verify pass**

Run: `.venv/bin/pytest test_store.py -v`
Expected: PASS (3 passed).

- [ ] **Step 10: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/tools/gwprofiler/store.py zephyr/tools/gwprofiler/codec.py zephyr/tools/gwprofiler/test_store.py zephyr/tools/gwprofiler/test_codec.py
git commit -m "feat(#72,#69): gw porta-lite store + CBOR codec

store.py: porta's space_cells/space_events in SQLite (PROTOCOL-NSL.md:349-352),
seq-monotonic cell upsert, append-only events. codec.py: node CBOR dialect
(tag27 Record, tag39 Symbol) over cbor2, plus span_ctl() for the one record the
gw emits. node_id = source address until #74. Design §5.

Refs #72, #69"
```

---

## Task 4: Gw — `mirror.py`, the collector (knows nothing about spans)

**Why:** design §5. Binds the collector UDP port, decodes inbound datagrams into the store, and sends `ctl` records. It mirrors *any* path a node pushes — spans are not special-cased here.

**Files:**
- Create: `~/workspace/tuvm/zephyr/tools/gwprofiler/mirror.py`
- Test: `~/workspace/tuvm/zephyr/tools/gwprofiler/test_mirror.py`

**Interfaces:**
- Consumes: `store.Store`, `codec.decode`, `codec.span_ctl`.
- Produces: `Mirror(store, bind_port)`; `Mirror.handle_datagram(raw, addr, ts)` (decode → write event + upsert cell, path taken from the record's `_tag`/a known mapping); `Mirror.send_ctl(node_addr, node_port, state)`; `Mirror.run()` (blocking recv loop). `handle_datagram` is factored out so it is testable without a socket.

- [ ] **Step 1: Write the failing mirror test**

```python
from store import Store
from mirror import Mirror
import codec, cbor2

def _result_bytes(n):
    rec = cbor2.CBORTag(27, [cbor2.CBORTag(39, "SpanCapture"),
                             {cbor2.CBORTag(39, "n"): n,
                              cbor2.CBORTag(39, "ms"): 1000,
                              cbor2.CBORTag(39, "slots"): []}])
    return cbor2.dumps(rec, canonical=True)

def test_datagram_lands_as_cell_and_event(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    m.handle_datagram(_result_bytes(7), ("192.0.2.40", 54371), ts=100.0)
    assert s.latest_cell("192.0.2.40", "diag/spans/result")["n"] == 7
    assert s.event_count("192.0.2.40") == 1

def test_bad_cbor_is_dropped_not_raised(tmp_path):
    s = Store(str(tmp_path / "p.db"))
    m = Mirror(s, bind_port=0)
    m.handle_datagram(b"\xff\xff\xff", ("192.0.2.40", 54371), ts=100.0)  # garbage
    assert s.event_count("192.0.2.40") == 0
```

- [ ] **Step 2: Run it, verify it fails**

Run: `.venv/bin/pytest test_mirror.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'mirror'`.

- [ ] **Step 3: Implement `mirror.py`**

```python
"""porta-lite collector. Binds the diag UDP port, decodes each datagram into the
store, sends ctl records. Knows only paths, not what a 'span' is — the next thing
a node pushes is a new profweb view, not new code here. Design §5."""
import socket
import time

import codec

# The one path this slice mirrors; a record's _tag maps to its cell path.
_TAG_TO_PATH = {"SpanCapture": "diag/spans/result"}


class Mirror:
    def __init__(self, store, bind_port=54371):
        self.store = store
        self.bind_port = bind_port
        self._seq = {}   # node_id -> monotonic seq assigned on write

    def _next_seq(self, node_id):
        n = self._seq.get(node_id, 0) + 1
        self._seq[node_id] = n
        return n

    def handle_datagram(self, raw, addr, ts):
        node_id = addr[0]
        try:
            value = codec.decode(raw)
        except Exception:
            return  # closed algebra: a bad datagram is dropped, never stored
        if not isinstance(value, dict) or "_tag" not in value:
            return
        path = _TAG_TO_PATH.get(value["_tag"])
        if path is None:
            return
        seq = self._next_seq(node_id)
        self.store.write_event(node_id, path, seq, value, ts, "in")
        self.store.upsert_cell(node_id, path, seq, value, ts)

    def send_ctl(self, node_addr, node_port, state):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.sendto(codec.span_ctl(state), (node_addr, node_port))
        finally:
            s.close()

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", self.bind_port))
        print(f"[mirror] collecting on :{self.bind_port}")
        while True:
            raw, addr = s.recvfrom(2048)
            self.handle_datagram(raw, addr, time.time())
```

- [ ] **Step 4: Run mirror test, verify pass**

Run: `.venv/bin/pytest test_mirror.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/tools/gwprofiler/mirror.py zephyr/tools/gwprofiler/test_mirror.py
git commit -m "feat(#72,#73): gw collector — mirror any pushed path into the store

mirror.py binds the diag UDP port, decodes each datagram (dropping bad CBOR,
closed algebra), writes it as event + cell, and sends ctl records. Path comes
from the record _tag; spans are not special-cased — the next pushed path is a
new profweb view, not new collector code. handle_datagram is socket-free so it
unit-tests. Design §5.

Refs #72, #73"
```

---

## Task 5: Gw — `profweb.py`, render the span table (never touches a node)

**Why:** design §3, §5. One HTTP view over the store. It calls the store only — no node socket — which is the structural guarantee against tuvm#73.

**Files:**
- Create: `~/workspace/tuvm/zephyr/tools/gwprofiler/profweb.py`
- Test: `~/workspace/tuvm/zephyr/tools/gwprofiler/test_profweb.py`

**Interfaces:**
- Consumes: `store.Store.latest_cell`, `mirror.Mirror.send_ctl`.
- Produces: `render_span_table(cell: dict | None) -> str` (pure, testable); an HTTP handler on `:8086` with `GET /` (render the latest `diag/spans/result` for a node) and `POST /run?node=<addr>` (send `ctl:running`, wait, send `ctl:stopped` — the node pushes the result asynchronously; design §5.1). Render reuses the #72 columns: span, lang, n, fails, min µs, max µs.

- [ ] **Step 1: Write the failing render test**

```python
from profweb import render_span_table

def test_render_lists_each_slot_row():
    cell = {"_tag": "SpanCapture", "n": 2, "ms": 1000, "slots": [
        {"_tag": "LatencySpan", "span": "acquire", "n": 2, "fails": 0,
         "minUs": 100, "maxUs": 200}]}
    html = render_span_table(cell)
    assert "acquire" in html and "<td" in html and "100" in html and "200" in html

def test_render_handles_no_capture():
    html = render_span_table(None)
    assert "No capture" in html
```

- [ ] **Step 2: Run it, verify it fails**

Run: `.venv/bin/pytest test_profweb.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'profweb'`.

- [ ] **Step 3: Implement `profweb.py`**

```python
"""porta-lite web view (:8086). Renders the latest diag/spans/result for a node
from the store. NEVER opens a socket to a node — that is the structural defence
against tuvm#73 (design §3). Drives a run via mirror.send_ctl; the node pushes
the result asynchronously (design §5.1)."""
import argparse
import html
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

from store import Store
from mirror import Mirror

PORT = 8086
_COLS = [("span", "span"), ("lang", "lang"), ("n", "n"),
         ("fails", "fails"), ("minUs", "min µs"), ("maxUs", "max µs")]


def render_span_table(cell):
    if not cell or not cell.get("slots"):
        return "<p>No capture taken.</p>"
    head = "".join(f"<th>{h}</th>" for _, h in _COLS)
    rows = []
    for slot in cell["slots"]:
        tds = "".join(f"<td>{html.escape(str(slot.get(k, '')))}</td>"
                      for k, _ in _COLS)
        rows.append(f"<tr>{tds}</tr>")
    return (f"<p><b>{cell.get('n', 0)}</b> samples over "
            f"<b>{cell.get('ms', -1)}</b> ms.</p>"
            f"<table><thead><tr>{head}</tr></thead>"
            f"<tbody>{''.join(rows)}</tbody></table>")


def make_handler(store, mirror, node_default):
    class H(BaseHTTPRequestHandler):
        def _send(self, body):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(body.encode())

        def do_GET(self):
            q = parse_qs(urlparse(self.path).query)
            node = q.get("node", [node_default])[0]
            cell = store.latest_cell(node, "diag/spans/result")
            self._send(f"<html><body><h1>node {html.escape(node)}</h1>"
                       f'<form method="post" action="/run?node={html.escape(node)}">'
                       f'<button>Run capture</button></form>'
                       f"{render_span_table(cell)}</body></html>")

        def do_POST(self):
            q = parse_qs(urlparse(self.path).query)
            node = q.get("node", [node_default])[0]
            mirror.send_ctl(node, 54370, "running")
            time.sleep(3.0)              # bracket the node's own thermal ticks
            mirror.send_ctl(node, 54370, "stopped")
            self.send_response(303)
            self.send_header("Location", f"/?node={node}")
            self.end_headers()

        def log_message(self, *a):
            pass
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="porta-lite.db")
    ap.add_argument("--node", default="192.0.2.40")
    args = ap.parse_args()
    store = Store(args.db)
    mirror = Mirror(store)
    srv = ThreadingHTTPServer(("0.0.0.0", PORT),
                              make_handler(store, mirror, args.node))
    print(f"[profweb] http://gw85224-01:{PORT}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run render test, verify pass**

Run: `.venv/bin/pytest test_profweb.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Full gw suite green**

Run: `.venv/bin/pytest -v`
Expected: PASS (all store/codec/mirror/profweb tests).

- [ ] **Step 6: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/tools/gwprofiler/profweb.py zephyr/tools/gwprofiler/test_profweb.py
git commit -m "feat(#72,#73): gw profweb — render span table from the store

profweb.py serves :8086, rendering the latest diag/spans/result for a node
using the #72 columns. GET renders from the store only; POST /run drives a
capture via mirror.send_ctl and the node pushes the result async (design §5.1).
It never opens a socket to a node — the structural defence against #73.

Refs #72, #73"
```

---

## Task 6: `/hello` — the one HTTP path a wifi node keeps (independent)

**Why:** design §8. A single no-links liveness page so a browser cannot fan out. Independent of Tasks 1–5; can land before or after. Build id stays tuvm#74; this uses a hand-maintained version constant + uptime.

**Files:**
- Modify: `~/workspace/tuvm/zephyr/ThermalHttpNode.nsl` — the `/hello` route (`~:239`)

**Interfaces:**
- Consumes: `0 millis` (uptime; already used by `SpanCapture>>beginWindow`, `kernel.nsl:1252`), a node-local `nslVersion` constant.

- [ ] **Step 1: Add a version constant and enrich `/hello`**

Add a method-side constant near `gwAddr`:

```
  nslVersion = ( ^ 'tuvm-node 2026-07-22' )   (* hand-maintained; real build id is #74 *)
```

Replace the `/hello` route body (`plat httpChunkId: id data: 'hello'`) with a single-chunk, no-links answer:

```
    http route: '/hello' do: [ :id :plat |
        plat httpChunkId: id data:
          'hello ', self nslVersion, ' up ', (0 millis) printString, 'ms' ].
```

No `<a href>` anywhere — design §8: a browser has nothing to prefetch.

- [ ] **Step 2: Verify on the flashed/pushed node**

After Task 7's push (or independently), from gw:
Run: `ssh gw85224-01 'curl -s http://192.0.2.40:18080/hello'`
Expected: one line like `hello tuvm-node 2026-07-22 up 43120ms`, no HTML links.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/ThermalHttpNode.nsl
git commit -m "feat(#73): /hello answers version + uptime, single path, no links

The one HTTP path a wifi node keeps: liveness with zero infrastructure. No
<a href>, so a browser cannot fan out and pin the connection budget (#73
defended by shape, not a reaper). Hand-maintained version string; real build
id is #74. Design §8.

Refs #73, #74"
```

---

## Task 7: End-to-end on device — push the image, drive one run

**Why:** design §5.1. Proves the whole path on real hardware: gw drives a window, the node pushes a CBOR result, the store holds it, the page renders it. No TDD loop — this is device verification.

**Files:** none (deploy + verify).

**Interfaces:** consumes everything above.

- [ ] **Step 1: Identify the board, then build and push the image**

NEVER trust a ttyUSB number — identify the board by `esptool read_mac` first (three CP210x on the bench). Build the Model B image for ThermalHttpNode and push it (per `zephyr/run.sh`; `.oimg` push, not a flash — tuvm#67):

Run: `cd ~/workspace/tuvm/zephyr && ./run.sh` (the ThermalHttpNode Model B path). Expected: `[blob] <N> bytes -> .incbin`, image pushed, node boots and prints its ready banner.

- [ ] **Step 2: Deploy the gw programs (venv + systemd, soak_web precedent)**

Copy `zephyr/tools/gwprofiler/` to gw85224-01 (`~/gwprofiler/`), create the venv there, and start `profweb.py` (which owns the `Mirror`). Follow the `soak-web.service` pattern (`zephyr/tools/soak-web.service`). Confirm both ports are listening:

Run: `ssh gw85224-01 'ss -lun | grep -E "54371" ; ss -ltn | grep 8086'`
Expected: UDP `:54371` (collector) and TCP `:8086` (profweb) both present.

- [ ] **Step 3: Drive one capture and verify the round trip**

Run: `ssh gw85224-01 'curl -s -X POST "http://localhost:8086/run?node=192.0.2.40" -o /dev/null -w "%{http_code}\n"; sleep 1; curl -s "http://localhost:8086/?node=192.0.2.40"'`
Expected: `303`, then HTML containing a span table with non-empty rows (`acquire`, `render`, `egress`, …) and a sample count > 0. If the table says "No capture", check: (a) the node bound `:54370` (`DIAG BIND FAILED` on its console means the `net:` cap failed), (b) `ss -unp` on gw shows a datagram arrived, (c) `mirror` did not drop it (temporarily log `handle_datagram`).

- [ ] **Step 4: Confirm the store is a witness (seeds tuvm#69)**

Run: `ssh gw85224-01 '~/gwprofiler/.venv/bin/python3 -c "import sqlite3; print(sqlite3.connect(\"~/gwprofiler/porta-lite.db\").execute(\"SELECT node_id,path,seq FROM space_events ORDER BY seq DESC LIMIT 3\").fetchall())"'`
Expected: rows for `192.0.2.40 diag/spans/result` — the append-only run log tuvm#69 asked for (attributable by address until tuvm#74).

- [ ] **Step 5: Record the result (no code commit)**

Post the outcome to tuvm#72 and tuvm#73 (comment, with the rendered table and the `space_events` rows as evidence). Promote the standing rule — figures come from the capture table, never the cumulative one (design §6) — into `~/workspace/tuvm/review/program-sequence.md` at close-out, per the CLAUDE.md promotion rule.

---

## Self-Review

- **Spec coverage:** §1 slice (Tasks 1–7), §3 shape (Tasks 3–5), §4 node/pump/§4a risk (Tasks 1–2), §5 store+mirror+run (Tasks 3–5, 7), §5.1 drive-a-run (Task 5 POST, Task 7), §6 integrity rules (Global Constraints + Task 5 no-node-socket), §7 out-of-scope (Global Constraints; #74 in Task 6), §8 /hello (Task 6), §9 direction-of-travel (correctly NOT planned). Covered.
- **Placeholders:** none — every code step shows complete code; every run step names the command and expected output.
- **Type consistency:** `Store.upsert_cell`/`latest_cell`/`write_event`/`event_count`, `codec.decode`/`span_ctl`/`Sym`, `Mirror.handle_datagram`/`send_ctl`/`run`, `render_span_table` — names match across Tasks 3→4→5→7. Record shape `{"_tag", ...}` consistent from codec through store, mirror, and render. `diag/spans/result` / `diag/spans/ctl`, ports 54370 (node bind / ctl) and 54371 (gw collector / result) consistent across node (Task 2) and gw (Tasks 4–5, 7).

One open verification carried into execution (not a placeholder — a labelled risk with a defined branch): Task 1 Step 2 confirms §4a; if it fails, the escalation path is specified.
