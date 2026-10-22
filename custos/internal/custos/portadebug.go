// Copyright (c) 2026 Ekorau LLC

package custos

// portadebug.go — the debug verbs on a porta node (rete#432 Q5, design
// 2026-10-15-432). One rule: what must survive a sleep is in the node's
// goal/debug/main cell, Debug{sha, breaks, patches}; everything else is a
// dbg/req line. A sleepy node hears either at its next check-in, so every verb
// that reaches the node answers Pending at the write (P9), and the node's
// answers come back from dbg/resp (exactly once through porta, P10), which a
// poller feeds to the session: /debug/state shows them, a stop and a bt in
// source terms. custos keeps no state of its own (P6): the breakpoints and the
// patch set are read back from the cell and porta's blob store at every edit.
//
//	GET    /debug, /debug/source, /debug/state   as on a cable (debug.go)
//	POST   /debug/do                one command: break/clear rewrite the cell's breaks;
//	                                where, list, methods, help answer here; any other
//	                                is one dbg/req line (debug.Session.Post)
//	POST   /debug/patch?method=V>>sel  the whole edited .nsl: the method compiled against
//	                                the map, its line put into the patch set (replacing
//	                                that method's), the set stored as a blob, the cell
//	                                rewritten. The node applies the new line at its next
//	                                check-in and replays the set at every boot.
//	DELETE /debug/patch?method=V>>sel  the method's line taken out: the node reboots into
//	                                the smaller set (a patch cannot be removed in place, Q2)

import (
	"context"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/davidg238/rete/custos/internal/debug"
	"github.com/davidg238/rete/custos/internal/load"
	"github.com/davidg238/rete/custos/internal/nsl"
	"github.com/davidg238/rete/protocol/cval"
)

// debugCellPath is the app's Debug cell: the node runs one app, main.
const debugCellPath = "goal/debug/main"

// patchSetMax is the methods one set may patch: METHOD_OVERRIDES_MAX (oevm.h),
// which binds before the VM's 64-entry patch table (P2).
const patchSetMax = 16

// compilePatch is nsl.Patch (a var so a test needs no compiler).
var compilePatch = nsl.Patch

// respEvery is how often the poller reads dbg/resp (a var so a test can
// shorten it).
var respEvery = time.Second

// debugCell is goal/debug/main's value (MANIFEST.cddl `debug`).
type debugCell struct {
	sha     []byte
	breaks  [][2]int64 // (blob offset, pc)
	patches []byte     // the set's BlobRef hash; nil = no patches
	size    int64
}

func (c debugCell) record() cval.Record {
	bs := cval.List{}
	for _, b := range c.breaks {
		bs = append(bs, cval.List{b[0], b[1]})
	}
	f := cval.Map{
		{K: cval.Symbol("sha"), V: cval.Bytes(c.sha)},
		{K: cval.Symbol("breaks"), V: bs},
	}
	if c.patches != nil {
		f = append(f, cval.Pair{K: cval.Symbol("patches"), V: cval.Record{Tag: "BlobRef", Fields: cval.Map{
			{K: cval.Symbol("hash"), V: cval.Bytes(c.patches)},
			{K: cval.Symbol("size"), V: c.size},
		}}})
	}
	return cval.Record{Tag: "Debug", Fields: f}
}

func field(r cval.Record, k string) any {
	for _, p := range r.Fields {
		if p.K == cval.Symbol(k) {
			return p.V
		}
	}
	return nil
}

// parseDebugCell reads a Debug record, or says what it is instead.
func parseDebugCell(v any) (debugCell, error) {
	bad := fmt.Errorf("%s is not Debug{sha, breaks, patches?}: %s", debugCellPath, cval.Format(v))
	r, ok := v.(cval.Record)
	if !ok || r.Tag != "Debug" {
		return debugCell{}, bad
	}
	var c debugCell
	sha, ok := field(r, "sha").(cval.Bytes)
	bs, ok2 := field(r, "breaks").(cval.List)
	if !ok || !ok2 {
		return debugCell{}, bad
	}
	c.sha = sha
	for _, b := range bs {
		l, ok := b.(cval.List)
		if !ok || len(l) != 2 {
			return debugCell{}, bad
		}
		off, ok1 := l[0].(int64)
		pc, ok2 := l[1].(int64)
		if !ok1 || !ok2 {
			return debugCell{}, bad
		}
		c.breaks = append(c.breaks, [2]int64{off, pc})
	}
	if p := field(r, "patches"); p != nil {
		ref, ok := p.(cval.Record)
		h, ok1 := field(ref, "hash").(cval.Bytes)
		if !ok || ref.Tag != "BlobRef" || !ok1 {
			return debugCell{}, bad
		}
		c.patches = h
		c.size, _ = field(ref, "size").(int64)
	}
	return c, nil
}

