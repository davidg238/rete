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
	b, _, err := p.call("POST", strings.TrimRight(p.Base, "/")+"/blob", img, nil)
	if err != nil {
		return Push{}, err
	}
	var signed struct {
		Hash, Record string
		Size         int64
	}
	if err := json.Unmarshal(b, &signed); err != nil {
		return Push{}, fmt.Errorf("porta POST /blob: %w", err)
	}
	rec, err := hex.DecodeString(signed.Record)
	if err != nil {
		return Push{}, fmt.Errorf("porta POST /blob: Record: %w", err)
	}
	name := filepath.Base(path)
	b, _, err = p.call("PUT", p.cell("goal/apps/main")+"?reason="+url.QueryEscape("custos load "+name), rec,
		map[string]string{"Content-Type": "application/cbor", "X-Porta-Actor": "custos"})
	if err != nil {
		return Push{}, err
	}
	var w struct{ Seq int64 }
	if err := json.Unmarshal(b, &w); err != nil {
		return Push{}, fmt.Errorf("porta PUT goal/apps/main: %w", err)
	}
	out := Push{Image: name, Size: signed.Size, SHA256: signed.Hash, Seq: w.Seq}
	return p.Refresh(out)
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
