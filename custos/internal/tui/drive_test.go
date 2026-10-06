// Copyright (c) 2026 Ekorau LLC

package tui

import (
	"testing"
	"time"

	tea "github.com/charmbracelet/bubbletea"
)

// drive runs cmd and every command the model answers with, until until(m)
// holds or timeout passes. A tea.BatchMsg is expanded; quit is ignored.
func drive(t *testing.T, m Model, cmd tea.Cmd, timeout time.Duration, until func(Model) bool) Model {
	t.Helper()
	msgs := make(chan tea.Msg, 64)
	run := func(c tea.Cmd) {
		if c != nil {
			go func() { msgs <- c() }()
		}
	}
	run(cmd)
	deadline := time.After(timeout)
	for !until(m) {
		select {
		case msg := <-msgs:
			switch msg := msg.(type) {
			case tea.BatchMsg:
				for _, c := range msg {
					run(c)
				}
				continue
			case tea.QuitMsg, nil:
				continue
			}
			nm, c := m.Update(msg)
			m = nm.(Model)
			run(c)
		case <-deadline:
			t.Fatalf("timed out; status=%q run=%q stop=%+v", m.status, m.st.Run, m.st.Stop)
		}
	}
	return m
}

// press delivers keys in order and answers the batch of commands they asked for.
func press(t *testing.T, m Model, keys ...string) (Model, tea.Cmd) {
	t.Helper()
	var cmds []tea.Cmd
	for _, k := range keys {
		var msg tea.KeyMsg
		switch k {
		case "enter":
			msg = tea.KeyMsg{Type: tea.KeyEnter}
		case "esc":
			msg = tea.KeyMsg{Type: tea.KeyEsc}
		case "backspace":
			msg = tea.KeyMsg{Type: tea.KeyBackspace}
		case "tab":
			msg = tea.KeyMsg{Type: tea.KeyTab}
		case "up":
			msg = tea.KeyMsg{Type: tea.KeyUp}
		case "down":
			msg = tea.KeyMsg{Type: tea.KeyDown}
		default:
			msg = tea.KeyMsg{Type: tea.KeyRunes, Runes: []rune(k)}
		}
		nm, c := m.Update(msg)
		m = nm.(Model)
		cmds = append(cmds, c)
	}
	return m, tea.Batch(cmds...)
}
