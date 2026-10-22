// Copyright (c) 2026 Ekorau LLC

package debug

// post.go — the session over a line that answers later (rete#432 P9): a porta
// node hears a dbg/req line at its next check-in, which may be half an hour
// away, so a command cannot wait for its answer. Post turns a command into the
// one line it sends and answers at once; the VM's answer arrives through OnLine,
// which translates a stop and a bt as it does on a cable. A line step is one
// bytecode here (dbg:step, dbg:over): a cable's step repeats until the line
// changes, which would be a check-in per bytecode.

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// Post answers the dbg: line cmd sends, or why it sends none. break and clear
// are not lines on such a node (the caller writes the breakpoint cell), and
// the session's local verbs (where, list, methods, help) are Do's.
func (s *Session) Post(cmd string) (string, error) {
	word, arg, _ := strings.Cut(strings.TrimSpace(cmd), " ")
	arg = strings.TrimSpace(arg)
	var line string
	switch {
	case strings.HasPrefix(word, "dbg:"):
		line = strings.TrimSpace(cmd)
	case word == "bt" || word == "backtrace":
		line = "dbg:backtrace"
	case word == "locals":
		n := 0
		if arg != "" {
			var err error
			if n, err = strconv.Atoi(arg); err != nil || n < 0 {
				return "", fmt.Errorf("locals takes a frame number from bt, got %q", arg)
			}
		}
		line = fmt.Sprintf("dbg:inspect %d", n)
	case words[word] != "":
		line = words[word]
	default:
		return "", fmt.Errorf("%s is not sent as a line here (type help)", word)
	}
	switch strings.Fields(line)[0] {
	case "dbg:continue", "dbg:run", "dbg:step", "dbg:over", "dbg:out":
		s.setStopped(nil) // the next stop says where, when the node next checks in
	}
	s.say("-- posted %s: the node answers at its next check-in", line)
	return line, nil
}

// SetBreaks makes lines the breakpoints State shows: on a porta node, the
// set the cell holds, which the node re-arms at every check-in.
func (s *Session) SetBreaks(lines []int) {
	sort.Ints(lines)
	s.mu.Lock()
	s.breaks = map[int]bool{}
	for _, n := range lines {
		s.breaks[n] = true
	}
	s.notify()
	s.mu.Unlock()
}

// Note says text in the transcript, as the session's own words.
func (s *Session) Note(text string) { s.say("-- %s", text) }
