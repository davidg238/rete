// Copyright (c) 2026 Ekorau LLC

package portacli

// ops.go — porta's operator write routes (rete#398): the HTTP twins of
// `porta blob put`, `porta put --raw` and `porta get`, so a peer off the
// gateway (custos) can push a program and read the proof. General on purpose:
// the same PUT writes goal/apps/main, goal/debug/main and goal/console, and the
// dbg/req lines (rete#415: `profile <ms>`), and porta never learns what an app
// or an .oimg is. Admission is the listener's CIDR
// allowlist and nothing else (design 2026-10-12 Q3: a known gap for 0.1).
//
//	POST /blob                   the bytes; answers {Hash, Size, Sig, Record} (hex)
//	PUT  /node/{id}/{path...}    one closed cval encoding to a goal/ cell, or a dbg/req row; answers {Seq}
//	GET  /node/{id}/{path...}    the cell's value as raw CBOR, its seq in X-Porta-Seq

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/davidg238/porta/internal/space"
	"github.com/davidg238/rete/protocol/cval"
)

// maxBlob bounds an upload: well over a 768K program slot.
const maxBlob = 4 << 20

func mountOps(mux *http.ServeMux, st *space.Store, db string) {
	mux.HandleFunc("POST /blob", func(w http.ResponseWriter, r *http.Request) {
		body := http.MaxBytesReader(w, r.Body, maxBlob)
		s, err := storeAndSign(db, body)
		if err != nil {
			http.Error(w, "porta: "+err.Error(), http.StatusBadRequest)
			return
		}
		answerJSON(w, map[string]any{
			"Hash":   hex.EncodeToString(s.Hash),
			"Size":   s.Size,
			"Sig":    hex.EncodeToString(s.Sig),
			"Record": hex.EncodeToString(s.Record),
		})
	})
	mux.HandleFunc("PUT /node/{id}/{path...}", func(w http.ResponseWriter, r *http.Request) {
		path := r.PathValue("path")
		// porta's inbound authority, exactly (PROTOCOL-NSL §3): goal/** as cells,
		// and dbg/req, the one stream porta writes. Nothing else is intent.
		kind := cval.KindCell
		switch {
		case path == "dbg/req":
			kind = cval.KindStream
		case !strings.HasPrefix(path, "goal/"):
			http.Error(w, fmt.Sprintf("porta: %q is neither under goal/ nor dbg/req; the HTTP write is intent only", path), http.StatusBadRequest)
			return
		}
		value, err := io.ReadAll(http.MaxBytesReader(w, r.Body, space.MaxValueBytes))
		if err != nil {
			http.Error(w, "porta: "+err.Error(), http.StatusRequestEntityTooLarge)
			return
		}
		actor := r.Header.Get("X-Porta-Actor")
		if actor == "" {
			actor = "http:" + r.RemoteAddr
		}
		seq, err := st.Write(r.PathValue("id"), path, kind, value, actor, r.URL.Query().Get("reason"))
		if err != nil {
			http.Error(w, "porta: "+err.Error(), http.StatusBadRequest)
			return
		}
		answerJSON(w, map[string]any{"Seq": seq})
	})
	mux.HandleFunc("GET /node/{id}/{path...}", func(w http.ResponseWriter, r *http.Request) {
		path := r.PathValue("path")
		rows, err := st.Tree(r.PathValue("id"), path)
		if err != nil {
			http.Error(w, "porta: "+err.Error(), http.StatusInternalServerError)
			return
		}
		for _, c := range rows {
			if c.Path == path {
				w.Header().Set("Content-Type", "application/cbor")
				w.Header().Set("X-Porta-Seq", strconv.FormatInt(c.Seq, 10))
				w.Write(c.Value)
				return
			}
		}
		http.NotFound(w, r)
	})
}

func answerJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	json.NewEncoder(w).Encode(v)
}
