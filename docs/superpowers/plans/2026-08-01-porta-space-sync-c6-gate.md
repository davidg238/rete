# porta space-sync — the C6 hardware gate (plan 3 of 3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Witness porta space-sync design §9's three legs on a real ESP32-C6 over a real
Thread mesh, plus a deliberate datagram drop that makes the node re-push rows the gateway
has already applied — and close the rung.

**Architecture:** Four moving parts. (1) The Go gateway gains a default-off
`--test-drop-ack N` that swallows every Nth outbound `#ack` **after** the push is
committed. (2) `PortaBridgeNodeC6.nsl` is plan 2's node with C6 constants and one console
line per tick. (3) `porta_c6_console.py` holds the single serial handle. (4)
`porta_sync_c6.sh` drives porta over ssh on the border router, scores four legs from
*both* ends, and prints one PASS/FAIL line per leg.

**Tech Stack:** Go 1.x (`~/workspace/porta`), nsl (`tuvm/zephyr/*.nsl`), Zephyr + esptool
via `zephyr/run.sh --board esp32c6`, bash + pyserial for the gate, OpenThread FTD, ssh to
`gw85224-01`.

**Source of truth:** `~/workspace/tuvm/docs/design/2026-08-01-porta-space-sync-c6-gate-design.md`
(approved 2026-08-01; D1–D9 are cited by number below), and
`~/workspace/porta/docs/design/2026-08-01-porta-space-sync-rewrite-design.md` §9 for the
legs themselves.

> **Filename note:** plan 2 is dated `2026-08-02` while this plan is dated `2026-08-01`,
> which is today. Filename order does not reflect execution order for these two.

---

## Global Constraints

- **`--db` is a persistent flag with default `porta.db`, relative to the current
  directory.** Every `porta get` / `put` / `log` / `serve` invocation in this plan passes
  `--db` explicitly. A verb given no `--db` silently opens an empty store in the cwd and
  reports a healthy node as absent — indistinguishable from a dead node. Plan 2's draft
  omitted it and execution had to correct it; do not reintroduce the omission.
- **Ports are addressed by `/dev/serial/by-id/`, never `ttyUSB0` / `ttyACM0`** — those are
  allocation-order names, and this C6 currently claims both. `run.sh` already does this;
  the gate does too.
- **Console and flash are different interfaces on this board.** Console = uart0 → the
  CP210x bridge (`usb-Silicon_Labs_CP2102N_…`), documented at `zephyr/prj_esp32c6.conf:30`.
  Flash = USB-JTAG (`usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:45:47:A4`). The
  console handle therefore opens *before* the flash and is held across it.
- **Exactly one process opens the console.** A second reader silently splits the stream.
- **Decode serial with `errors="replace"`.** One NUL byte has turned a grep monitor binary
  here before, and grep on a "binary file" answers nothing, which reads like a dead node.
- **The gate never requires `misses == 0`.** A 512 B batch is 5–6 6LoWPAN fragments over a
  127 B 802.15.4 PHY, so real loss is expected. The assertions are *no duplicates* and
  *no gaps* (D7).
- **`sudo` on gw85224-01 needs a password.** Exactly one step in this plan uses it (Task 5,
  Step 2), performed by the operator. No gate run may require it.
- **nsl dialect:** `[ ... ] whileTrue:`, never `repeat`; no modulo; state in instance slots
  so every reaction block is a one-line self-send (the LD_SLOT gotcha).
