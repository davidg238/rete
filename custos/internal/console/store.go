// Copyright (c) 2026 Ekorau LLC

// Package console is the console collector's store (peer design D16, rete#314):
// one board's console, stamped and appended to a file per local day, and
// nothing parsed. It writes the format the Python collector wrote (deleted with
// it), so the readers that read the files directly — the target harness,
// bench-identity.sh — read custos's files unchanged:
//
//	<root>/console-<board>-<YYYY-MM-DD>.log      the local date
//	2026-08-03T20:19:38.087-07:00 <text>         stamped on arrival, ms, the local offset
//	… @@rete-console@@ heartbeat port=up         the collector's own lines
//	<root>/<board>.lock                          flock, "pid=N": one reader per board
package console

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Marker starts every line the collector writes about itself.
const Marker = "@@rete-console@@"

// DefaultRoot and DefaultBudget are the collector's defaults: 2 GiB of whole
// days, oldest first.
var DefaultRoot = filepath.Join(os.Getenv("HOME"), ".local", "state", "rete", "console")

const DefaultBudget = 2 << 30

// Store is one board's day files.
type Store struct {
	Root   string
	Board  string
	Budget int64

	mu sync.Mutex
}

// Path is day's file.
func (s *Store) Path(day time.Time) string {
	return filepath.Join(s.Root, fmt.Sprintf("console-%s-%s.log", s.Board, day.Format("2006-01-02")))
}

// Stamp is a line as the store writes it.
func Stamp(text string, when time.Time) string {
	return when.Format("2006-01-02T15:04:05.000-07:00") + " " + text + "\n"
}

// Append writes text stamped with when (local time) to when's day file.
func (s *Store) Append(text string, when time.Time) error {
	when = when.Local()
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := os.MkdirAll(s.Root, 0o755); err != nil {
		return err
	}
	f, err := os.OpenFile(s.Path(when), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = f.WriteString(Stamp(text, when))
	return err
}

// Note appends one of the collector's own lines.
func (s *Store) Note(text string, when time.Time) error { return s.Append(Marker+" "+text, when) }

// Days answers the dates the board has files for, oldest first; a file whose
// name does not parse is not the store's and is left alone.
func (s *Store) Days() ([]time.Time, error) {
	names, err := os.ReadDir(s.Root)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	prefix := "console-" + s.Board + "-"
	var out []time.Time
	for _, n := range names {
		name := n.Name()
		if !strings.HasPrefix(name, prefix) || !strings.HasSuffix(name, ".log") {
			continue
		}
		d, err := time.ParseInLocation("2006-01-02", strings.TrimSuffix(strings.TrimPrefix(name, prefix), ".log"), time.Local)
		if err != nil {
			continue
		}
		out = append(out, d)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Before(out[j]) })
	return out, nil
}

// Prune deletes whole oldest days until the board is under its budget, and
// answers what it deleted. Never the newest day: one wedged day over budget is a
// fact to report, not a reason to destroy the only evidence.
func (s *Store) Prune() ([]string, error) {
	days, err := s.Days()
	if err != nil {
		return nil, err
	}
	size := func() (n int64) {
		for _, d := range days {
			if fi, err := os.Stat(s.Path(d)); err == nil {
				n += fi.Size()
			}
		}
		return n
	}
	var deleted []string
	for len(days) > 1 && size() > s.Budget {
		victim := s.Path(days[0])
		if err := os.Remove(victim); err != nil {
			return deleted, err
		}
		deleted = append(deleted, victim)
		days = days[1:]
	}
	return deleted, nil
}

// Lock takes the board's one-reader lock, or answers who holds it. Two readers
// of one console split its bytes between them. The lock is held until the
// process exits or the answered file is closed.
func Lock(root, board string) (*os.File, error) {
	if err := os.MkdirAll(root, 0o755); err != nil {
		return nil, err
	}
	path := filepath.Join(root, board+".lock")
	f, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0o644)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		holder, _ := os.ReadFile(path)
		f.Close()
		return nil, fmt.Errorf("board %s's console is held (%s): stop that collector first (systemctl --user stop rete-console@%s)",
			board, strings.TrimSpace(string(holder)), board)
	}
	f.Truncate(0)
	f.WriteAt([]byte("pid="+strconv.Itoa(os.Getpid())+"\n"), 0)
	return f, nil
}

func pid() int { return os.Getpid() }
