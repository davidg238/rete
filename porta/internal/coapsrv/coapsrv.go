// Copyright (c) 2026 Ekorau LLC

// Package coapsrv is porta's CoAP listener (rete#505): an adapter only. It
// forwards a POST /space body to wire.Server.HandleFrom unchanged and answers
// GET /blob/<hash> by Block2 from the blob store. Check-in rules live in wire,
// not here: a refusal is an #err record in a 2.04, exactly as UDP carries it.
//
// The library's blockwise layer is OFF, because it is one setting for the
// whole server and would split a /space batch; only /blob is blockwise, and
// this package slices it (design §5.1). There is no per-request log line:
// HandleFrom already logs its rejections, and the peer string reads as UDP's.
package coapsrv

import (
	"bytes"
	"context"
	"io"
	"net"

	"github.com/davidg238/porta/internal/blob"
	"github.com/davidg238/porta/internal/wire"
	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/mux"
	coapnet "github.com/plgd-dev/go-coap/v3/net"
	"github.com/plgd-dev/go-coap/v3/options"
	"github.com/plgd-dev/go-coap/v3/udp"
)

type Server struct {
	ex    *wire.Server
	blobs *blob.Store
	logf  func(string, ...any)
}

func New(ex *wire.Server, blobs *blob.Store, logf func(string, ...any)) *Server {
	if logf == nil {
		logf = func(string, ...any) {}
	}
	return &Server{ex: ex, blobs: blobs, logf: logf}
}

// Serve runs the CoAP listener on an already-bound socket, mirroring
// blockserv.Server.Serve. Takes ownership of pc. Returns nil once ctx is
// cancelled; a non-nil error only if the listener dies otherwise.
func (s *Server) Serve(ctx context.Context, pc *net.UDPConn) error {
	r := mux.NewRouter()
	r.SetErrorHandler(func(err error) { s.logf("porta: coap: %v", err) })
	if err := r.Handle("/space", mux.HandlerFunc(s.space)); err != nil {
		pc.Close()
		return err
	}
	srv := udp.NewServer(
		options.WithMux(r),
		options.WithBlockwise(false, maxSZX, 0),
		options.WithErrors(func(err error) { s.logf("porta: coap: %v", err) }),
	)
	// Cancelling a context handed to the library does not return Serve; Stop
	// does (plan fact 5).
	go func() { <-ctx.Done(); srv.Stop() }()
	return srv.Serve(coapnet.NewUDPConn("udp", pc))
}

func (s *Server) reply(w mux.ResponseWriter, code codes.Code, cf message.MediaType, body []byte, opts ...message.Option) {
	// A nil body must reach the library as a nil interface, not a nil *Reader.
	var rd io.ReadSeeker
	if body != nil {
		rd = bytes.NewReader(body)
	}
	err := w.SetResponse(code, cf, rd, opts...)
	if err != nil {
		s.logf("porta: coap: cannot set response %v: %v", code, err)
	}
}

// space is POST /space: the body is exactly a UDP check-in datagram and the
// reply body exactly HandleFrom's bytes, in a 2.04.
func (s *Server) space(w mux.ResponseWriter, r *mux.Message) {
	if r.Code() != codes.POST {
		s.reply(w, codes.MethodNotAllowed, message.TextPlain, nil)
		return
	}
	var body []byte
	if r.Body() != nil {
		b, err := r.ReadBody()
		if err != nil {
			s.reply(w, codes.BadRequest, message.TextPlain, nil)
			return
		}
		body = b
	}
	resp := s.ex.HandleFrom(body, w.Conn().RemoteAddr().String())
	s.reply(w, codes.Changed, message.AppCBOR, resp)
}
