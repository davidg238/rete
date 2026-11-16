// Copyright (c) 2026 Ekorau LLC

package coapsrv

import (
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"

	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
	"github.com/plgd-dev/go-coap/v3/mux"
	"github.com/plgd-dev/go-coap/v3/net/blockwise"
)

// maxSZX is the block size blockserv.BlockSize names, as an SZX. A test pins
// the two together; there is no second literal 256.
var maxSZX = blockwise.SZX256

var errBlock = errors.New("block option out of range")

// window answers which bytes of an n-byte blob a request names, and the Block2
// value the reply carries. has is false when the request carried no Block2:
// that is block 0 at maxSZX. A client asking for a block larger than maxSZX
// gets the block at the SAME byte offset at maxSZX, renumbered (RFC 7959
// §2.4); a smaller size is honoured. Nothing is kept between requests, so a
// client resumes by asking for any block number.
func window(n int64, block2 uint32, has bool) (off, end int64, reply uint32, err error) {
	reqSZX, num := maxSZX, int64(0)
	if has {
		var derr error
		reqSZX, num, _, derr = blockwise.DecodeBlockOption(block2)
		if derr != nil {
			return 0, 0, 0, derr
		}
		if reqSZX > blockwise.SZX1024 { // BERT is CoAP over TCP, not UDP
			return 0, 0, 0, fmt.Errorf("%w: szx %d", errBlock, reqSZX)
		}
	}
	off = num * int64(reqSZX.Size())
	replySZX := min(reqSZX, maxSZX)
	size := int64(replySZX.Size())
	if off > n || (off == n && n > 0) {
		return 0, 0, 0, fmt.Errorf("%w: offset %d of a %d-byte blob", errBlock, off, n)
	}
	end = min(off+size, n)
	reply, err = blockwise.EncodeBlockOption(replySZX, off/size, end < n)
	if err != nil {
		return 0, 0, 0, err
	}
	return off, end, reply, nil
}

func isLowerHex64(s string) bool {
	if len(s) != 64 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !('0' <= c && c <= '9' || 'a' <= c && c <= 'f') {
			return false
		}
	}
	return true
}

// blob is GET /blob/<hash>: one Block2 block of the blob, sliced here because
// the library's blockwise layer is off. The whole blob is read per request
// (Store.Get); a range read is earned by a measured image size where it costs.
func (s *Server) blob(w mux.ResponseWriter, r *mux.Message) {
	if r.Code() != codes.GET {
		s.reply(w, codes.MethodNotAllowed, message.TextPlain, nil)
		return
	}
	name := r.RouteParams.Vars["hash"]
	if !isLowerHex64(name) {
		s.reply(w, codes.BadRequest, message.TextPlain, nil)
		return
	}
	h, _ := hex.DecodeString(name) // isLowerHex64 checked it
	body, err := s.blobs.Get(h)
	if errors.Is(err, fs.ErrNotExist) {
		s.reply(w, codes.NotFound, message.TextPlain, nil)
		return
	}
	if err != nil {
		s.logf("porta: coap: blob %s: %v", name, err)
		s.reply(w, codes.InternalServerError, message.TextPlain, nil)
		return
	}
	has := true
	v, err := r.GetOptionUint32(message.Block2)
	if errors.Is(err, message.ErrOptionNotFound) {
		has = false
	} else if err != nil {
		s.reply(w, codes.BadOption, message.TextPlain, nil)
		return
	}
	off, end, reply, err := window(int64(len(body)), v, has)
	if err != nil {
		s.reply(w, codes.BadOption, message.TextPlain, nil)
		return
	}
	buf := make([]byte, 4)
	n, err := message.EncodeUint32(buf, reply)
	if err != nil {
		s.reply(w, codes.InternalServerError, message.TextPlain, nil)
		return
	}
	// body[off:end] is non-nil even when empty, so an empty blob still answers
	// a 2.05 with Content-Format and Block2 set.
	s.reply(w, codes.Content, message.AppOctets, body[off:end], message.Option{ID: message.Block2, Value: buf[:n]})
}
