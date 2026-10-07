package debug

// Patch sends one method's replacement to the stopped VM as `dbg:patch <text>`
// (rete#313; the text is what nsl.Patch answers). A running program is stopped
// first, by the pause rule. The stop is kept: `restart` then runs the stopped
// frame again in the new code, and `continue` resumes.
func (s *Session) Patch(text string) Result {
	s.cmdMu.Lock() // one command on the line at a time, as Do
	defer s.cmdMu.Unlock()
	if s.Gone() {
		s.say("-- the program ended")
		return Result{Err: "the program ended"}
	}
	if r := s.stopFirst(); r.Err != "" {
		return r
	}
	s.say("-- patch (%d B)", len(text))
	_, r := s.command("dbg:patch "+text, `^dbg:(ok|error)`, answerWait)
	return r
}
