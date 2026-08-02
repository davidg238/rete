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

	// TEST-ONLY fault injection — see SetDropAckEvery.
	dropAckEvery int
	acks         int
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
	bootID, err := intField(r.Fields, "bootId")
	if err != nil {
		return nil, err
	}
	// A pull carries the cursor the node has ALREADY applied, so recording
	// it here is what makes "the node's cursor passed seq N" better evidence
	// of delivery than v1's delivered_at ever was.
	if err := s.st.NoteCursor(id, cursor); err != nil {
		return nil, err
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
	deliveredHi, _, err := DecodeBatch(resp)
	if err != nil {
		return nil, err
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
	bootID, err := intField(r.Fields, "bootId")
	if err != nil {
		return nil, err
	}
	ev, ok := field(r.Fields, "entries")
	if !ok {
		return nil, fmt.Errorf("%w: missing #entries", errShape)
	}
	entries, err := listToEntries(ev)
	if err != nil {
		return nil, err
	}
	lo := int64(0)
	if len(entries) > 0 {
		lo = entries[0].Seq
	}
	hw, err := s.st.Ingest(id, bootID, entries)
	if err != nil {
		return nil, err
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
