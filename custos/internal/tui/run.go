// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"flag"
	"fmt"
	"io"

	tea "github.com/charmbracelet/bubbletea"
)

// Run is `custos tui`. args excludes the verb; it answers the exit status.
func Run(args []string, stderr io.Writer) int {
	fs := flag.NewFlagSet("custos tui", flag.ContinueOnError)
	fs.SetOutput(stderr)
	url := fs.String("url", "http://127.0.0.1:6971", "the serving custos")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if _, err := tea.NewProgram(New(NewClient(*url)), tea.WithAltScreen()).Run(); err != nil {
		fmt.Fprintln(stderr, "custos tui:", err)
		return 1
	}
	return 0
}
