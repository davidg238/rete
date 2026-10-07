// Copyright (c) 2026 Ekorau LLC

package console

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"strings"
	"sync"
	"time"
)

// Cable is one board's console, read all the time (peer design D16): every line
// goes to the store and to whoever is subscribed (a debug session, a capture),
// and a verb's lines go down it. A load lends it: the port is closed while the
// loader holds it, then opened again. A cable that fails is retried every
// second, and the store is told once per change ("port lost", "port
// reacquired"); a heartbeat says whether it is up, whether or not the board says
// anything, because a log that stops reads the same as a board gone quiet.
type Cable struct {
	Store     *Store
	Open      func() (io.ReadWriteCloser, error)
	Heartbeat time.Duration // 0 = 60 s
	Port      string        // named in the start marker
	// Lock is the board's one-reader lock (Lock), kept here so that it lives as
	// long as the cable: an unreferenced *os.File is closed by its finalizer,
	// which would release the flock silently.
	Lock io.Closer
	Now       func() time.Time

	mu    sync.Mutex
	port  io.ReadWriteCloser
	up    bool
	lent  chan struct{} // non-nil while lent: closed when it comes back
	subs  map[int]func(string)
	next  int
}

func (c *Cable) now() time.Time {
	if c.Now != nil {
		return c.Now()
	}
	return time.Now()
}

func (c *Cable) note(text string) { c.Store.Note(text, c.now()) }

// Up reports whether the port is open.
func (c *Cable) Up() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.up
}

// Subscribe hands every line read from now on to fn; the answer stops it.
func (c *Cable) Subscribe(fn func(string)) (cancel func()) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.subs == nil {
		c.subs = map[int]func(string){}
	}
	id := c.next
	c.next++
	c.subs[id] = fn
	return func() {
		c.mu.Lock()
		delete(c.subs, id)
		c.mu.Unlock()
	}
}

// Send writes one line down the cable.
func (c *Cable) Send(line string) error {
	c.mu.Lock()
	p, up := c.port, c.up
	c.mu.Unlock()
	if !up {
		return errors.New("the board's console cable is not open")
	}
	_, err := io.WriteString(p, line+"\n")
	return err
}

// Lend closes the port, runs fn (which may open it itself: esptool, a reset),
// and opens it again; the store is told both ways.
func (c *Cable) Lend(fn func() error) error {
	c.mu.Lock()
	back := make(chan struct{})
	c.lent = back
	p := c.port
	c.port, c.up = nil, false
	c.mu.Unlock()
	if p != nil {
		p.Close()
	}
	c.note("cable lent")
	err := fn()
	c.mu.Lock()
	c.lent = nil
	c.mu.Unlock()
	close(back)
	c.note("cable back")
	return err
}

// Run reads the cable until ctx ends, then writes "collector stopped".
func (c *Cable) Run(ctx context.Context) {
	c.note(fmt.Sprintf("collector started pid=%d port=%s", pid(), c.Port))
	every := c.Heartbeat
	if every == 0 {
		every = 60 * time.Second
	}
	go func() {
		t := time.NewTicker(every)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				state := "down"
				if c.Up() {
					state = "up"
				}
				c.note("heartbeat port=" + state)
			}
		}
	}()
	go func() {
		<-ctx.Done()
		c.mu.Lock()
		p := c.port
		c.port, c.up = nil, false
		c.mu.Unlock()
		if p != nil {
			p.Close()
		}
	}()
	lost := false
	for ctx.Err() == nil {
		c.mu.Lock()
		back := c.lent
		c.mu.Unlock()
		if back != nil {
			select {
			case <-back:
			case <-ctx.Done():
			}
			continue
		}
		p, err := c.Open()
		if err != nil {
			if !lost {
				c.note("port lost: " + err.Error())
				lost = true
			}
			select {
			case <-time.After(time.Second):
			case <-ctx.Done():
			}
			continue
		}
		c.mu.Lock()
		if c.lent != nil || ctx.Err() != nil { // lent (or stopped) while opening
			c.mu.Unlock()
			p.Close()
			continue
		}
		c.port, c.up = p, true
		c.mu.Unlock()
		if lost {
			c.note("port reacquired")
			lost = false
		}
		err = c.read(p)
		c.mu.Lock()
		closedUnder := c.port != p // Lend or stop closed it: not a loss
		if !closedUnder {
			c.port, c.up = nil, false
		}
		c.mu.Unlock()
		if !closedUnder {
			p.Close()
			if ctx.Err() == nil {
				c.note("port lost: " + err.Error())
				lost = true
			}
		}
	}
	c.note("collector stopped")
}

// read splits what the port answers into lines until a read fails. A serial
// read timeout is an empty read, not the end.
func (c *Cable) read(p io.Reader) error {
	var buf []byte
	chunk := make([]byte, 4096)
	for {
		n, err := p.Read(chunk)
		buf = append(buf, chunk[:n]...)
		for {
			i := bytes.IndexByte(buf, '\n')
			if i < 0 {
				break
			}
			c.line(buf[:i])
			buf = buf[i+1:]
		}
		if err != nil {
			if len(buf) > 0 {
				c.line(buf)
			}
			return err
		}
	}
}

// line is one line as the store keeps it: NULs dropped, a trailing \r
// stripped, invalid UTF-8 replaced.
func (c *Cable) line(b []byte) {
	text := strings.ToValidUTF8(strings.TrimSuffix(strings.ReplaceAll(string(b), "\x00", ""), "\r"), "�")
	c.Store.Append(text, c.now())
	c.mu.Lock()
	subs := make([]func(string), 0, len(c.subs))
	for _, fn := range c.subs {
		subs = append(subs, fn)
	}
	c.mu.Unlock()
	for _, fn := range subs {
		fn(text)
	}
}