// portaDebug is one porta node's debug verbs: the session, the node through
// porta, and the program the session's map describes.
type portaDebug struct {
	p load.Porta
	s *debug.Session
}

// cell is the node's Debug cell for the map's program: a cell naming another
// image (or none) reads as the empty set, since the node refuses its breaks and
// patches anyway.
func (d portaDebug) cell() (debugCell, error) {
	sha, err := hex.DecodeString(d.s.SourceMap().SHA256())
	if err != nil {
		return debugCell{}, err
	}
	v, err := d.p.Read(debugCellPath)
	if err != nil || v == nil {
		return debugCell{sha: sha}, err
	}
	c, err := parseDebugCell(v)
	if err != nil {
		return debugCell{}, err
	}
	if !strings.EqualFold(hex.EncodeToString(c.sha), hex.EncodeToString(sha)) {
		return debugCell{sha: sha}, nil
	}
	return c, nil
}

// write rewrites the cell and shows its breakpoints in State.
func (d portaDebug) write(c debugCell, reason string) (int64, error) {
	seq, err := d.p.Write(debugCellPath, c.record(), reason)
	if err != nil {
		return 0, err
	}
	d.showBreaks(c)
	return seq, nil
}

func (d portaDebug) showBreaks(c debugCell) {
	sm := d.s.SourceMap()
	var lines []int
	for _, b := range c.breaks {
		if _, line, _, ok := sm.Place(int(b[0]), int(b[1])); ok && line > 0 {
			lines = append(lines, line)
		}
	}
	d.s.SetBreaks(lines)
}

// set is the cell's patch set, one VM patch text per line.
func (d portaDebug) set(c debugCell) ([]string, error) {
	if c.patches == nil {
		return nil, nil
	}
	b, err := d.p.Blob(hex.EncodeToString(c.patches))
	if err != nil {
		return nil, err
	}
	var lines []string
	for _, l := range strings.Split(string(b), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			lines = append(lines, l)
		}
	}
	return lines, nil
}

// putSet stores lines as the cell's patch set (none when empty).
func (d portaDebug) putSet(c *debugCell, lines []string) error {
	c.patches, c.size = nil, 0
	if len(lines) == 0 {
		return nil
	}
	st, err := d.p.PutBlob([]byte(strings.Join(lines, "\n") + "\n"))
	if err != nil {
		return err
	}
	h, err := hex.DecodeString(st.Hash)
	if err != nil {
		return fmt.Errorf("porta POST /blob: Hash: %w", err)
	}
	c.patches, c.size = h, st.Size
	return nil
}

// patchKey is the method a patch text replaces: `oevm-patch 1 vtable <v>
// selector <s>`, the header patch_method.py writes.
func patchKey(text string) string {
	f := strings.Fields(text)
	if len(f) < 6 {
		return text
	}
	return strings.Join(f[:6], " ")
}

// portaDebugLine is the session's line on a porta node: each command is a dbg/req
// row, heard at the node's next check-in.
type portaDebugLine struct{ p load.Porta }

func (l portaDebugLine) Send(line string) error {
	_, err := l.p.Write("dbg/req", line, "custos debug")
	return err
}

