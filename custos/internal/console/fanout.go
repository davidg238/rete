// Copyright (c) 2026 Ekorau LLC

package console

import "sync"

// fanout is whoever is subscribed to a console's lines (a debug session, a
// capture): a cable's and, since rete#415, a mesh sink's, so a profile taken
// through porta reads its dump from the sink as a cable capture reads the cable.
type fanout struct {
	mu   sync.Mutex
	subs map[int]func(string)
	next int
}

// Subscribe calls fn with every line from now on, until cancel.
func (f *fanout) Subscribe(fn func(string)) (cancel func()) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.subs == nil {
		f.subs = map[int]func(string){}
	}
	id := f.next
	f.next++
	f.subs[id] = fn
	return func() {
		f.mu.Lock()
		delete(f.subs, id)
		f.mu.Unlock()
	}
}

// publish hands text to every subscriber, outside the lock: a subscriber may
// cancel itself from inside its own callback.
func (f *fanout) publish(text string) {
	f.mu.Lock()
	subs := make([]func(string), 0, len(f.subs))
	for _, fn := range f.subs {
		subs = append(subs, fn)
	}
	f.mu.Unlock()
	for _, fn := range subs {
		fn(text)
	}
}
