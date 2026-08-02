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
}

func NewServer(st *space.Store, logf func(string, ...any)) *Server {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	return &Server{st: st, logf: logf}
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
		s.logf("porta: rejecting datagram: %v", err)
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
		if _, err := pc.WriteTo(resp, peer); err != nil {
			s.logf("porta: reply to %s failed: %v", peer, err)
		}
	}
}