// pollResp feeds the node's dbg/resp rows to the session until ctx ends,
// starting after the last row porta holds now: what the node said before this
// custos started belongs to no command of its.
func pollResp(ctx context.Context, p load.Porta, s *debug.Session) {
	var cursor int64
	skip := true
	failing := false
	for {
		rows, err := p.Log("dbg/resp", cursor)
		switch {
		case err != nil:
			if !failing {
				s.Note("dbg/resp not read from porta: " + err.Error())
			}
			failing = true
		default:
			failing = false
			for _, r := range rows {
				cursor = r.ID
				if line, ok := r.Value.(string); ok && !skip {
					s.OnLine(line)
				}
			}
			if skip && len(rows) > 0 {
				continue // page on to the end before feeding anything
			}
			skip = false
			if len(rows) > 0 {
				continue
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(respEvery):
		}
	}
}

// mountPortaDebug serves the debug verbs on a porta node whose program the map
// at b.Map describes, and answers what a porta load tells it (follow).
func mountPortaDebug(ctx context.Context, mux *http.ServeMux, prefixes []string, b Board, p load.Porta, busy *sync.Mutex) (func(sha string) string, error) {
	sm, err := debug.LoadSourceMap(b.Map, b.Source)
	if err != nil {
		return nil, err
	}
	s := debug.NewSession(sm, io.Discard)
	line := portaDebugLine{p}
	s.Attach(line)
	d := portaDebug{p, s}
	if c, err := d.cell(); err == nil {
		d.showBreaks(c)
	}
	go pollResp(ctx, p, s)
	mountDebugView(mux, prefixes, s)
	refuse := func(w http.ResponseWriter, err error) {
		answerOK(w, Done{Error: err.Error(), State: s.State(-1, 0)})
	}
	pending := func(w http.ResponseWriter, seq int64) {
		answerOK(w, Done{OK: true, Pending: true, Seq: seq, State: s.State(-1, 0)})
	}
	locked := func(w http.ResponseWriter) bool {
		if !busy.TryLock() {
			http.Error(w, "custos: a command or a push to this node is being written", http.StatusConflict)
			return false
		}
		return true
	}

	handle(mux, prefixes, "POST /debug/do", func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(io.LimitReader(r.Body, 4096))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		if !locked(w) {
			return
		}
		defer busy.Unlock()
		cmd := strings.TrimSpace(string(body))
		word, arg, _ := strings.Cut(cmd, " ")
		switch word {
		case "", "h", "help", "w", "where", "l", "list", "methods", "q", "quit":
			res := s.Do(cmd)
			answerOK(w, Done{OK: res.Err == "", Error: res.Err, State: s.State(-1, 0)})
		case "b", "break", "clear":
			bp, err := s.SourceMap().Resolve(strings.TrimSpace(arg))
			if err != nil {
				refuse(w, err)
				return
			}
			c, err := d.cell()
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadGateway)
				return
			}
			at := [2]int64{int64(bp.Off), int64(bp.PC)}
			kept := [][2]int64{}
			for _, b := range c.breaks {
				if b != at {
					kept = append(kept, b)
				}
			}
			if word != "clear" {
				kept = append(kept, at)
			}
			c.breaks = kept
			seq, err := d.write(c, "custos debug "+word)
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadGateway)
				return
			}
			pending(w, seq)
		default:
			l, err := s.Post(cmd)
			if err != nil {
				refuse(w, err)
				return
			}
			seq, err := p.Write("dbg/req", l, "custos debug")
			if err != nil {
				http.Error(w, err.Error(), http.StatusBadGateway)
				return
			}
			pending(w, seq)
		}
	})

	// edit puts method's new line in the set (text != "") or takes it out (text
	// == "", the method named by its unedited source's header), and answers.
	edit := func(w http.ResponseWriter, method, text string) {
		key := patchKey(text)
		if text == "" {
			sm := s.SourceMap()
			src, err := os.ReadFile(sm.SourcePath)
			if err == nil {
				text, err = compilePatch(sm.MapPath, sm.SourcePath, src, method, true)
			}
			if err != nil {
				refuse(w, err)
				return
			}
			key, text = patchKey(text), ""
		}
		c, err := d.cell()
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		lines, err := d.set(c)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		var out []string
		for _, l := range lines {
			if patchKey(l) != key {
				out = append(out, l)
			}
		}
		if text != "" {
			out = append(out, text)
		} else if len(out) == len(lines) {
			refuse(w, fmt.Errorf("the patch set holds no patch of %s", method))
			return
		}
		if len(out) > patchSetMax {
			refuse(w, fmt.Errorf("a patch set holds %d methods (METHOD_OVERRIDES_MAX), and this would be %d", patchSetMax, len(out)))
			return
		}
		seq, err := int64(0), d.putSet(&c, out)
		if err == nil {
			seq, err = d.write(c, "custos debug patch "+method)
		}
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		s.Note(fmt.Sprintf("patch set of %d for sha256 %.12s: the node takes it at its next check-in", len(out), hex.EncodeToString(c.sha)))
		pending(w, seq)
	}
	handle(mux, prefixes, "POST /debug/patch", func(w http.ResponseWriter, r *http.Request) {
		method, edited, ok := patchRequest(w, r)
		if !ok || !locked(w) {
			return
		}
		defer busy.Unlock()
		sm := s.SourceMap()
		text, err := compilePatch(sm.MapPath, sm.SourcePath, edited, method, true)
		if err != nil {
			refuse(w, err) // a refusal is the answer: nothing was written
			return
		}
		edit(w, method, strings.TrimSpace(text))
	})
	handle(mux, prefixes, "DELETE /debug/patch", func(w http.ResponseWriter, r *http.Request) {
		method := r.URL.Query().Get("method")
		if method == "" {
			http.Error(w, "custos: name the method, ?method=<Vtable>>selector", http.StatusBadRequest)
			return
		}
		if !locked(w) {
			return
		}
		defer busy.Unlock()
		edit(w, method, "")
	})

	// A load is a new image: the old breaks and patches name the old sha. The
	// cell is cleared to the new one, then the session follows the map.
	reread := func() (*debug.SourceMap, error) { return debug.LoadSourceMap(b.Map, b.Source) }
	return func(sha string) string {
		h, err := hex.DecodeString(sha)
		if err == nil {
			_, err = d.write(debugCell{sha: h}, "custos load")
		}
		if err != nil {
			return "the debug cell was not cleared: " + err.Error()
		}
		return follow(s, line, reread)(sha)
	}, nil
}
