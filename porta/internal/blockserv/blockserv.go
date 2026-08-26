// Copyright (c) 2026 Ekorau LLC

// Package blockserv answers the node's stage-time block pulls (#25 design §2):
// a stateless stop-and-wait fetch by content hash — the CoAP blockwise GET
// stand-in, in a fixed binary frame (D1 note). Resume is the client asking
// from any block; the server holds nothing per client.
package blockserv

import (
	"context"
	"encoding/binary"
	"net"

	"github.com/davidg238/porta/internal/blob"
)

const BlockSize = 256

type Server struct{ st *blob.Store }

func New(st *blob.Store) *Server { return &Server{st: st} }

func fail(msg string) []byte { return append([]byte{0x7F}, msg...) }

// Handle turns one request datagram into one response datagram. It is pure
// (bytes in, bytes out) and never returns nil, matching wire.Server.Handle.
func (s *Server) Handle(req []byte) []byte {
	if len(req) != 37 || req[0] != 0x01 {
		return fail("bad request")
	}
	body, err := s.st.Get(req[1:33])
	if err != nil {
		return fail("unknown hash")
	}
	blk := binary.BigEndian.Uint32(req[33:37])
	off := int64(blk) * BlockSize
	if off > int64(len(body)) || (off == int64(len(body)) && blk != 0) {
		return fail("block past end")
	}
	end := off + BlockSize
	more := byte(0)
	if end < int64(len(body)) {
		more = 1
	} else {
		end = int64(len(body))
	}
	resp := make([]byte, 6, 6+end-off)
	resp[0] = 0x02
	binary.BigEndian.PutUint32(resp[1:5], blk)
	resp[5] = more
	return append(resp, body[off:end]...)
}

// ListenAndServe runs the UDP loop, mirroring wire.Server.ListenAndServe: one
// datagram in, one datagram out, no connection state and no per-peer state.
func (s *Server) ListenAndServe(ctx context.Context, addr string) error {
	pc, err := net.ListenPacket("udp", addr)
	if err != nil {
		return err
	}
	return s.Serve(ctx, pc)
}

// Serve runs the loop on an already-bound socket. Split from ListenAndServe
// for rete#176: serve needs to bind BEFORE spawning the goroutine, so a bind
// failure can degrade the plane instead of killing the daemon later through
// the listener error channel. Takes ownership of pc.
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
		resp := s.Handle(req)
		_, _ = pc.WriteTo(resp, peer)
	}
}
