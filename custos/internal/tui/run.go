// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"flag"
	"fmt"
	"io"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
)

// Run is `custos tui`. args excludes the verb; it answers the exit status.
func Run(args []string, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos tui", flag.ContinueOnError)
	fs.SetOutput(stderr)
	url := fs.String("url", "http://127.0.0.1:6971", "the serving custos")
	board := fs.String("board", "", "the board to drive, by name (default: custos's only board; GET /boards lists them)")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	base := *url
	if *board != "" {
		base = strings.TrimRight(base, "/") + "/board/" + *board
	}
	if _, err := tea.NewProgram(New(NewClient(base)), tea.WithAltScreen()).Run(); err != nil {
		fmt.Fprintln(stderr, "custos tui:", err)
		return 1
	}
	return 0
}
