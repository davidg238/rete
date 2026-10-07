// Copyright (c) 2026 Ekorau LLC

package tui

// client.go — custos's HTTP surface as the TUI reads it (rete#341 D1): the
// endpoints the page and an agent use, typed with the server's own answers.

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/davidg238/rete/custos/internal/custos"
	"github.com/davidg238/rete/custos/internal/debug"
)

// Client talks to one serving custos.
type Client struct {
	Base string
	HTTP *http.Client
}

// NewClient's timeout outlasts the server's 25 s long-poll and a long capture.
func NewClient(base string) *Client {
	return &Client{strings.TrimRight(base, "/"), &http.Client{Timeout: 70 * time.Second}}
}

// StatusError is a non-2xx answer: 404 (no such target configured), 409 (one
// already in flight), 400 (not an .oimg), 502 (the capture's program ended
// early; the load's target refused or failed).
type StatusError struct {
	Code int
	Body string
}

func (e *StatusError) Error() string {
	return fmt.Sprintf("%d %s", e.Code, strings.TrimSpace(e.Body))
}

func (c *Client) call(method, path, body string, v any) error {
	req, err := http.NewRequest(method, c.Base+path, strings.NewReader(body))
	if err != nil {
		return err
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 != 2 {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		return &StatusError{resp.StatusCode, string(b)}
	}
	return json.NewDecoder(resp.Body).Decode(v)
}

func (c *Client) Source() (s custos.Source, err error) {
	err = c.call("GET", "/debug/source", "", &s)
	return s, err
}

func (c *Client) State(after int) (s debug.State, err error) {
	err = c.call("GET", fmt.Sprintf("/debug/state?after=%d", after), "", &s)
	return s, err
}

func (c *Client) Do(cmd string) (d custos.Done, err error) {
	err = c.call("POST", "/debug/do", cmd, &d)
	return d, err
}

func (c *Client) Capture(ms int) (p custos.Capture, err error) {
	err = c.call("POST", fmt.Sprintf("/profile/capture?ms=%d", ms), "", &p)
	return p, err
}

// Load uploads the .oimg at path to /load, named by its base name.
func (c *Client) Load(path string) (l custos.Load, err error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return l, err
	}
	err = c.call("POST", "/load?name="+url.QueryEscape(filepath.Base(path)), string(b), &l)
	return l, err
}

func (c *Client) LastLoad() (l custos.Load, err error) {
	err = c.call("GET", "/load/last", "", &l)
	return l, err
}

func (c *Client) Last() (p custos.Capture, err error) {
	err = c.call("GET", "/profile/last", "", &p)
	return p, err
}

// root and board split Base into the serving custos and the board it names:
// build and run are custos-wide verbs that take the board as ?for= (peer design D18).
func (c *Client) root() (root, board string) {
	if i := strings.Index(c.Base, "/board/"); i >= 0 {
		return c.Base[:i], c.Base[i+len("/board/"):]
	}
	return c.Base, ""
}

// Patch sends the edited .nsl at path to /debug/patch for method (peer design D17).
func (c *Client) Patch(path, method string) (d custos.Done, err error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return d, err
	}
	err = c.call("POST", "/debug/patch?method="+url.QueryEscape(method), string(b), &d)
	return d, err
}

// compile posts the .nsl at path to verb (build or run) for this client's board.
func (c *Client) compile(verb, path string, v any) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	root, board := c.root()
	q := "?name=" + url.QueryEscape(filepath.Base(path))
	if board != "" {
		q += "&for=" + url.QueryEscape(board)
	}
	return (&Client{root, c.HTTP}).call("POST", "/"+verb+q, string(b), v)
}

// Build compiles the .nsl at path for the board (POST /build).
func (c *Client) Build(path string) (b custos.Built, err error) {
	err = c.compile("build", path, &b)
	return b, err
}

// Run builds the .nsl at path for the host VM and runs it once (POST /run).
func (c *Client) Run(path string) (r custos.Ran, err error) {
	err = c.compile("run", path, &r)
	return r, err
}

// LoadLastBuild loads the last build's image onto the board: GET
// /build/last.oimg, then POST it to the board's /load, as the page does.
func (c *Client) LoadLastBuild(name string) (l custos.Load, err error) {
	root, _ := c.root()
	resp, err := c.HTTP.Get(root + "/build/last.oimg")
	if err != nil {
		return l, err
	}
	defer resp.Body.Close()
	img, err := io.ReadAll(resp.Body)
	if err != nil {
		return l, err
	}
	if resp.StatusCode/100 != 2 {
		return l, &StatusError{resp.StatusCode, string(img)}
	}
	err = c.call("POST", "/load?name="+url.QueryEscape(name), string(img), &l)
	return l, err
}
