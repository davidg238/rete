// Copyright (c) 2026 Ekorau LLC

package load

// porta.go — a program pushed through porta to a node that is not on a cable
// (rete#398, design 2026-10-12-398). Two of porta's general routes do it, the
// same two steps as `porta blob put` + `porta put --raw` on the gateway: POST
// /blob stores and signs the image, and the App record it answers is written as
// the node's goal/apps/main. The push answers at the goal write and does not
// wait for the node, which may be asleep for half an hour (Q2): Running is
// re-read from obs/apps/main whenever it is asked, and Converged is the proof.

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/davidg238/rete/custos/internal/oimg"
	"github.com/davidg238/rete/protocol/cval"
)

// Porta is one node, reached through porta's operator HTTP listener at Base.
type Porta struct {
	Base   string
	Node   string
	Client *http.Client
}

// Push is what a push did, and what the node runs as last read. SHA256 is the
// image's hash as porta stored and signed it; Running is obs/apps/main's ("" when
// the node has published none); Converged is the two equal.
type Push struct {
	Image     string
	Size      int64
	SHA256    string
	Seq       int64
	Running   string
	Converged bool
}

func (p Porta) client() *http.Client {
	if p.Client != nil {
		return p.Client
	}
	return &http.Client{Timeout: 30 * time.Second}
}

func (p Porta) cell(path string) string {
	return strings.TrimRight(p.Base, "/") + "/node/" + url.PathEscape(p.Node) + "/" + path
}

// call sends one request to porta and answers its body, or porta's own words
// on a refusal.
func (p Porta) call(method, u string, body []byte, hdr map[string]string) ([]byte, int, error) {
	req, err := http.NewRequest(method, u, bytes.NewReader(body))
	if err != nil {
		return nil, 0, err
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := p.client().Do(req)
	if err != nil {
		return nil, 0, fmt.Errorf("porta: %w", err)
	}
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, 0, err
	}
	if resp.StatusCode/100 != 2 && resp.StatusCode != http.StatusNotFound {
		return nil, resp.StatusCode, fmt.Errorf("porta %s %s: %d %s", method, u, resp.StatusCode, strings.TrimSpace(string(b)))
	}
	return b, resp.StatusCode, nil
}

// Push stores and signs the image at porta, writes it as the node's goal, and
// answers with what the node runs now (before it has asked, the old program).
func (p Porta) Push(path string) (Push, error) {
	if _, err := oimg.Read(path); err != nil {
		return Push{}, err
	}
	img, err := os.ReadFile(path)
	if err != nil {
		return Push{}, err
	}
	signed, err := p.PutBlob(img)
	if err != nil {
		return Push{}, err
	}
	name := filepath.Base(path)
	seq, err := p.put("goal/apps/main", signed.Record, "custos load "+name)
	if err != nil {
		return Push{}, err
	}
	out := Push{Image: name, Size: signed.Size, SHA256: signed.Hash, Seq: seq}
	return p.Refresh(out)
}

// Stored is what porta answers for a stored blob: its hash (hex), its size, and
// the goal/apps/<name> App record that names it, signed (cval).
type Stored struct {
	Hash   string
	Size   int64
	Record []byte
}

// PutBlob stores b at porta, which signs it.
func (p Porta) PutBlob(b []byte) (Stored, error) {
	body, _, err := p.call("POST", strings.TrimRight(p.Base, "/")+"/blob", b, nil)
	if err != nil {
		return Stored{}, err
	}
	var signed struct {
		Hash, Record string
		Size         int64
	}
	if err := json.Unmarshal(body, &signed); err != nil {
		return Stored{}, fmt.Errorf("porta POST /blob: %w", err)
	}
	rec, err := hex.DecodeString(signed.Record)
	if err != nil {
		return Stored{}, fmt.Errorf("porta POST /blob: Record: %w", err)
	}
	return Stored{signed.Hash, signed.Size, rec}, nil
}

