// Copyright (c) 2026 Ekorau LLC

package tui

// client.go — custos's HTTP surface as the TUI reads it (rete#341 D1): the
// endpoints the page and an agent use, typed with the server's own answers.

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
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
// already in flight), 502 (the capture's program ended early).
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

func (c *Client) Last() (p custos.Capture, err error) {
	err = c.call("GET", "/profile/last", "", &p)
	return p, err
}
