// Copyright (c) 2026 Ekorau LLC

package console

import (
	"context"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"
)

// Sink is a board's console over the mesh (log_udp.c): one datagram is one
// line, stamped on arrival like every other line in the store, so its stamps
// are evidence of order, never of the device's timing. It runs beside the
// gateway, whose address the node already reaches. "port=up" in its heartbeat
// means the socket is bound: a UDP listener has no peer to lose, and whether
// the node still sends is what the node's own lines say.
type Sink struct {
	Store     *Store
	Addr      string        // "[::]:54330"
	Heartbeat time.Duration // 0 = 60 s
	Lock      io.Closer     // the board's one-reader lock, kept alive with the sink
	Now       func() time.Time

	fanout   // rete#415: a profile through porta reads its dump here
	mu       sync.Mutex
	since    time.Time
	up       bool
	reason   string
	lastLine time.Time
}

func (s *Sink) now() time.Time {
	if s.Now != nil {
		return s.Now()
	}
	return time.Now()
}

// Health answers the sink's state: up while its socket is bound.
func (s *Sink) Health() Health {
	s.mu.Lock()
	defer s.mu.Unlock()
	h := Health{State: "down", Since: s.since, Reason: s.reason, LastLine: s.lastLine}
	if s.up {
		h.State, h.Reason = "up", ""
	}
	return h
}

func (s *Sink) set(up bool, reason string) {
	s.mu.Lock()
	s.up, s.reason, s.since = up, reason, s.now()
	s.mu.Unlock()
}

// Listen binds the socket; Run reads it until ctx ends.
func (s *Sink) Listen() (net.PacketConn, error) { return net.ListenPacket("udp", s.Addr) }

// Run collects datagrams from conn until ctx ends, then closes it.
func (s *Sink) Run(ctx context.Context, conn net.PacketConn) {
	s.set(true, "")
	s.Store.Note(fmt.Sprintf("udpsink started pid=%d port=%s", pid(), conn.LocalAddr()), s.now())
	every := s.Heartbeat
	if every == 0 {
		every = 60 * time.Second
	}
	go func() {
		<-ctx.Done()
		conn.Close()
	}()
	go func() {
		t := time.NewTicker(every)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				state := "down"
				if s.Health().State == "up" {
					state = "up"
				}
				s.Store.Note("heartbeat port="+state, s.now())
			}
		}
	}()
	peers := map[string]bool{}
	buf := make([]byte, 2048)
	for {
		n, addr, err := conn.ReadFrom(buf)
		if err != nil {
			if ctx.Err() == nil {
				s.set(false, err.Error())
				s.Store.Note("socket error: "+err.Error(), s.now())
			}
			break
		}
		host, _, _ := net.SplitHostPort(addr.String())
		if !peers[host] {
			peers[host] = true
			s.Store.Note("peer "+host, s.now())
		}
		text := strings.ToValidUTF8(strings.TrimRight(string(buf[:n]), "\r\n"), "�")
		if text == "" {
			continue
		}
		now := s.now()
		s.Store.Append(text, now)
		s.mu.Lock()
		s.lastLine = now
		s.mu.Unlock()
		s.publish(text)
	}
	s.set(false, "stopped")
	s.Store.Note("udpsink stopped", s.now())
}