// Blob is a stored blob's bytes, named by its hash (hex).
func (p Porta) Blob(hash string) ([]byte, error) {
	u := strings.TrimRight(p.Base, "/") + "/blob/" + url.PathEscape(hash)
	b, code, err := p.call("GET", u, nil, nil)
	if err == nil && code == http.StatusNotFound {
		err = fmt.Errorf("porta holds no blob %.12s", hash)
	}
	return b, err
}

// Read is the node's cell at path, decoded; nil when porta holds none.
func (p Porta) Read(path string) (any, error) {
	b, code, err := p.call("GET", p.cell(path), nil, nil)
	if err != nil || code == http.StatusNotFound {
		return nil, err
	}
	v, err := cval.Decode(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return v, nil
}

// Row is one row of the node's event log: porta's row id (the cursor) and the
// value, decoded.
type Row struct {
	ID    int64
	Value any
}

// Log is the node's rows on path with a row id past after, oldest first: at
// most porta's page, so a reader asks again from the last ID.
func (p Porta) Log(path string, after int64) ([]Row, error) {
	u := p.cell("log") + "?path=" + url.QueryEscape(path) + "&after=" + fmt.Sprint(after)
	b, _, err := p.call("GET", u, nil, nil)
	if err != nil {
		return nil, err
	}
	var rows []struct {
		ID    int64
		Value string
	}
	if err := json.Unmarshal(b, &rows); err != nil {
		return nil, fmt.Errorf("porta GET log: %w", err)
	}
	out := make([]Row, 0, len(rows))
	for _, r := range rows {
		enc, err := hex.DecodeString(r.Value)
		if err != nil {
			return nil, fmt.Errorf("porta GET log: row %d: %w", r.ID, err)
		}
		v, err := cval.Decode(enc)
		if err != nil {
			return nil, fmt.Errorf("porta GET log: row %d: %w", r.ID, err)
		}
		out = append(out, Row{r.ID, v})
	}
	return out, nil
}

// Write encodes v as one closed cval value and writes it at path on the node:
// a goal/ cell (rete#415: goal/console and goal/profile), or a dbg/req line.
// It answers porta's seq for the write.
func (p Porta) Write(path string, v any, reason string) (int64, error) {
	enc, err := cval.Encode(v)
	if err != nil {
		return 0, err
	}
	return p.put(path, enc, reason)
}

func (p Porta) put(path string, enc []byte, reason string) (int64, error) {
	b, _, err := p.call("PUT", p.cell(path)+"?reason="+url.QueryEscape(reason), enc,
		map[string]string{"Content-Type": "application/cbor", "X-Porta-Actor": "custos"})
	if err != nil {
		return 0, err
	}
	var w struct{ Seq int64 }
	if err := json.Unmarshal(b, &w); err != nil {
		return 0, fmt.Errorf("porta PUT %s: %w", path, err)
	}
	return w.Seq, nil
}

// Refresh re-reads what the node runs.
func (p Porta) Refresh(push Push) (Push, error) {
	run, err := p.Running()
	if err != nil {
		return push, err
	}
	push.Running = run
	push.Converged = run != "" && run == push.SHA256
	return push, nil
}

// Running is the image hash in the node's obs/apps/main, "" before it has
// published one.
func (p Porta) Running() (string, error) {
	b, code, err := p.call("GET", p.cell("obs/apps/main"), nil, nil)
	if err != nil || code == http.StatusNotFound {
		return "", err
	}
	return imageHash(b)
}

// imageHash digs AppObs{image: BlobRef{hash}} out of one cval encoding.
func imageHash(enc []byte) (string, error) {
	v, err := cval.Decode(enc)
	if err != nil {
		return "", fmt.Errorf("obs/apps/main: %w", err)
	}
	field := func(v any, k string) any {
		if r, ok := v.(cval.Record); ok {
			for _, p := range r.Fields {
				if p.K == cval.Symbol(k) {
					return p.V
				}
			}
		}
		return nil
	}
	h, ok := field(field(v, "image"), "hash").(cval.Bytes)
	if !ok {
		return "", fmt.Errorf("obs/apps/main is not AppObs{image: BlobRef{hash}}: %s", cval.Format(v))
	}
	return hex.EncodeToString(h), nil
}
