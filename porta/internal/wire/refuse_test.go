// Copyright (c) 2026 Ekorau LLC

// internal/wire/refuse_test.go
package wire

import (
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
	"github.com/davidg238/porta/internal/space"
)

// decodeErrReply asserts the reply is an #err and answers its #msg. No
// DecodeErr exists in the package because production code never decodes its
// own refusals; the test does it longhand.
func decodeErrReply(t *testing.T, b []byte) string {
	t.Helper()
	v, err := cval.Decode(b)
	if err != nil {
		t.Fatalf("reply does not decode: %v", err)
	}
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "err" {
		t.Fatalf("reply is not an #err: %s", cval.Format(v))
	}
	for _, f := range r.Fields {
		if sym, ok := f.K.(cval.Symbol); ok && string(sym) == "msg" {
			if s, ok := f.V.(string); ok {
				return s
			}
		}
	}
	t.Fatalf("#err carries no #msg string: %s", cval.Format(v))
	return ""
}

// TestRefuseAll — rete#232's supported injector: with the knob on, every
// well-formed datagram is answered #err at the DOOR, before handle() —
// nothing is ingested and no node row appears, because #199's witnessed
// refusal was a reject-at-decode, not an ingest-then-nack. With the knob
// off (the zero value), the same frames are served normally.
func TestRefuseAll(t *testing.T) {
	srv, st := testServer(t)
	srv.SetRefuseAll(true)

	pull, err := EncodePull("n1", 0, 7, 0)
	if err != nil {
		t.Fatalf("EncodePull: %v", err)
	}
	push, err := EncodePush("n1", 7, []space.Entry{
		{Seq: 1, Path: "tel/log", Kind: cval.KindStream, Value: mustEnc(t, "hello")},
	})
	if err != nil {
		t.Fatalf("EncodePush: %v", err)
	}

	// (c) never silence: the refusal is a reply, not a dropped datagram — a
	// node that gets no reply cannot tell rejection from loss.
	for _, req := range [][]byte{pull, push} {
		resp := srv.Handle(req)
		if resp == nil {
			t.Fatalf("refusal answered nil — a refused node must still get a reply")
		}
		msg := decodeErrReply(t, resp)
		if !strings.Contains(msg, "TEST-REFUSE") {
			t.Fatalf("refusal does not name the injector: %q", msg)
		}
	}

	// (a) refused before handle: neither request reached the store, so no
	// node row exists at all.
	n, err := st.CountNodes()
	if err != nil {
		t.Fatalf("CountNodes: %v", err)
	}
	if n != 0 {
		t.Fatalf("a refused exchange reached the store: %d node row(s)", n)
	}

	// (b) knob off: the same frames are served normally.
	srv.SetRefuseAll(false)
	if _, err := DecodeAck(srv.Handle(push)); err != nil {
		t.Fatalf("push with the knob off was not acked: %v", err)
	}
	if _, _, err := DecodeBatch(srv.Handle(pull)); err != nil {
		t.Fatalf("pull with the knob off was not served a #batch: %v", err)
	}
}