- **Deliberately out of scope**, each with a home: NVS persistence of cursor/bootId
  (#117), the adaptive cadence controller (#115), a capability pair for the bridge
  (#116), stream loss while the bridge is down (#114), and `diag/spans/result`,
  which stays unsynced until tuvm#107 splits its ~1120 B value.

---

## File Structure

| file | fate | responsibility |
|---|---|---|
| `porta/internal/wire/server.go` | modify | `SetDropAckEvery` + the post-commit swallow in `ListenAndServe` |
| `porta/internal/wire/drop_test.go` | **create** | the flag drops the right datagram at the right moment |
| `porta/internal/portacli/serve.go` | modify | the `--test-drop-ack` flag and its wiring |
| `porta/internal/portacli/serve_test.go` | modify | the flag is registered and defaults to off |
| `tuvm/tests/porta_sync_host.sh` | modify | D7's contiguity assertion, and a robust tick parse |
| `tuvm/zephyr/PortaBridgeNodeC6.nsl` | **create** | the C6 node: plan 2's body, C6 constants, per-tick line |
| `tuvm/tests/porta_c6_console.py` | **create** | the single console handle; tees to a log |
| `tuvm/tests/porta_sync_c6.sh` | **create** | the four-leg hardware gate |
| `tuvm/review/tests/CLAIMS.md`, `tuvm/review/program-sequence.md`, `tuvm/docs/next-steps-overview.md`, `tuvm/protocol/PROTOCOL-NSL.md` | modify | close-out promotion |

---

### Task 1: The gateway's post-commit ack drop

**Files:**
- Modify: `~/workspace/porta/internal/wire/server.go`
- Create: `~/workspace/porta/internal/wire/drop_test.go`
- Modify: `~/workspace/porta/internal/portacli/serve.go`
- Modify: `~/workspace/porta/internal/portacli/serve_test.go`

**Interfaces:**
- Consumes: `wire.NewServer(st, logf) *Server`, `Server.HandleFrom([]byte, string) []byte`,
  `Server.ListenAndServe(ctx, addr) error`, `DecodeAck([]byte) (int64, error)`,
  `EncodePush(id string, bootID int64, entries []space.Entry) ([]byte, error)`,
  `space.Store.Events(nodeID string, limit int) ([]Event, error)`, and the existing test
  helpers `testServer(t)` / `mustEnc(t, v)` in `internal/wire/wire_test.go`.
- Produces: `func (s *Server) SetDropAckEvery(n int)` — 0 disables. Called before
  `ListenAndServe` starts; not concurrency-safe and does not need to be.

- [ ] **Step 1: Write the failing tests**

Create `~/workspace/porta/internal/wire/drop_test.go`:

```go
// Copyright (c) 2026 Ekorau LLC

// internal/wire/drop_test.go
package wire

import (
	"context"
	"net"
	"testing"
	"time"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// dropHarness runs a real ListenAndServe on loopback and answers a push helper.
// A real socket rather than a direct Handle call, because the swallow happens in
// the UDP loop and not in Handle — testing Handle would test the wrong layer.
func dropHarness(t *testing.T, dropEvery int) (*Server, *space.Store, func(seq int64, text string) bool) {
	t.Helper()
	srv, st := testServer(t)
	srv.SetDropAckEvery(dropEvery)

	// Bind on port 0 and read the address back, then close: the listen/close/
	// re-listen window is small and the alternative (a guessed port) is worse.
	pc, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := pc.LocalAddr().String()
	pc.Close()

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go srv.ListenAndServe(ctx, addr)

	conn, err := net.Dial("udp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	push := func(seq int64, text string) bool {
		t.Helper()
		req, err := EncodePush("n1", 1, []space.Entry{
			{Seq: seq, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, text)},
		})
		if err != nil {
			t.Fatalf("EncodePush: %v", err)
		}
		if _, err := conn.Write(req); err != nil {
			t.Fatalf("write: %v", err)
		}
		buf := make([]byte, 1500)
		conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, err := conn.Read(buf)
		if err != nil {
			return false // no reply within the budget: swallowed (or lost)
		}
		if _, err := DecodeAck(buf[:n]); err != nil {
			t.Fatalf("reply to seq %d was not an #ack: %v", seq, err)
		}
		return true
	}
	return srv, st, push
}

// waitForListener sends seq 1 until it is answered. UDP has no accept, so a
// datagram that lands before the bind completes is silently lost. Re-sending
// the same seq is safe — an already-ingested seq is deduped, which is the very
// property this file exists to protect.
func waitForListener(t *testing.T, push func(int64, string) bool) {
	t.Helper()
	for i := 0; i < 50; i++ {
		if push(1, "one") {
			return
		}
	}
	t.Fatal("the listener never answered")
}

// The fault injector the C6 hardware gate scores leg 4 with. What matters is not
// that an ack went missing but WHEN: the push must already be committed, so the
// node re-pushes rows porta has applied and only the `seq <= hw` dedupe stands
// between that and a duplicate. A drop placed before the commit would merely
// reproduce a lost push, which any retry handles and which proves nothing.
func TestDropAckEverySwallowsTheNthAckAfterTheCommit(t *testing.T) {
	_, st, push := dropHarness(t, 3)
	waitForListener(t, push) // ack #1

	if !push(2, "two") {
		t.Fatal("the second ack was dropped; only every third should be")
	}
	if push(3, "three") {
		t.Fatal("the third ack should have been swallowed")
	}

	evs, err := st.Events("n1", 100)
	if err != nil {
		t.Fatalf("Events: %v", err)
	}
	if len(evs) != 3 {
		t.Fatalf("got %d events, want 3 — a swallowed ack must not skip the commit", len(evs))
	}
}

// The push half only. A node that never hears a #batch stalls its cursor, which
// is a different failure from the one being injected — so with every ack
// swallowed, pulls must still answer.
func TestDropAckEveryAnswersPullsWhileSwallowingAcks(t *testing.T) {
	srv, st, push := dropHarness(t, 1)
	if _, err := st.Write("n1", "goal/name", cval.KindCell, mustEnc(t, "alpha"), "david", ""); err != nil {
		t.Fatalf("Write: %v", err)
	}
	req, err := EncodePull("n1", 0, 1, 0)
	if err != nil {
		t.Fatalf("EncodePull: %v", err)
	}
	// Handle directly for the batch half: the swallow is ack-only, so this
	// asserts the discriminator rather than the socket.
	if _, _, err := DecodeBatch(srv.HandleFrom(req, "")); err != nil {
		t.Fatalf("a #batch must never be swallowed: %v", err)
	}
	if push(1, "one") {
		t.Fatal("with dropEvery=1 no ack should ever be delivered")
	}
}
```

- [ ] **Step 2: Run the tests and watch them fail**

```bash
cd ~/workspace/porta && go test ./internal/wire/ -run 'DropAck' -v
```
Expected: compile failure — `srv.SetDropAckEvery undefined (type *Server has no field or
method SetDropAckEvery)`.

- [ ] **Step 3: Implement the swallow**

In `~/workspace/porta/internal/wire/server.go`, extend the struct:

```go
type Server struct {
	st   *space.Store
	logf func(string, ...any)

	// TEST-ONLY fault injection — see SetDropAckEvery.
	dropAckEvery int
	acks         int
}
```

Add, after `NewServer`:

```go
// SetDropAckEvery makes the UDP loop swallow every Nth outbound #ack. TEST ONLY;
// 0 (the zero value) disables it, so a production gateway is unaffected by the
// field's existence.
//
// The swallow happens in ListenAndServe, AFTER handle() has ingested the push
// and advanced the high-water. That position is load-bearing and is the whole
// reason the injector lives in the gateway rather than in a firewall rule: the
// node must re-push rows porta has ALREADY applied, so the `seq <= hw` dedupe is
// what stops the duplicate. Dropping the datagram BEFORE the commit would only
// reproduce a lost push — the node re-pushes, porta applies once, and the
// exactly-once assertion passes without ever exercising the dedupe.
//
// Not concurrency-safe, and does not need to be: ListenAndServe is one
// goroutine and the setter is called before it starts.
func (s *Server) SetDropAckEvery(n int) { s.dropAckEvery = n }

// swallowAck counts outbound acks and answers whether this one is the Nth. A
// #batch or an #err is never swallowed: starving the pull half stalls the
// node's cursor, which is a different failure from the one being injected.
func (s *Server) swallowAck(resp []byte) bool {
	if s.dropAckEvery <= 0 {
		return false
	}
	if _, err := DecodeAck(resp); err != nil {
		return false
	}
	s.acks++
	return s.acks%s.dropAckEvery == 0
}
```

And in `ListenAndServe`, replace the reply block:

```go
		resp := s.HandleFrom(req, peer.String())
		if s.swallowAck(resp) {
			s.logf("porta: TEST-DROP: ack %d to %s swallowed after commit", s.acks, peer)
			continue
		}
		if _, err := pc.WriteTo(resp, peer); err != nil {
			s.logf("porta: reply to %s failed: %v", peer, err)
		}
```

- [ ] **Step 4: Run the tests and watch them pass**

```bash
cd ~/workspace/porta && go test ./internal/wire/ -run 'DropAck' -v
```
Expected: both remaining tests PASS.

- [ ] **Step 5: Write the failing flag test**

Append to `~/workspace/porta/internal/portacli/serve_test.go`:

```go
// The injector must be off unless asked for: a gateway that silently drops one
// ack in three is a gateway that manufactures the very bug the flag exists to
// study.
func TestServeRegistersTestDropAckFlagDefaultingToOff(t *testing.T) {
	f := newServeCmd().Flags().Lookup("test-drop-ack")
	if f == nil {
		t.Fatal("--test-drop-ack is not registered")
	}
	if f.DefValue != "0" {
		t.Fatalf("default = %q, want \"0\" (off)", f.DefValue)
	}
}
```

- [ ] **Step 6: Run it and watch it fail**

```bash
cd ~/workspace/porta && go test ./internal/portacli/ -run 'TestDropAck|TestServeRegistersTestDropAck' -v
```
Expected: FAIL — `--test-drop-ack is not registered`.

- [ ] **Step 7: Wire the flag**

In `~/workspace/porta/internal/portacli/serve.go`, add to the var block in `newServeCmd`:

```go
	var udpAddr, httpBind string
	var httpPort, testDropAck int
	var httpAllowCIDR []string
```

Replace the UDP goroutine line with:

```go
			srv := wire.NewServer(st, log.Printf)
			if testDropAck > 0 {
				srv.SetDropAckEvery(testDropAck)
				log.Printf("porta: TEST-DROP ACTIVE — every %d-th #ack will be swallowed AFTER commit", testDropAck)
			}
			udpErr := make(chan error, 1)
			go func() { udpErr <- srv.ListenAndServe(ctx, udpAddr) }()
```

(the `udpErr := make(...)` line already exists immediately above the goroutine — keep one
copy, ordered as shown), and register the flag beside the others:

```go
	cmd.Flags().IntVar(&testDropAck, "test-drop-ack", 0,
		"TEST ONLY: swallow every Nth outbound #ack AFTER the push is committed (0 = off)")
```

- [ ] **Step 8: Run the whole Go suite**

```bash
cd ~/workspace/porta && go test ./... 2>&1 | tail -20
```
Expected: all packages `ok`. If `internal/portacli` fails on `TestNewServeCmdRegistersFlags`,
read that test — it may enumerate the expected flag set and need the new name added.

- [ ] **Step 9: Commit**

```bash
cd ~/workspace/porta && git add internal/wire/server.go internal/wire/drop_test.go \
  internal/portacli/serve.go internal/portacli/serve_test.go && \
  git commit -m "feat(wire): --test-drop-ack, the post-commit ack injector for the C6 gate (#120)"
```

---

### Task 2: Strengthen the host gate's exactly-once assertion

**Files:**
- Modify: `~/workspace/tuvm/tests/porta_sync_host.sh`

**Interfaces:**
- Consumes: the existing script's `$DB`, `$NODE`, and `/tmp/porta`.
- Produces: nothing another task imports — but the *definition* of exactly-once it
  establishes (no duplicates **and** no gaps) is the one Task 6 reuses verbatim.

Exactly-once is two claims (D7). The host gate checks only "no duplicates", because
loopback loses nothing. Fixing it here first means Task 6 inherits a definition that has
already been run.

- [ ] **Step 1: Replace the leg-3 block**

In `~/workspace/tuvm/tests/porta_sync_host.sh`, replace the block that computes `LOGS`
and `UNIQ` (currently lines ~98–106) with:

```bash
# Leg 3: exactly once, which is TWO claims and not one.
#   no duplicates -> rows == distinct ticks
#   no gaps       -> the distinct ticks run contiguously
# Loopback loses nothing, so only the first can fail here; the second exists so
# that this gate and the C6 gate (plan 3) score the same property. Parse with
# `grep -o` rather than `sed 's/.*tick //'` — the record's formatting puts
# punctuation after the number, which sort -u tolerates and arithmetic does not.
sleep 3
TICKS=$(/tmp/porta log --db "$DB" "$NODE" --limit 200 | grep -o 'tick [0-9]\+' | awk '{print $2}')
LOGS=$(printf '%s\n' "$TICKS" | grep -c . || true)
UNIQ=$(printf '%s\n' "$TICKS" | sort -n -u | grep -c . || true)
echo "  tel/log rows=$LOGS distinct ticks=$UNIQ"
test "$LOGS" -ge 2 || { echo "FAIL: fewer than two tel/log entries — the stream never rode"; exit 1; }
test "$LOGS" = "$UNIQ" || {
  echo "FAIL: a tel/log entry was delivered more than once"
  /tmp/porta log --db "$DB" "$NODE" --limit 200; exit 1; }

FIRST=$(printf '%s\n' "$TICKS" | sort -n -u | head -1)
LAST=$(printf '%s\n' "$TICKS" | sort -n -u | tail -1)
test $((LAST - FIRST + 1)) -eq "$UNIQ" || {
  echo "FAIL: a gap in the tick series ($FIRST..$LAST holds only $UNIQ entries) — an entry was lost"
  /tmp/porta log --db "$DB" "$NODE" --limit 200; exit 1; }
echo "  tel/log arrived exactly once per entry, with no gaps ($FIRST..$LAST)"
```

- [ ] **Step 2: Run the host gate**

```bash
cd ~/workspace/tuvm && tests/porta_sync_host.sh
```
Expected: `PORTA SYNC OK`, with the new line naming the tick range. If it fails on the
contiguity check *on loopback*, that is a real finding, not a test bug — stop and work it
with `superpowers:systematic-debugging` before touching hardware, because it would mean
plan 2's node loses entries in the absence of any loss at all.

- [ ] **Step 3: Commit**

```bash
cd ~/workspace/tuvm && git add tests/porta_sync_host.sh && \
  git commit -m "test(spacesync): exactly-once is no duplicates AND no gaps (#120)"
```

---

### Task 3: `PortaBridgeNodeC6.nsl` — the node

**Files:**
- Create: `~/workspace/tuvm/zephyr/PortaBridgeNodeC6.nsl`

**Interfaces:**
- Consumes: `platform PortaBridge` constructed
  `socket:space:id:peer:port:`; `PortaBridge` accessors `cursor`, `acked`, `misses`,
  `rejected`, `pendingSize`; `platform timers` / `space` / `heapStats` /
  `Record` / `Map` / `OrderedCollection` / `monotonicMillis` / `spaceOverruns:`;
  `net udpBindPort:`.
- Produces: a console contract two later tasks depend on — the literal lines
  `PORTA NODE READY` and, per tick, `tick <N> misses <M> cursor <C> acked <A>`.

Constants only, plus the per-tick line (D9). The device node keeps plan 2's
`usingPlatform: platform net: net` shape — `SoakNode.nsl` uses exactly that shape on this
same board, so nothing about the class header changes for hardware.

- [ ] **Step 1: Write the node**

Create `~/workspace/tuvm/zephyr/PortaBridgeNodeC6.nsl`:

```
(*
PortaBridgeNodeC6 — PortaBridgeNode, on the C6, over a real Thread mesh. The
hardware half of design §9's gate (plan 3 of 3).

This file is PortaBridgeNode.nsl with three differences and no others:
  1. the constants below address the real gateway instead of loopback;
  2. `chip` answers #esp32c6, so obs/build says which silicon reported;
  3. `tick` prints one console line, because the gate must be able to tell a
     surviving drop from a drop that never fired — porta cannot witness the
     node's own `misses`, and a leg that passes either way is not a test (D8).
A change to the body of either file is a change to both.

The gateway is addressed by its MESH-LOCAL EID, not its OMR address: there is no
global IPv6 here and no DNS64, so literal addresses are the only ones that work,
and an MTD child has no OMR source address at all (rung-7). This node is an FTD,
but the ML-EID is correct either way and does not depend on the role.

Cadence stays the 2 s of plan 2. The adaptive controller is #115.

Dialect: `[ ... ] whileTrue:`, never `repeat`; state in instance slots so every
reaction block is a one-line self-send (the LD_SLOT gotcha).
expect (console, once attached):
PORTA NODE READY
tick 1 misses 0 cursor 0 acked 2
*)
class PortaBridgeNodeC6 usingPlatform: platform net: net = (
  | space bridge timers ticks ::= 0. |
) (
  nodeId    = ( ^ 'c6-bench' )
  (* gw85224-01's mesh-local EID, read from the OTBR on 2026-06-28 and unchanged
     since; the same literal SoakNode.nsl has been echoing against. *)
  gwAddr    = ( ^ 'fdc5:9109:9afe:97ea:9541:d963:f8d9:8567' )
  gwPort    = ( ^ 6969 )
  localPort = ( ^ 16970 )
  cadence   = ( ^ 2000 )   (* ms between check-ins *)
  chip      = ( ^ #esp32c6 )

  public main: platform2 args: args = (
    | sock tickChan |
    space:: platform space.
    sock:: net udpBindPort: self localPort.
    sock == nil ifTrue: [ ^ self fail: 'bind' ].
    bridge:: platform PortaBridge
      socket: sock space: space id: self nodeId peer: self gwAddr port: self gwPort.
    bridge subscribeStream: 'tel/log'.
    bridge subscribeStream: 'sys/reset'.
    (* NOT diag/spans/result: SpanCapture publishes ~1120 B against the 256 B value
       cap (tuvm#107). It must not be synced until that value is split into sibling
       cells at diag/spans/<span>. *)

    (* porta's writes land as ordinary cell writes, so the echo is an ordinary
       reaction — there is no protocol handler anywhere in this node. *)
    (space channel: 'goal/name') whenever: '' do: [ :v | self nameChanged: v ].

    space put: 'obs/build' value: (platform Record tag: #Build with: #chip -> self chip).

    timers:: platform timers.     (* hold ONE instance — it owns the pending registry *)
    space whenever: 'sys/timer' do: [ :e | timers dispatch: e ].
    tickChan:: space channel: 'sys/tick'.
    tickChan whenever: '' do: [ :m | self tick ].
    timers every: self cadence post: #go on: tickChan.

    'PORTA NODE READY' println.
    [ true ] whileTrue: [
      platform pumpEvents.        (* blocks until the cadence timer fires *)
      space drain ].
    ^ 0
  )

  (* One check-in, then one report. In that order: applying goal/** first means the
     health this node publishes describes the state porta just asked for, not the
     one before it. *)
  tick = (
    ticks:: ticks + 1.
    bridge checkIn.
    self publishHealth.
    space out: 'tel/log' value:
      (platform Record tag: #Log with: #level -> #info
                                 with: #text -> ('tick ' , ticks printString)).
    self report.
    ^ self
  )

  (* The console half of the gate. `misses` is the load-bearing number: leg 4
     fails if it never moves, because an injector that silently did nothing
     would otherwise be scored as a survived drop. *)
  report = (
    ('tick ' , ticks printString ,
     ' misses ' , bridge misses printString ,
     ' cursor ' , bridge cursor printString ,
     ' acked ' , bridge acked printString) println.
    ^ self
  )

  nameChanged: v = ( space put: 'obs/name' value: v. ^ self )

  (* Health. heapFree/heapLargest come from heapStats: largestFreeBlock is the
     fragmentation signal — the number that says a years-long node is dying slowly,
     which heapFree alone will not. wakes/pollTimeouts carry the tick count and the
     bridge's missed replies until the mode record gives them a stricter meaning. *)
  publishHealth = (
    | h pairs |
    h:: platform heapStats.
    pairs:: platform OrderedCollection new.
    pairs add: #uptimeMs -> platform monotonicMillis.
    pairs add: #bootId -> 1.
    pairs add: #wakes -> ticks.
    pairs add: #pollTimeouts -> bridge misses.
    pairs add: #heapFree -> (h heapSize - h liveBytes).
    pairs add: #heapLargest -> h largestFreeBlock.
    pairs add: #overruns -> (platform spaceOverruns: '').
    space put: 'obs/health' value:
      (platform Record tag: #Health fields: (platform Map fromPairs: pairs)).
    ^ self
  )

  fail: msg = ( msg println. ^ 1 )
) : (
  public packageUsing: manifest = ( ^ self new )
)
```

- [ ] **Step 2: Compile it against the C6 layout — a syntax and seam check**

```bash
cd ~/workspace/tuvm
python3 ~/workspace/nsl-tuvm/tools/ns_compile_oevm.py \
  zephyr/PortaBridgeNodeC6.nsl /tmp/porta_c6.oimg \
  --format=oimg --layout=zephyr/layout-esp32c6.json
```
Expected: writes `/tmp/porta_c6.oimg` with no diagnostics. A `not understood` here means
an accessor is missing from `kernel.nsl` — an app's `platform` is the KERNEL INSTANCE, so
every class it names needs an explicit accessor. All of the ones used above already exist
(they are plan 2's, plus `bridge cursor` / `acked`, both `public` in `kernel.nsl`).

- [ ] **Step 3: Diff it against the host node and confirm the differences are only the three claimed**

```bash
cd ~/workspace/tuvm && diff zephyr/PortaBridgeNode.nsl zephyr/PortaBridgeNodeC6.nsl
```
Expected: the header comment, the class name, `nodeId`/`gwAddr`/`gwPort`/`chip`, and the
added `report` method plus its `self report.` call. Anything else in the diff is drift —
remove it.

- [ ] **Step 4: Commit**

```bash
cd ~/workspace/tuvm && git add zephyr/PortaBridgeNodeC6.nsl && \
  git commit -m "feat(node): PortaBridgeNodeC6 — the space-sync node on real silicon (#120)"
```

---

### Task 4: The console reader, and the first flash

**Files:**
- Create: `~/workspace/tuvm/tests/porta_c6_console.py`

**Interfaces:**
- Consumes: the console contract from Task 3 (`PORTA NODE READY`, `tick N misses M …`).
- Produces: a log file at `--log` that Task 6 greps, and the exit convention
  `0` = the wanted pattern appeared, `1` = timeout.

- [ ] **Step 1: Write the reader**

Create `~/workspace/tuvm/tests/porta_c6_console.py`:

```python
#!/usr/bin/env python3
# Copyright (c) 2026 Ekorau LLC
"""Hold the ONE serial handle on the C6 console and tee it to a log.

A separate process for one reason: nothing else may open the port. Serial bytes
go to exactly one reader, so a monitor and a `cat` running together each see a
plausible-looking fraction of the output and neither looks wrong enough to
notice. This file is the only opener; everything else greps the log.

Decoding is errors="replace" on purpose. One NUL byte has turned a grep monitor
binary here before, and grep on a "binary file" answers nothing at all — which
reads exactly like a dead node.

The C6's console is uart0 on the CP210x bridge (zephyr/prj_esp32c6.conf:30),
which is NOT the interface esptool flashes over (USB-JTAG). So this handle can
be opened before a flash and held across it.

Usage:
  tests/porta_c6_console.py --port /dev/serial/by-id/usb-Silicon_Labs_... \\
      --log /tmp/c6_console.log --await 'PORTA NODE READY' --timeout 90
  tests/porta_c6_console.py --port ... --log ... --follow      # run until killed
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is not installed: python3 -m pip install --user pyserial")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", required=True)
    ap.add_argument("--await", dest="pattern", default=None,
                    help="exit 0 as soon as this substring appears")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--follow", action="store_true",
                    help="never exit on a pattern; read until killed")
    ap.add_argument("--reset", action="store_true",
                    help="pulse DTR/RTS to reset the board after opening")
    args = ap.parse_args()

    # Append, never truncate: the gate opens this reader once and greps the log
    # repeatedly, and a truncation between legs would silently erase leg 3's
    # evidence.
    with serial.Serial(args.port, args.baud, timeout=1) as ser, \
            open(args.log, "a", buffering=1) as log:
        if args.reset:
            ser.dtr = False
            ser.rts = True
            time.sleep(0.1)
            ser.rts = False

        deadline = time.time() + args.timeout
        while args.follow or time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            log.write(line + "\n")
            print(line, flush=True)
            if args.pattern and args.pattern in line:
                return 0
        return 0 if args.follow else 1


if __name__ == "__main__":
    sys.exit(main())
```

```bash
chmod +x ~/workspace/tuvm/tests/porta_c6_console.py
```

- [ ] **Step 2: Confirm pyserial is present and the port name is right**

```bash
python3 -c "import serial; print(serial.__version__)"
ls -l /dev/serial/by-id/
```
Expected: a version, and both by-id names present — `usb-Silicon_Labs_CP2102N_…` (console)
and `usb-Espressif_USB_JTAG_serial_debug_unit_40:4C:CA:45:47:A4` (flash). If the CP210x
name differs from the one in Task 6's script, update the script's default; do not rename
the device.

- [ ] **Step 3: Start the reader, THEN flash**

Two terminals, or one backgrounded. The reader first — it is held across the flash, which
is why the boot banner survives here where it would not on a single-interface board.

```bash
cd ~/workspace/tuvm
tests/porta_c6_console.py \
  --port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_ac2dce6f529ced118f43fd6be259fb3e-if00-port0 \
  --log /tmp/c6_console.log --follow &
CONSOLE_PID=$!

cd ~/workspace/tuvm/zephyr && ./run.sh --board esp32c6 --node PortaBridgeNodeC6.nsl --flash \
  --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf"
```

- [ ] **Step 4: Read the console honestly**

```bash
sleep 60; grep -n "PORTA NODE READY\|tick \|role\|CHILD\|ROUTER\|bind" /tmp/c6_console.log | tail -20
```

Expected, in order: Zephyr boot output, the Thread bringup reporting CHILD or ROUTER, then
`PORTA NODE READY`, then `tick N misses M cursor C acked A` every ~2 s. **`misses` will
climb**, and that is correct at this point — v1 is still on :6969 on the gateway and does
not speak this wire, so every check-in times out. That is positive evidence the node is
transmitting.

Three failures worth telling apart before touching anything:
- **No role line at all** → the mesh did not attach. A radio/dataset problem, nothing to do
  with porta. Check `prj_esp32c6_thread.local.conf` is present (it is gitignored) and that
  the OTBR on gw85224-01 is up.
- **Role line but no `PORTA NODE READY`** → `bind` failed, or the VM died. The `fail:`
  path prints `bind`.
- **`PORTA NODE READY` then silence** → the cadence timer never fired. Compare with
  `SoakNode.nsl`, which arms a timer the same way on this board.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/tuvm && git add tests/porta_c6_console.py && \
  git commit -m "test(spacesync): the single-handle C6 console reader (#120)"
```

---

### Task 5: Cut the gateway over to v2

**Files:** none in the repos — this is an operator task on `gw85224-01`, recorded here so
it is reproducible and so its one irreversible step is explicit.

**Interfaces:**
- Consumes: Task 1's binary.
- Produces: `~/porta-v2/porta` on gw85224-01, `porta serve` reachable at
  `[fdc5:9109:9afe:97ea:9541:d963:f8d9:8567]:6969`, and v1 stopped.

**Precondition — do not start this task until Tasks 1–4 are green.** Once v1 stops, the
LAN nodes at 192.0.2.40 and .241 have no gateway, and there is no migration between the
two wires (D1). The rollback artifact `~/porta-v1-0.7.6` was captured on 2026-08-01 and its
sha256 matches `/usr/bin/porta`.

- [ ] **Step 1: Build for the gateway and copy it**

Both boxes are x86_64, so this is an ordinary build.

```bash
cd ~/workspace/porta && go build -o /tmp/porta-v2 ./cmd/porta
ssh gw85224-01 'mkdir -p ~/porta-v2'
scp /tmp/porta-v2 gw85224-01:porta-v2/porta
ssh gw85224-01 '~/porta-v2/porta --version && ~/porta-v2/porta serve --help | grep test-drop-ack'
```
Expected: a version line, and the `--test-drop-ack` help text. If the flag is absent you
copied a stale binary.

- [ ] **Step 2: Retire v1 — the one irreversible step, run by the operator**

```bash
ssh gw85224-01 'sha256sum ~/porta-v1-0.7.6 /usr/bin/porta'   # confirm the rollback artifact first
```
Then, interactively (this prompts for a password; from Claude Code, prefix the command
with `!` so it runs in the session):

```bash
ssh -t gw85224-01 'sudo systemctl disable --now porta.service'
ssh gw85224-01 'systemctl is-active porta.service; ss -lunp | grep -w 6969 || echo "6969 is free"'
```
Expected: `inactive`, and `6969 is free`.

Rollback, at any later time:
```bash
ssh -t gw85224-01 'sudo cp ~/porta-v1-0.7.6 /usr/bin/porta && sudo systemctl enable --now porta.service'
```

- [ ] **Step 3: Start v2 by hand and watch the node find it**

```bash
ssh gw85224-01 'cd ~/porta-v2 && rm -f space.db && nohup ./porta serve --db space.db --udp :6969 --http-port 0 > serve.log 2>&1 & sleep 1; head -3 serve.log'
```
Expected: `porta: serving the space on udp :6969 (db=space.db)`. Note `:6969` and not
`0.0.0.0:6969` — Go's `ListenPacket("udp", ":6969")` gives a dual-stack socket, which is
what lets a Thread node on `fdc5::/64` reach it.

Then watch both ends:

```bash
tail -5 /tmp/c6_console.log
ssh gw85224-01 'cd ~/porta-v2 && ./porta get --db space.db c6-bench'
```
Expected: on the console, `misses` stops climbing and `acked` starts advancing; at porta,
`obs/health` and `obs/build` appear. That is legs 1 and 2 witnessed by hand, before any
script exists to score them.

If porta logs `rejecting datagram: malformed message`, the node's field names disagree
with `internal/wire/message.go` — read the message, it names the field. This should not
happen: plan 2 proved the same node code against the same server over loopback.

- [ ] **Step 4: Stop the hand-started server**

```bash
ssh gw85224-01 'pkill -f "porta serve --db space.db" ; sleep 1; ss -lunp | grep -w 6969 || echo "6969 is free"'
```
The gate starts and stops its own server from here on.

---

### Task 6: `porta_sync_c6.sh` — the four-leg hardware gate

**Files:**
- Create: `~/workspace/tuvm/tests/porta_sync_c6.sh`

**Interfaces:**
- Consumes: `tests/porta_c6_console.py`, `zephyr/run.sh`, `~/porta-v2/porta` on the
  gateway, and the console contract from Task 3.
- Produces: exit 0 and a final `PORTA SYNC C6 OK` line, or a `FAIL:` line naming the leg.

- [ ] **Step 1: Write the gate**

Create `~/workspace/tuvm/tests/porta_sync_c6.sh`:

```bash
#!/usr/bin/env bash
# porta space-sync plan 3 — THE HARDWARE GATE. PortaBridgeNodeC6.nsl on a real
# ESP32-C6, over a real Thread mesh, against a real `porta serve` on the border
# router, including a deliberate datagram drop.
#
# Legs 1-3 are plan 2's host gate in the same shape, so a failure of legs 1-3
# here is a HARDWARE or MESH failure and not a protocol one — that separation is
# the whole reason the host gate ran first. Leg 4 is new and is what design §9
# actually asks for.
#
# Assertions live at porta, because porta is the independently written
# implementation: if it says obs/health is there, two codebases agree about one
# wire. The console supplies only what porta cannot see — the node's own misses —
# and leg 4 fails if that number never moves, because an injector that silently
# did nothing must not be scored as a survived drop.
set -euo pipefail
cd "$(dirname "$0")/.."

GW=${GW:-gw85224-01}
GWDIR=${GWDIR:-porta-v2}
NODE=${NODE_ID:-c6-bench}
DROP_EVERY=${DROP_EVERY:-3}
TICKS_PER_LEG=${TICKS_PER_LEG:-10}
CADENCE_MS=2000
CONSOLE=${CONSOLE:-/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_ac2dce6f529ced118f43fd6be259fb3e-if00-port0}
LOG=${LOG:-/tmp/c6_console.log}
DO_FLASH=${DO_FLASH:-1}
CONSOLE_PID=""

say()  { echo "  $*"; }
fail() { echo "FAIL: $*" >&2; dump; exit 1; }
dump() {
  echo "--- last 20 console lines ---" >&2; tail -20 "$LOG" >&2 || true
  echo "--- last 20 gateway lines ---" >&2
  ssh "$GW" "tail -20 ~/$GWDIR/serve.log" >&2 || true
}
gwporta() { ssh "$GW" "cd ~/$GWDIR && ./porta $* --db space.db"; }
cleanup() {
  [ -n "$CONSOLE_PID" ] && kill "$CONSOLE_PID" 2>/dev/null || true
  ssh "$GW" 'pkill -f "porta serve --db space.db"' 2>/dev/null || true
}
trap cleanup EXIT

# ticks_from_log prints the distinct tick numbers porta has logged, ascending.
# grep -o rather than sed 's/.*tick //': the record's formatting puts punctuation
# after the number, which sort tolerates and arithmetic does not.
ticks_from_log() {
  gwporta "log $NODE --limit 500" | grep -o 'tick [0-9]\+' | awk '{print $2}' | sort -n -u
}

# assert_exactly_once enforces BOTH halves of the claim: no duplicates (rows ==
# distinct) and no gaps (the distinct ticks run contiguously). A radio can lose
# a datagram where loopback cannot, so the second half is not decoration here.
assert_exactly_once() {
  local leg="$1" rows uniq first last
  rows=$(gwporta "log $NODE --limit 500" | grep -c 'tel/log' || true)
  uniq=$(ticks_from_log | grep -c . || true)
  say "$leg: tel/log rows=$rows distinct ticks=$uniq"
  [ "$uniq" -ge 2 ] || fail "$leg: fewer than two tel/log entries — the stream never rode"
  [ "$rows" = "$uniq" ] || fail "$leg: a tel/log entry was delivered more than once"
  first=$(ticks_from_log | head -1)
  last=$(ticks_from_log | tail -1)
  [ $((last - first + 1)) -eq "$uniq" ] || \
    fail "$leg: a gap in the tick series ($first..$last holds only $uniq) — an entry was lost"
  say "$leg: exactly once, no gaps ($first..$last)"
}

# console_ticks answers how many tick lines the node has printed, and misses
# answers its current missed-reply count. Both come from the log the ONE reader
# writes; nothing here opens the port.
console_ticks()  { grep -c '^tick ' "$LOG" || true; }
console_misses() { grep '^tick ' "$LOG" | tail -1 | grep -o 'misses [0-9]\+' | awk '{print $2}'; }

wait_ticks() {  # wait_ticks <n-more> <what>
  local want=$(( $(console_ticks) + $1 )) budget=$(( $1 * CADENCE_MS / 1000 + 30 ))
  say "waiting for $1 more ticks ($2), budget ${budget}s"
  for _ in $(seq 1 "$budget"); do
    [ "$(console_ticks)" -ge "$want" ] && return 0
    sleep 1
  done
  fail "$2: the node stopped ticking (have $(console_ticks), want $want)"
}

gw_serve_start() {  # gw_serve_start [extra flags...]
  ssh "$GW" "cd ~/$GWDIR && nohup ./porta serve --db space.db --udp :6969 --http-port 0 $* >> serve.log 2>&1 & sleep 1"
  ssh "$GW" "grep -q 'serving the space' ~/$GWDIR/serve.log" || fail "the gateway did not start"
}
gw_serve_stop() { ssh "$GW" 'pkill -f "porta serve --db space.db"' || true; sleep 1; }

echo "[1/8] preflight"
[ -e "$CONSOLE" ] || fail "no console at $CONSOLE (ls /dev/serial/by-id/)"
ssh -o BatchMode=yes "$GW" true || fail "cannot ssh to $GW without a password"
ssh "$GW" "test -x ~/$GWDIR/porta" || fail "no v2 binary at ~/$GWDIR/porta on $GW (Task 5)"
ssh "$GW" "systemctl is-active porta.service" | grep -qv '^active$' || \
  fail "v1 porta.service is still running and owns :6969 (Task 5 Step 2)"
say "console, gateway binary and a free :6969 all present"

echo "[2/8] start the console reader — BEFORE any flash, and held across it"
: > "$LOG"
tests/porta_c6_console.py --port "$CONSOLE" --log "$LOG" --follow >/dev/null 2>&1 &
CONSOLE_PID=$!
sleep 1
kill -0 "$CONSOLE_PID" 2>/dev/null || fail "the console reader died immediately (port busy?)"

echo "[3/8] build and flash the node"
if [ "$DO_FLASH" = 1 ]; then
  ( cd zephyr && ./run.sh --board esp32c6 --node PortaBridgeNodeC6.nsl --flash \
      --conf-extra "prj_esp32c6_thread.conf;prj_esp32c6_thread_ftd.conf" ) \
    || fail "the build/flash failed"
else
  say "DO_FLASH=0 — using whatever is already on the board"
fi

echo "[4/8] start the gateway on a fresh db"
gw_serve_stop
ssh "$GW" "cd ~/$GWDIR && rm -f space.db"
gw_serve_start

echo "[5/8] wait for the node: attach, then READY"
for _ in $(seq 1 120); do grep -q 'PORTA NODE READY' "$LOG" && break; sleep 1; done
grep -q 'PORTA NODE READY' "$LOG" || \
  fail "the node never printed PORTA NODE READY — no role line means the mesh never attached"
say "node is up and attached"

echo "[6/8] legs 1 and 2"
# Leg 2 first: it needs no operator write at all — an idle check-in IS the
# heartbeat, so obs/health arriving is also the liveness proof.
for _ in $(seq 1 60); do gwporta "get $NODE obs" 2>/dev/null | grep -q 'obs/health' && break; sleep 1; done
gwporta "get $NODE obs" | grep -q 'obs/health' || fail "leg 2: obs/health never reached porta"
gwporta "get $NODE obs" | grep -q 'obs/build'  || fail "leg 2: obs/build missing"
gwporta "get $NODE obs" | grep -q 'esp32c6'    || fail "leg 2: obs/build does not say esp32c6"
say "leg 2 PASS: obs/health and obs/build are in porta's cell tree, from real silicon"

gwporta "put $NODE goal/name c6" >/dev/null
for _ in $(seq 1 60); do gwporta "get $NODE obs/name" 2>/dev/null | grep -q 'c6' && break; sleep 1; done
gwporta "get $NODE obs/name" | grep -q 'c6' || fail "leg 1: goal/name never converged to obs/name"
say "leg 1 PASS: goal/name converged to obs/name — the diff IS the view"

echo "[7/8] leg 3: exactly once, no injected drop"
wait_ticks "$TICKS_PER_LEG" "leg 3"
assert_exactly_once "leg 3"
say "leg 3 PASS"

echo "[8/8] leg 4: exactly once ACROSS a deliberate drop"
# Restart onto the flag keeping the SAME db, and leave the node running. A fresh
# db would reset porta's high-water to 0 while the node's acked kept climbing,
# breaking contiguity for reasons that have nothing to do with the drop. Reusing
# it also means leg 4 incidentally witnesses that the high-water survives a
# gateway restart; the node sees the restart window as ordinary loss and
# re-pushes, which is the designed behaviour.
MISSES_BEFORE=$(console_misses)
gw_serve_stop
gw_serve_start --test-drop-ack "$DROP_EVERY"
ssh "$GW" "grep -q 'TEST-DROP ACTIVE' ~/$GWDIR/serve.log" || fail "leg 4: the injector did not arm"
wait_ticks "$TICKS_PER_LEG" "leg 4"

DROPS=$(ssh "$GW" "grep -c 'TEST-DROP: ack' ~/$GWDIR/serve.log" || true)
MISSES_AFTER=$(console_misses)
say "leg 4: gateway swallowed $DROPS acks; node misses $MISSES_BEFORE -> $MISSES_AFTER"
[ "$DROPS" -ge 2 ] || fail "leg 4: the gateway swallowed $DROPS acks — the drop never fired"
[ "$MISSES_AFTER" -gt "$MISSES_BEFORE" ] || \
  fail "leg 4: the node's misses never moved — it did not notice the drop, so nothing was tested"
assert_exactly_once "leg 4"
say "leg 4 PASS: the node re-pushed rows porta had already applied, and porta deduped them"

echo "PORTA SYNC C6 OK"
```

```bash
chmod +x ~/workspace/tuvm/tests/porta_sync_c6.sh
```

- [ ] **Step 2: Run it and read the first failure honestly**

```bash
cd ~/workspace/tuvm && tests/porta_sync_c6.sh
```

Expect a real failure on the first run. Work it with `superpowers:systematic-debugging`.
The candidates, in the order they are likely:

- **`the node stopped ticking`** — the cadence timer stalled, or the console reader died.
  Check `kill -0 $CONSOLE_PID` and whether `/tmp/c6_console.log` is still growing.
- **`a gap in the tick series`** — a genuine loss that never got re-pushed. Before blaming
  the gate, check `pendingSize` on the console line and porta's `serve.log`: a 512 B batch
  is 5–6 6LoWPAN fragments, and a value that grew past the 256 B cap gets the whole push
  rejected with `#err`.
- **`a tel/log entry was delivered more than once`** — this is the real thing the gate
  hunts. Do not weaken the assertion; it means the dedupe did not hold.
- **Intermittent, one leg in several runs** — suspect `awaitReply` taking the first
  datagram that arrives, whatever it is: a batch delayed past the pull's budget and
  arriving during the push's wait is consumed as if it were the ack, and the real ack is
  then missed. Microseconds on loopback, milliseconds over a mesh. If this is it, it is a
  bridge defect to file (Task 8), not a gate bug to paper over.

- [ ] **Step 3: Get it to green, then commit**

```bash
cd ~/workspace/tuvm && git add tests/porta_sync_c6.sh && \
  git commit -m "test(spacesync): THE HARDWARE GATE — C6 over Thread vs real porta, with a deliberate drop (#120)"
```

---

### Task 7: Five consecutive runs

**Files:** none — this task produces evidence, not code.

- [ ] **Step 1: Run the gate five times, logging each**

```bash
cd ~/workspace/tuvm
for i in 1 2 3 4 5; do
  echo "=== run $i ==="
  tests/porta_sync_c6.sh > /tmp/c6_gate_$i.log 2>&1 && echo "run $i PASS" || echo "run $i FAIL"
done
grep -c PASS /tmp/c6_gate_*.log
```
Expected: five `PORTA SYNC C6 OK`. Each run re-flashes, which is deliberate: it exercises
cold boot at cursor 0, the designed full-replay path (#117).

- [ ] **Step 2: Do not round a flake up to a pass**

If any run fails, that run is the finding. Diagnose it before continuing; if it is the
`awaitReply` window, file it (Task 8) and state plainly in the promotion how many runs
were clean out of how many. A gate that passes four times in five is a gate that has
found something.

- [ ] **Step 3: Keep the evidence**

```bash
mkdir -p ~/workspace/tuvm/docs/spikes && \
  cp /tmp/c6_gate_1.log ~/workspace/tuvm/docs/spikes/2026-08-01-c6-gate-run1.log
cd ~/workspace/tuvm && git add docs/spikes/2026-08-01-c6-gate-run1.log && \
  git commit -m "docs(spacesync): the C6 gate's first clean run, kept as evidence (#120)"
```

---

### Task 8: Promotion and the loose ends

**Files:**
- Modify: `~/workspace/tuvm/review/tests/CLAIMS.md`,
  `~/workspace/tuvm/review/program-sequence.md`,
  `~/workspace/tuvm/docs/next-steps-overview.md`,
  `~/workspace/tuvm/protocol/PROTOCOL-NSL.md`

The promotion rule: a rung is not closed until its durable knowledge is in a mainline
document of record. The dated design stays behind as the historical "why".

- [ ] **Step 1: Close the CLAIMS row**

In `~/workspace/tuvm/review/tests/CLAIMS.md`, find the space-sync row (it ends
`| DONE (host); hardware gate is plan 3 |`). Extend its evidence column with the hardware
gate and change the status:

```markdown
… hardware: `tuvm/tests/porta_sync_c6.sh` — **THE GATE** (PortaBridgeNodeC6.nsl on an ESP32-C6 over a real Thread mesh against `porta serve` on gw85224-01: `goal/name` converges to `obs/name`, `obs/health`/`obs/build` reach porta's cell tree, and `tel/log` arrives exactly once — no duplicates AND no gaps — across a deliberate post-commit ack drop injected by `porta serve --test-drop-ack 3`); design: `tuvm/docs/design/2026-08-01-porta-space-sync-c6-gate-design.md`; plan: `tuvm/docs/superpowers/plans/2026-08-01-porta-space-sync-c6-gate.md` | DONE |
```

- [ ] **Step 2: Close the rung in the ladder**

In `~/workspace/tuvm/review/program-sequence.md`, update the space-sync rung entry to
closed, and state what the radio added over loopback — this is the part that is not
recoverable from the code:

- the `seq <= hw` dedupe was **witnessed**, not reasoned about: the node re-pushed rows
  porta had already applied and porta discarded them;
- a 512 B batch is 5–6 6LoWPAN fragments over a 127 B 802.15.4 PHY, so real loss is
  expected on a mesh and the gate asserts *no duplicates and no gaps* rather than
  `misses == 0`;
- a gateway restart mid-stream is indistinguishable from loss to the node, and the
  high-water survives it.

- [ ] **Step 3: Amend PROTOCOL-NSL §4.1**

Append to `~/workspace/tuvm/protocol/PROTOCOL-NSL.md` §4.1, after the plan-2 paragraph:

```markdown
**What hardware added (2026-08-01, plan 3 — the C6 gate).** The exchange was witnessed on
an ESP32-C6 over a real Thread mesh against `porta serve` on the border router, including
a deliberate post-commit ack drop. Two facts belong to the protocol rather than to the
bench:

- **Exactly-once is two claims.** No duplicates *and* no gaps. On loopback only the first
  can fail, so a gate that checks only duplication is green for the wrong reason.
- **A 512 B batch is not one radio frame.** The 802.15.4 PHY payload is 127 B, so a
  full-size datagram is five or six 6LoWPAN fragments and losing any one kills the whole
  datagram. Batch sizing trades against loss probability on a mesh in a way it does not on
  a LAN, and a node's `misses` being non-zero is normal operation, not a fault.
```

- [ ] **Step 4: Update the overview**

In `~/workspace/tuvm/docs/next-steps-overview.md`, Part 2, "1. porta → channels": the rung
is closed, all three plans executed, and the hardware gate is re-runnable as
`tuvm/tests/porta_sync_c6.sh`.

- [ ] **Step 5: File what was deliberately not built**

```bash
cd ~/workspace/porta
gh issue create --title "v1 LAN nodes at 192.0.2.40/.241 have no gateway since v2 took :6969" \
  --body "Plan 3 D1. porta.service (v1, 0.7.6) was disabled on gw85224-01 on 2026-08-01 so the space-sync gateway could take :6969. The two LAN nodes that were still checking in (most recently 2026-07-30) have had no gateway since, and there is no migration path — v1's JSON report wire and v2's CBOR space-sync wire share nothing. Decide their fate: port them to the space-sync wire, retire them, or run v1 alongside on another port. Rollback artifact: ~/porta-v1-0.7.6 on gw85224-01 (sha256 matches the binary that was running)."

cd ~/workspace/tuvm
gh issue create --title "PortaBridge>>awaitReply consumes the first datagram, whatever it is" \
  --body "Plan 3, §3 of the C6 gate design. awaitReply returns the first datagram to arrive within its budget, so a #batch delayed past the pull's budget and arriving during pushOnce's wait is consumed as if it were an ack; applyAck: rejects it (returning false) and the real ack is then missed, costing a re-push. Microseconds of exposure on loopback, milliseconds over a mesh. Fix is to check the record tag and keep waiting on a mismatch rather than returning the first thing that lands. Filed whether or not it was observed during the gate runs — it is a real narrowing of the bridge's assumptions that loopback could not have surfaced."
```

- [ ] **Step 6: Commit the promotion**

```bash
cd ~/workspace/tuvm && git add review/tests/CLAIMS.md review/program-sequence.md docs/next-steps-overview.md && \
  git commit -m "docs(spacesync): close the rung — the C6 hardware gate is green (#120)"
cd ~/workspace/tuvm && git add protocol/PROTOCOL-NSL.md && \
  git commit -m "docs(protocol): §4.1 records what hardware added (#120)"
```

---

## Self-Review

**1. Spec coverage.** D1 (v2 replaces v1, retired once by hand, rollback captured) → Task
5 Step 2, with the precondition stated and the orphaned nodes filed in Task 8. D2 (fresh
db per gate run, not per leg) → Task 6 step [4/8] plus the leg-4 comment explaining why
legs 3 and 4 share one. D3 (the node waits for the mesh; the gate waits for the node) →
Task 6 step [5/8]. D4 (`--http-port 0`) → Tasks 5 and 6. D5 (assertions at porta, console
corroborates) → every leg asserts via `gwporta`, and leg 4 additionally requires the
console's `misses` to move. D6 (the drop is a gateway flag firing after the commit) → Task
1, with the ordering both implemented and tested. D7 (exactly-once is two assertions) →
Task 2 for the host gate, `assert_exactly_once` for the C6 gate. D8 (a drop that never
fired must fail the leg) → the two guards on `$DROPS` and `$MISSES_AFTER`. D9 (a sibling
node file) → Task 3, with a diff step that enforces "constants and the console line only".
§5's evidence-to-close (Go test, host gate regression, five runs) → Tasks 1, 2 and 7. §6's
promotion → Task 8.

**2. Placeholders.** None: every step carries the command or the code. The one judgement
call left to the implementer is Task 6 Step 2 — which failure appears first — and it is
enumerated with four named candidates rather than left open. Task 4's Step 2 says to
update the script default if the CP210x by-id name differs, which is a real environmental
variable rather than an unwritten decision.

**3. Type consistency.** `SetDropAckEvery(n int)` is defined in Task 1 and used in Task 1's
`serve.go` wiring and nowhere else. `swallowAck([]byte) bool` is internal to `server.go`.
The console contract — `PORTA NODE READY` and `tick <N> misses <M> cursor <C> acked <A>` —
is produced by Task 3's `report` and consumed by Task 6's `console_ticks` /
`console_misses` greps (`^tick `, `misses [0-9]\+`) in exactly that spelling. The node id
is `c6-bench` in Task 3's `nodeId`, Task 5's `porta get` and Task 6's `$NODE` default.
`--db space.db` is passed on every `gwporta` call by construction, since the helper appends
it. `--test-drop-ack` is spelled identically in `serve.go`, `serve_test.go`, Task 5's help
check and Task 6's `gw_serve_start`.

**One gap I could not close from here:** `TestNewServeCmdRegistersFlags` in
`internal/portacli/serve_test.go` may enumerate the expected flag set; if it does, Task 1
Step 8 will fail and the new flag must be added to its list. The step says so rather than
pretending to know.
