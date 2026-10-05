// Copyright (c) 2026 Ekorau LLC

// internal/wire/server.go
package wire

import (
	"context"
	"fmt"
	"net"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

type Server struct {
	st   *space.Store
	logf func(string, ...any)

	// TEST-ONLY fault injection — see SetDropAckEvery. acks is a FLEET-WIDE
	// counter (one Server serves every node on the port), so "every Nth ack"
	// is a global modulo across whichever nodes are checking in — harmless on
	// a single-node bench, but a second node would steal some of the first
	// node's drops and vice versa. Fine for what this exists to test.
	dropAckEvery int
	acks         int

	// TEST-ONLY fault injection — see SetRefuseAll and SetDropAll.
	refuseAll bool
	dropAll   bool
}

func NewServer(st *space.Store, logf func(string, ...any)) *Server {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	return &Server{st: st, logf: logf}
}

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

// SetRefuseAll makes every datagram answer #err at the DOOR — before handle(),
// so nothing is decoded, nothing is ingested and no exchange row is written.
// TEST ONLY; false (the zero value) disables it, so a production gateway is
// unaffected by the field's existence.
//
// This is rete#232's injector: the supported form of the throwaway env-gated
// early return the #199 spike used. Refusing at the door rather than after
// ingest is load-bearing — the scenario reproduces a node whose frames are
// REJECTED (a mis-versioned peer, a malformed message), and #199's witnessed
// death was a reject-at-decode: the node got an #err to every exchange while
// `misses` read 0 the whole way. The reply is never silence: a node that gets
// no reply cannot distinguish rejection from loss (Handle's own contract).
//
// Not concurrency-safe, and does not need to be: set before the loop starts.
func (s *Server) SetRefuseAll(on bool) { s.refuseAll = on }

// SetDropAll makes the UDP loop discard every datagram at the DOOR and answer
// NOTHING. TEST ONLY; false (the zero value) disables it, so a production
// gateway is unaffected by the field's existence.
//
// This is rete#269's injector, and it is SetRefuseAll's mirror: same position
// (the door — nothing is decoded, nothing is ingested, no exchange row is
// written), opposite reply. Refusal answers #err; this answers silence.
//
// The distinction is the whole point, and this package already named it before
// there was a gate for it: Handle's contract is that it NEVER returns nil,
// because "a node that gets no reply cannot distinguish a rejection from a lost
// datagram". #199's death and #232's gate are both the REFUSED path — `ref`
// climbing, `misses` at 0. On 2026-08-27 c6spare died the same death 204 s after
// its uplink went silent against a LIVE gateway two other nodes were reaching:
// `ref 0`, `rej 0`, `misses` climbing 2/tick. That path had no injector.
//
// It lives HERE, in the loop, and not in Handle, precisely so Handle's never-nil
// contract stays true: silence is a property of the wire, not of the protocol.
// It drops BEFORE handle rather than after (which is where swallowAck sits, for
// its own good reason) because the witnessed failure ingested nothing — porta
// held no exchange row for c6spare after 10:40:09, so a drop that committed
// first would reproduce a lost ACK and not a lost UPLINK.
//
// Not concurrency-safe, and does not need to be: set before the loop starts.
func (s *Server) SetDropAll(on bool) { s.dropAll = on }

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

// Handle turns one request datagram into one response datagram. It is pure
// (bytes in, bytes out) so the whole protocol is testable without a socket,
// and it NEVER returns nil: a node that gets no reply cannot distinguish a
// rejection from a lost datagram, and would retry forever.
func (s *Server) Handle(req []byte) []byte { return s.HandleFrom(req, "") }

// HandleFrom is Handle plus the address the datagram arrived from, stamped on
// the exchange so the fleet keeps the transport breakdown v1 read out of
// nodes.source_addr (design §4.2). An empty peer is legal and never clobbers.
func (s *Server) HandleFrom(req []byte, peer string) []byte {
	if s.refuseAll {
		s.logf("porta: TEST-REFUSE: refusing datagram from %s (%d bytes)", peer, len(req))
		return encodeErr("refused by TEST-REFUSE (--test-refuse-all)")
	}
	resp, err := s.handle(req, peer)
	if err != nil {
		// Name the sender and show the bytes. A bare "rejecting datagram:
		// trailing bytes" cannot distinguish one misbehaving node from another,
		// nor a framing bug from a stray sender on the port — which is exactly
		// the ambiguity that cost an afternoon during the C6 bring-up. The hex
		// is capped so a hostile or confused peer cannot flood the log.
		s.logf("porta: rejecting datagram from %s (%d bytes): %v; hex=%x",
			peer, len(req), err, req[:min(len(req), 96)])
		return encodeErr(err.Error())
	}
	return resp
}

func (s *Server) handle(req []byte, peer string) ([]byte, error) {
	v, err := cval.Decode(req)
	if err != nil {
		return nil, err
	}
	r, ok := v.(cval.Record)
	if !ok {
		return nil, fmt.Errorf("%w: not a record", errShape)
	}
	switch r.Tag {
	case "pull":
		return s.handlePull(r, peer)
	case "push":
		return s.handlePush(r, peer)
	}
	return nil, fmt.Errorf("%w: unknown message #%s", errShape, r.Tag)
}

func (s *Server) handlePull(r cval.Record, peer string) ([]byte, error) {
	id, err := strField(r.Fields, "id")
	if err != nil {
		return nil, err
	}
	cursor, err := intField(r.Fields, "cursor")
	if err != nil {
		return nil, err
	}
	bootID, err := intField(r.Fields, "bid")
	if err != nil {
		return nil, err
	}
	// A pull carries the cursor the node has ALREADY applied, so recording
	// it here is what makes "the node's cursor passed seq N" better evidence
	// of delivery than v1's delivered_at ever was. It also carries `hw` — what
	// the node believes porta has accepted FROM it — which was decoded and
	// thrown away until 2026-08-05. Required, not optional: a peer that cannot
	// name its own watermark is mis-versioned, and decoding here is closed by
	// design. Both go to the store together, because the RESTART test compares
	// the declared cursor against the stored one and the order of those two
	// writes is the whole correctness of it (see Store.NoteCheckIn).
	nodeHW, err := intField(r.Fields, "hw")
	if err != nil {
		return nil, err
	}
	// rete#272 D3.3 — the node's count of this boot's check-ins. Required for the
	// same reason hw is: decoding is closed, and a peer that cannot name it is an
	// image from before the verdict could judge progress.
	wak, err := intField(r.Fields, "wak")
	if err != nil {
		return nil, err
	}
	// A cursor this store never issued is refused, not recorded (the same
	// closed-decoding posture as an unknown tag): it means the node is synced
	// to a DIFFERENT store's history — a reset or a restore — and both
	// recording it (poisons the replay guard's receipt) and serving against
	// it (hands rows to a peer whose cursor means something else) are guesses.
	// The reply is the ordinary #err, which the node counts in `refused` and
	// surfaces via lastErrMsg (rete#199); its RAM cursor means the next
	// reboot resyncs from 0 cleanly.
	outSeq, err := s.st.OutSeq(id)
	if err != nil {
		return nil, err
	}
	if cursor > outSeq {
		return nil, fmt.Errorf("cursor %d is beyond this store's out-seq %d — node and store disagree about history; a node reboot resyncs from 0", cursor, outSeq)
	}
	was, restarted, err := s.st.NoteCheckIn(id, cursor, nodeHW, bootID, wak)
	if err != nil {
		return nil, err
	}
	// Logged for the same reason the skip line is: a correction nobody can see
	// is indistinguishable from one that never fired.
	if restarted {
		s.logf("porta: %s restarted (boot %d) — high-water %d -> %d, so this boot's rows are not mistaken for duplicates",
			id, bootID, was, nodeHW)
	}
	// A node with no persistent boot identity cannot be told apart from one whose
	// ack we merely missed, which is the whole of #162. Said once per check-in
	// rather than swallowed: this node is running WITHOUT the protection every
	// other node has, and the only symptom of that is silence.
	if bootID == 0 {
		s.logf("porta: %s declares bootId 0 — no persistent boot identity, so a reboot cannot be distinguished from a missed ack for this node",
			id)
	}
	entries, hi, err := s.st.OutboundSince(id, cursor, space.MaxBatchBytes)
	if err != nil {
		return nil, err
	}
	// Build the datagram first: capping may trim the tail, and the exchange
	// row must record the hi the node was actually told, not the one the
	// store offered.
	resp, err := encodeBatchCapped(hi, entries, space.MaxBatchBytes)
	if err != nil {
		return nil, err
	}
	deliveredHi, delivered, err := DecodeBatch(resp)
	if err != nil {
		return nil, err
	}
	// Post-capping, and only when something rode the batch: an empty batch's
	// hi is the node's own cursor echoed back, not a delivery.
	if len(delivered) > 0 {
		if err := s.st.NoteServed(id, deliveredHi); err != nil {
			return nil, err
		}
	}
	if err := s.st.RecordExchange(id, bootID, cursor, deliveredHi, 0, 0, peer); err != nil {
		return nil, err
	}
	return resp, nil
}

func (s *Server) handlePush(r cval.Record, peer string) ([]byte, error) {
	id, err := strField(r.Fields, "id")
	if err != nil {
		return nil, err
	}
	bootID, err := intField(r.Fields, "bid")
	if err != nil {
		return nil, err
	}
	ev, ok := field(r.Fields, "ent")
	if !ok {
		return nil, fmt.Errorf("%w: missing #ent", errShape)
	}
	entries, err := listToEntries(ev)
	if err != nil {
		return nil, err
	}
	lo := int64(0)
	if len(entries) > 0 {
		lo = entries[0].Seq
	}
	hw, skipped, err := s.st.Ingest(id, bootID, entries)
	if err != nil {
		return nil, err
	}
	// Only when skipped > 0, so an ordinary push stays quiet. This is what
	// makes the seq <= hw dedupe OBSERVABLE rather than merely reasoned
	// about: previously it was a silent skip, provable only by the ABSENCE
	// of a duplicate — which reads identically to a node that never
	// re-pushed at all.
	if skipped > 0 {
		s.logf("porta: push from %s skipped %d already-applied row(s) (high-water %d)",
			id, skipped, hw)
	}
	if err := s.st.RecordExchange(id, bootID, 0, 0, lo, hw, peer); err != nil {
		return nil, err
	}
	return encodeAck(hw)
}

// ListenAndServe runs the UDP loop. One datagram in, one datagram out; no
// connection state, so a sleepy node costs the gateway nothing between
// check-ins.
func (s *Server) ListenAndServe(ctx context.Context, addr string) error {
	pc, err := net.ListenPacket("udp", addr)
	if err != nil {
		return err
	}
	return s.Serve(ctx, pc)
}

// Serve runs the loop on an already-bound socket, mirroring
// blockserv.Server.Serve (rete#176). Split for rete#158: a test that needs
// the listener's address hands its own bound conn over instead of the
// listen/close/re-listen dance, whose unbound window made the drop-ack
// harness flake. Takes ownership of pc.
func (s *Server) Serve(ctx context.Context, pc net.PacketConn) error {
	defer pc.Close()
	go func() { <-ctx.Done(); pc.Close() }()

	buf := make([]byte, 1500) // the IPv6 MTU bounds a legal datagram
	for {
		n, peer, err := pc.ReadFrom(buf)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		req := make([]byte, n)
		copy(req, buf[:n])
		if s.dropAll {
			// rete#269: the uplink is gone. Nothing decoded, nothing ingested,
			// no reply — which is what a node sees when its datagrams stop
			// arriving at a gateway that is otherwise perfectly healthy.
			s.logf("porta: TEST-SILENT: dropping datagram from %s (%d bytes) — no reply, nothing ingested (rete#269)", peer, n)
			continue
		}
		resp := s.HandleFrom(req, peer.String())
		if s.swallowAck(resp) {
			s.logf("porta: TEST-DROP: ack %d to %s swallowed after commit", s.acks, peer)
			continue
		}
		if _, err := pc.WriteTo(resp, peer); err != nil {
			s.logf("porta: reply to %s failed: %v", peer, err)
		}
	}
}
