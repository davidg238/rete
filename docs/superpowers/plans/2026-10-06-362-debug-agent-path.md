# The agent path over /debug (rete#362) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An agent driving custos's `/debug` learns from every answer whether its command did what it says, can drive a host VM with no board, and has a gated, documented way to wait for a stop.

**Architecture:** `debug.Session.Do` answers a `Result{Leave, Err}` (the CLI's text is unchanged except a new line when the VM never answers); `POST /debug/do` wraps it as `{OK, Error, State}`; `--debug-host "<VM command>"` attaches the same session to `StartHost` instead of a cable; the nsl suite's `test_custos_debug.py` gains an HTTP case over `--debug-host`.

**Tech Stack:** Go (custos module), Python 3 (the nsl suite's gate), the host OEVM.

**Spec:** `docs/design/2026-10-06-custos-debug-page-design.md` §5 (and §1–§2 for the session and server it extends).

## Global Constraints

- The CLI's printed text does not change by a byte, except the new `-- no answer to <line> in <timeout>` line (§5). `session_test.go`'s existing text assertions, `test_custos_debug.py`'s CLI case and `tutorial-debugger` are the guard.
- Exactly one of `--debug-port` / `--debug-host`; either needs `--debug-map`.
- Waiting for a stop is a long-poll on `Seq`, never a sleep (D4). The piped CLI keeps the pause rule.
- porta and custos share only `protocol/` (#338): nothing here imports porta.
- Commit with explicit paths, never `-a` (the tree has ~50 untracked board artifacts). Every commit message ends `refs #362` and the attribution lines.
- Never run two nsl suites at once; the suite takes ~20 min — run it in the background with `-u` to a file, never under `timeout`, never through `tail`.

## Review Focus

- A VM that never answers (a hung board, a wrong image): the command answers `Err` "no answer to …", never a silent OK — Task 1 `TestNoAnswerIsAFailure`.
- The program ends during a waited command (`finish` into the end): that is an answer, `Err` is empty and `Run` is `ended` — Task 1 `TestTheEndIsAnAnswer`.
- `--debug-host` naming an image the map does not describe: refused at start-up, not debugged with wrong line numbers — Task 3 `TestDebugHostConfigIsChecked`.
- A `--debug-host` VM that does not exist: custos stops at start-up with the error — Task 3, same test.
- A page user after the answer's shape change: the page still renders the state and shows the error line — Task 4 Step 3 (a manual check over `--debug-host`).

---

### Task 1: A result for every command (`internal/debug`)

**Files:**
- Modify: `custos/internal/debug/session.go` (`waitFrom`, `ask`, `askQuiet`, `stopFirst`, `Do`, `stepLine`; new `Result`, `answerWait`, `command`)
- Modify: `custos/internal/debug/debug.go:120` (`Run` reads `.Leave`)
- Test: `custos/internal/debug/session_test.go`

**Interfaces:**
- Produces: `type Result struct { Leave bool; Err string }`; `func (s *Session) Do(cmd string) Result`. `Err` is the failure as said, without the `-- ` prefix, or the VM's `dbg:error …` line verbatim; `""` when the command did what it says.

- [ ] **Step 1: Write the failing tests** — append to `session_test.go`:

```go
// silentVM never answers: a hung board, as the session sees one.
type silentVM struct{}

func (silentVM) Send(string) error { return nil }

// rete#362: a command says whether it did what it says, as well as saying it.
func TestACommandSaysWhetherItDidWhatItSays(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	if r := s.Do("list"); r.Err != "not stopped in a line" {
		t.Errorf("list before any stop: %+v", r)
	}
	vm := &fakeVM{s: s}
	s.Attach(vm)
	s.OnLine("dbg:ready")
	for cmd, want := range map[string]string{
		"break 37":   "",
		"frobnicate": "unknown: frobnicate (type help)",
		"locals x":   `locals takes a frame number from bt, got "x"`,
	} {
		if r := s.Do(cmd); r.Err != want || r.Leave {
			t.Errorf("%s: %+v, want Err %q", cmd, r, want)
		}
		vm.wg.Wait()
	}
	if r := s.Do("break 9999"); r.Err == "" {
		t.Errorf("a line with no code anywhere was accepted: %+v", r)
	}
	if r := s.Do("quit"); !r.Leave || r.Err != "" {
		t.Errorf("quit: %+v", r)
	}
	vm.refuse = true
	if r := s.Do("break 37"); r.Err != "dbg:error no code at that pc" {
		t.Errorf("a refused break: %+v", r)
	}
	vm.wg.Wait()
}

func TestNoAnswerIsAFailure(t *testing.T) {
	defer func(w time.Duration) { answerWait = w }(answerWait)
	answerWait = 50 * time.Millisecond
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.Attach(silentVM{})
	s.OnLine("dbg:ready")
	out.Reset()
	r := s.Do("break 37")
	if !strings.HasPrefix(r.Err, "no answer to dbg:break ") {
		t.Fatalf("a break nobody answered: %+v", r)
	}
	if !strings.Contains(out.String(), "-- no answer to dbg:break ") {
		t.Errorf("the CLI did not say it:\n%s", out.String())
	}
	if len(s.State(-1, 0).Breaks) != 0 {
		t.Errorf("an unanswered break is in the state")
	}
}

func TestTheEndIsAnAnswer(t *testing.T) {
	sm, _ := loaded(t)
	var out strings.Builder
	s := NewSession(sm, &out)
	s.Attach(&fakeVM{s: s}) // answers nothing to dbg:out
	s.OnLine("dbg:paused break 200 2")
	go func() { time.Sleep(20 * time.Millisecond); s.OnEnd() }()
	if r := s.Do("finish"); r.Err != "" {
		t.Fatalf("finish into the end: %+v", r)
	}
	if run := s.State(-1, 0).Run; run != "ended" {
		t.Errorf("run %q", run)
	}
}
```

Add `"time"` to the test file's imports.

- [ ] **Step 2: Run them and see them fail**

Run: `cd custos && go test ./internal/debug/ -run 'Says|NoAnswer|TheEnd' -count=1`
Expected: build failure — `s.Do(...)` is a `bool`, `r.Err` undefined, `answerWait` undefined.

- [ ] **Step 3: Implement** in `session.go`:

Above `LineStepLimit`, add:

```go
// answerWait is how long a command waits for the VM's answer (a var so a test
// can shorten it). A run to the next stop (finish, a line step) waits 30 s.
var answerWait = 10 * time.Second

// Result is what one command came to.
type Result struct {
	Leave bool   // quit: the person is done
	Err   string // why the command did not do what it says, as said; "" when it did
}
```

`waitFrom` answers whether the wait was answered (a match, or the VM's end) or timed out:

```go
func (s *Session) waitFrom(pattern string, timeout time.Duration, mark int) ([]seen, bool) {
```
— `return got` in the match/end branch becomes `return got, true`; the deadline branch returns `append([]seen(nil), s.seen[mark:]...), false`. `ask` returns `([]seen, bool)` and passes both through. `Wait` calls `s.waitFrom(pattern, timeout, mark)` as a statement (unchanged). `askQuiet`: `got, _ := s.ask(line, pattern, answerWait); return texts(got)` (frames.go already turns an empty answer into "no answer to dbg:backtrace").

Below `askQuiet`, add:

```go
// command is ask for a command: a dbg:error answer is the VM's refusal (its line
// is already in the transcript), and no answer is said, since nothing else would
// say it (rete#362).
func (s *Session) command(line, pattern string, timeout time.Duration) ([]seen, Result) {
	got, ok := s.ask(line, pattern, timeout)
	if !ok {
		msg := fmt.Sprintf("no answer to %s in %s", line, timeout)
		s.say("-- %s", msg)
		return got, Result{Err: msg}
	}
	if l := got[len(got)-1]; !l.end && strings.HasPrefix(l.text, "dbg:error") {
		return got, Result{Err: l.text}
	}
	return got, Result{}
}
```

`stopFirst` answers a `Result`:

```go
func (s *Session) stopFirst() Result {
	if s.Running() {
		_, r := s.command("dbg:pause", `^dbg:ok pause$`, answerWait)
		return r
	}
	return Result{}
}
```

`Do(cmd string) Result`. At its top, after the lock:

```go
	fail := func(format string, args ...any) Result {
		msg := fmt.Sprintf(format, args...)
		s.say("-- %s", msg)
		return Result{Err: msg}
	}
```

Then, mechanically through `Do`:
- `return true` → `return Result{}`; `return false` → `return Result{Leave: true}`.
- every `s.stopFirst()` statement → `if r := s.stopFirst(); r.Err != "" { return r }` (in the `pause` branch too).
- every `s.say("-- %s", err); return true` → `return fail("%s", err)`; `s.say("-- not stopped in a line"); return true` → `return fail("not stopped in a line")`; the `locals` arg error → `return fail("locals takes a frame number from bt, got %q", arg)`; unknown → `return fail("unknown: %s (type help)", word)`.
- the `dbg:` pass-through: `_, r := s.command(cmd, `^dbg:(ok|error|stack|frames|paused)`, answerWait); return r`.
- `break`/`clear`: `got, r := s.command(fmt.Sprintf("dbg:%s %d %d", verb, bp.Off, bp.PC), `^dbg:(ok|error)`, answerWait)`, loop over `got` as today, then `return r`.
- `dbg:inspect`: `_, r := s.command(verb, `^dbg:(stack|error)`, answerWait); return r`. `dbg:out`: `_, r := s.command(verb, `^dbg:paused |^dbg:error`, 30*time.Second); return r`. Default: `return s.stepLine(verb)`. `dbg:continue` falls through to `return Result{}`.

`stepLine(verb string) Result`: the loop's ask becomes

```go
		got, r := s.command(verb, `^dbg:paused |^dbg:error`, 30*time.Second)
		if r.Err != "" {
			stepping(false)
			return r
		}
```
keeping the rest of the loop; its bare `return`s become `return Result{}`, and the function ends `return Result{}`.

In `debug.go`'s `Run`: `if !s.Do(cmd) {` → `if s.Do(cmd).Leave {`.

- [ ] **Step 4: Run the package and the vet**

Run: `cd custos && go vet ./... && go test ./internal/debug/ -count=1`
Expected: `ok` — the three new tests and every existing text assertion in `session_test.go`/`state_test.go`. If `break 9999` passes with `Err == ""`, read `SourceMap.Resolve` and pick a line it refuses; do not weaken the assertion.

- [ ] **Step 5: Commit**

```bash
git add custos/internal/debug/session.go custos/internal/debug/debug.go custos/internal/debug/session_test.go
git commit -m "custos: every debug command answers whether it did what it says, refs #362"
```

---

### Task 2: `/debug/do` answers `{OK, Error, State}`; the page shows the error

**Files:**
- Modify: `custos/internal/custos/debug.go` (the `POST /debug/do` handler; header comment)
- Modify: `custos/internal/custos/debug.html:127-129` (`send`)
- Test: `custos/internal/custos/debug_test.go`

**Interfaces:**
- Consumes: `debug.Result` from Task 1.
- Produces: the JSON answer `{"OK": bool, "Error": string, "State": debug.State}` — Go's field names, as `State`'s own are. Task 4's gate reads these keys.

- [ ] **Step 1: Write the failing test** — in `debug_test.go`, add a decoder and replace `TestABreakByCommandIsInTheState`:

```go
func doAnswer(t *testing.T, srv *httptest.Server, cmd string) (int, done) {
	t.Helper()
	resp := do(t, srv, cmd)
	defer resp.Body.Close()
	var d done
	if err := json.NewDecoder(resp.Body).Decode(&d); err != nil {
		t.Fatal(err)
	}
	return resp.StatusCode, d
}

func TestABreakByCommandIsInTheState(t *testing.T) {
	srv, _ := debugServer(t, nil)
	code, d := doAnswer(t, srv, "break 20")
	if code != http.StatusOK || !d.OK || d.Error != "" || strings.Join(d.State.Breaks, ",") != "Fixture.nsl:20" {
		t.Fatalf("status %d, answer %+v", code, d)
	}
}

// rete#362: a failed command is a 200 whose OK is false, with the reason.
func TestAFailedCommandSaysSo(t *testing.T) {
	srv, _ := debugServer(t, nil)
	code, d := doAnswer(t, srv, "frobnicate")
	if code != http.StatusOK || d.OK || d.Error != "unknown: frobnicate (type help)" {
		t.Fatalf("status %d, answer %+v", code, d)
	}
}
```

- [ ] **Step 2: Run and see it fail**

Run: `cd custos && go test ./internal/custos/ -run 'ABreakByCommand|AFailedCommand' -count=1`
Expected: build failure, `undefined: done`.

- [ ] **Step 3: Implement** — in `debug.go`, above `mountDebug`:

```go
// done is /debug/do's answer: whether the command did what it says, why not
// (as the CLI said it, or the VM's dbg:error line), and the state after it.
type done struct {
	OK    bool
	Error string
	State debug.State
}
```

The handler's last two lines become:

```go
		r := s.Do(strings.TrimSpace(string(body))) // quit has nothing to leave: the cable stays held
		answer(w, done{r.Err == "", r.Err, s.State(-1, 0)})
```

and the header comment's `POST /debug/do` line reads `one command line, as typed at `custos debug`; answers {OK, Error, State}`.

In `debug.html`'s `send`, `else { fail(''); render(await resp.json()); }` becomes

```js
    else { const d = await resp.json(); fail(d.Error); render(d.State); }
```

- [ ] **Step 4: Run the package**

Run: `cd custos && go test ./internal/custos/ -count=1`
Expected: `ok`.

- [ ] **Step 5: Commit** (the page is checked by hand in Task 4, Step 3)

```bash
git add custos/internal/custos/debug.go custos/internal/custos/debug.html custos/internal/custos/debug_test.go
git commit -m "custos: /debug/do answers {OK, Error, State}, and the page shows the error, refs #362"
```

---

### Task 3: `--debug-host "<VM command>"`

**Files:**
- Modify: `custos/internal/custos/custos.go` (`Config.DebugHost`; the debug block of `Handler`)
- Modify: `custos/cmd/custos/main.go:60-62` (the flag; the map flag's help)
- Test: `custos/internal/custos/debug_test.go`

**Interfaces:**
- Consumes: `debug.StartHost(argv []string, onLine func(string), onEnd func()) (Line, error)`, `(*debug.SourceMap).CheckImage(path string) error`.
- Produces: `Config.DebugHost string` — a host VM command line, split on white space (`/tmp/oevm_img /tmp/x.oimg`). Task 4 starts custos with `--debug-host`.

- [ ] **Step 1: Write the failing test** — replace `TestDebugPortConfigIsChecked` with:

```go
func TestDebugPortConfigIsChecked(t *testing.T) {
	if _, err := Handler(Config{DebugPort: "/dev/null"}); err == nil || !strings.Contains(err.Error(), "--debug-map") {
		t.Errorf("a debug port without a map: %v", err)
	}
	_, err := Handler(Config{DebugPort: "/dev/ttyX", DebugMap: fixtureMap(t), ProfilePort: "/dev/ttyX", ProfileMap: fixtureMap(t)})
	if err == nil || !strings.Contains(err.Error(), "one cable") {
		t.Errorf("one port for both pages: %v", err)
	}
}

// rete#362: a host VM is the second cable, and exactly one is held.
func TestDebugHostConfigIsChecked(t *testing.T) {
	m := fixtureMap(t)
	cases := map[string]Config{
		"one of":       {DebugPort: "/dev/ttyX", DebugHost: "/bin/true", DebugMap: m},
		"--debug-map":  {DebugHost: "/bin/true"},
		"--debug-host": {DebugHost: "/nonexistent/oevm_img", DebugMap: m},
	}
	for want, cfg := range cases {
		if _, err := Handler(cfg); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%+v: %v, want an error naming %q", cfg, err, want)
		}
	}
	wrong := filepath.Join(t.TempDir(), "wrong.oimg")
	os.WriteFile(wrong, []byte("not the image the map describes"), 0o644)
	if _, err := Handler(Config{DebugHost: "/bin/true " + wrong, DebugMap: m}); err == nil {
		t.Errorf("an image the map does not describe was accepted")
	}
}
```

Add `"os"` and `"path/filepath"` to the imports.

- [ ] **Step 2: Run and see it fail**

Run: `cd custos && go test ./internal/custos/ -run DebugHost -count=1`
Expected: build failure, `unknown field DebugHost`.

- [ ] **Step 3: Implement** — in `custos.go`'s `Config`, below `DebugPort`:

```go
	// DebugHost is a host VM command line (split on white space) the debug page
	// runs instead of holding a board's cable: the agent's board-free path
	// (rete#362). Exactly one of DebugPort and DebugHost.
	DebugHost string
```

`Handler`'s debug block becomes:

```go
	if cfg.DebugPort != "" || cfg.DebugHost != "" {
		if cfg.DebugPort != "" && cfg.DebugHost != "" {
			return nil, fmt.Errorf("custos: give one of --debug-port or --debug-host")
		}
		if cfg.DebugMap == "" {
			return nil, fmt.Errorf("custos: the debug page needs --debug-map (<name>.oimg.map.json)")
		}
		if cfg.DebugPort != "" && cfg.DebugPort == cfg.ProfilePort {
			return nil, fmt.Errorf("custos: --debug-port and --profile-port are one cable, and a cable has one holder")
		}
		sm, err := debug.LoadSourceMap(cfg.DebugMap, cfg.DebugSource)
		if err != nil {
			return nil, err
		}
		s := debug.NewSession(sm, io.Discard)
		s.AutoFetch = true
		var line debug.Line
		if cfg.DebugPort != "" {
			if line, err = debug.OpenPort(cfg.DebugPort, 115200, s.OnLine, s.OnLost); err != nil {
				return nil, fmt.Errorf("custos: --debug-port: %w", err)
			}
		} else {
			argv := strings.Fields(cfg.DebugHost)
			for _, a := range argv {
				if strings.HasSuffix(a, ".oimg") {
					if err := sm.CheckImage(a); err != nil {
						return nil, fmt.Errorf("custos: --debug-host: %w", err)
					}
				}
			}
			if line, err = debug.StartHost(argv, s.OnLine, s.OnEnd); err != nil {
				return nil, fmt.Errorf("custos: --debug-host: %w", err)
			}
		}
		s.Attach(line)
		mountDebug(mux, s, sm)
	}
```

(The old "--debug-port needs --debug-map" message now reads "the debug page needs --debug-map"; the existing test asserts only `--debug-map`.) Update the comment above `Handler` ("/debug a debugging session on a board") to "on a board or a host VM".

In `main.go`, after the `debug-port` flag:

```go
	flag.StringVar(&cfg.DebugHost, "debug-host", "", "a host VM command line for the debug page instead of a board: \"/tmp/oevm_img app.oimg\" (rete#362)")
```

and the `debug-map` help becomes `"the <name>.oimg.map.json of the image on --debug-port or --debug-host"`.

- [ ] **Step 4: Run the module**

Run: `cd custos && go vet ./... && go test ./... -count=1`
Expected: `ok` for every package.

- [ ] **Step 5: Commit**

```bash
git add custos/internal/custos/custos.go custos/cmd/custos/main.go custos/internal/custos/debug_test.go
git commit -m "custos: --debug-host, the debug page over a host VM, refs #362"
```

---

### Task 4: The real-VM gate over HTTP (`test_custos_debug.py`)

**Files:**
- Modify: `nodus/nsl/tools/test_custos_debug.py` (new `run_http`; `main` runs it after `run`; docstring)

**Interfaces:**
- Consumes: `--debug-host` (Task 3), the `{OK, Error, State}` answer (Task 2), `State`'s keys `Seq`, `Run`, `Stop.Line`, `Breaks`, `Locals.Vars[].Name`.

- [ ] **Step 1: Add the case** — below `run()`:

```python
def run_http():
    """rete#362: the agent path. custos serves the session over --debug-host; every
    answer is read as data ({OK, Error, State}), and a stop is waited for by
    long-polling on Seq, never by sleeping."""
    import json, socket, time, urllib.error, urllib.request
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    base = f"http://127.0.0.1:{port}"
    src = os.path.basename(PAUSE_APP)
    env = {**os.environ, "OEVM_TIMER_WALLCLOCK": "1"}
    proc = subprocess.Popen([CUSTOS, "--bind", "127.0.0.1", "--port", str(port),
                             "--debug-host", f"{VM} {OIMG}",
                             "--debug-map", OIMG + ".map.json", "--debug-source", PAUSE_APP],
                            cwd=OEVM, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)

    def get(path):
        with urllib.request.urlopen(base + path, timeout=30) as r:
            return json.load(r)

    def do(cmd):
        req = urllib.request.Request(base + "/debug/do", data=cmd.encode(), method="POST")
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)

    def until(what, ok, seq=-1):
        # The long-poll answers on the next change, or after 25 s with nothing new.
        deadline = time.monotonic() + 30
        while True:
            st = get(f"/debug/state?after={seq}")
            if ok(st):
                return st
            if time.monotonic() > deadline:
                raise AssertionError(f"never {what}; last Run={st['Run']} Stop={st['Stop']}")
            seq = st["Seq"]

    try:
        # custos's listener is up when it answers; this is start-up, not a stop.
        for _ in range(100):
            if proc.poll() is not None:
                raise AssertionError(f"custos exited: {proc.stderr.read()}")
            try:
                get("/debug/state")
                break
            except urllib.error.URLError:
                time.sleep(0.1)
        until("at boot", lambda st: st["Run"] == "boot")
        d = do("break 37")
        assert d["OK"] and d["Error"] == "", d
        assert f"{src}:37" in d["State"]["Breaks"], d["State"]["Breaks"]
        d = do("break 1")  # the file's header comment: outside every method
        assert not d["OK"] and d["Error"], d
        d = do("continue")
        assert d["OK"], d
        st = until("stopped at 37 with its locals",
                   lambda st: st["Run"] == "stopped" and st["Stop"]["Line"] == 37
                   and st["Locals"] is not None, d["State"]["Seq"])
        names = [v["Name"] for v in st["Locals"]["Vars"]]
        assert "n" in names, names
        assert do("clear 37")["OK"]
        d = do("continue")
        until("ended", lambda st: st["Run"] == "ended", d["State"]["Seq"])
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    return f"http: break 37 OK, break 1 refused, stopped at 37 with {names}, ended"
```

`main` becomes:

```python
    build()
    transcript = run()
    http = run_http()
    print("test_custos_debug: PASSED (Path B, custos debug by source line, and /debug over --debug-host)")
    print(transcript)
    print(http)
```

Add to the module docstring, after its first paragraph: `The same session is then driven as an agent drives it (rete#362): custos serving /debug over --debug-host, every command's {OK, Error, State} checked, and the stop awaited by long-polling on Seq.`

- [ ] **Step 2: Run the gate**

Run: `cd nodus/nsl && python3 tools/test_custos_debug.py`
Expected: `test_custos_debug: PASSED (... /debug over --debug-host)` and the `http:` line. (It builds the Path B VM; not concurrently with a suite.)

- [ ] **Step 3: Check the page by hand over a host VM** (Review Focus 5). Step 2 left the image and the custos binary in `/tmp`:

```bash
cd nodus/oevm && OEVM_TIMER_WALLCLOCK=1 /tmp/custos_debug_gate --bind 127.0.0.1 --port 6979 \
  --debug-host "/tmp/oevm_img /tmp/custos_debug_pause.oimg" \
  --debug-map /tmp/custos_debug_pause.oimg.map.json \
  --debug-source ../tests/04_somns_basic/47_debug_pause_target.nsl
```

Open `http://127.0.0.1:6979/debug` (headless Chrome, per the memory on local HTML: screenshot and Read the PNG). Click line 37, then `continue` → the stop at 37 is shown with its stack. Type `frobnicate` in the command box → the error line shows `unknown: frobnicate (type help)`. Record what was seen for the issue comment.

- [ ] **Step 4: Falsify it** — in `custos/internal/custos/debug.go` change `done{r.Err == "", ...}` to `done{true, ...}`, rerun Step 2, and confirm it FAILS with the `break 1` assertion (`AssertionError: {'OK': True, ...}`). Restore the line (`git diff --stat` shows only the gate file changed), rerun Step 2, and confirm PASSED.

- [ ] **Step 5: Commit**

```bash
git add nodus/nsl/tools/test_custos_debug.py
git commit -m "nsl gate: /debug over --debug-host, driven as an agent drives it, refs #362"
```

---

### Task 5: The agent path, documented; the spec agrees with what was built

**Files:**
- Modify: `docs/tutorials/04-debugger.md` (a new §8 after §7; a sentence in §2 or §5 on the piped race)
- Modify: `docs/design/2026-10-06-custos-debug-page-design.md` §5 (as-built corrections)
- Modify: `docs/development.md` §5 custos row, if it lists the debug flags

- [ ] **Step 1: Write tutorial §8** — insert before `## Where next`. Plain `bash` fences, NOT `bash gate` (the tutorial gate runs only `bash gate` blocks):

````markdown
## 8. The agent path

An agent drives the same session over HTTP (rete#362). Without a board, custos runs
the host VM itself:

```bash
custos --debug-host "/tmp/oevm_img /tmp/46_debug_target.oimg" \
       --debug-map /tmp/46_debug_target.oimg.map.json
```

Every `POST /debug/do` answers `{OK, Error, State}`. `OK` is false when the command
did not do what it says (a line with no code, an unknown word, the VM's `dbg:error`,
or no answer at all), and `Error` says why. To wait for a stop, never sleep: long-poll
on `Seq` until `Run` is `stopped` (or `ended`, or `lost`):

```bash
seq=$(curl -s -d continue localhost:6971/debug/do | jq .State.Seq)
until st=$(curl -s "localhost:6971/debug/state?after=$seq"); \
      echo "$st" | jq -e '.Run | test("stopped|ended|lost")' >/dev/null; do
  seq=$(echo "$st" | jq .Seq)
done
echo "$st" | jq '.Stop, .Locals'
```

A piped `custos debug` script cannot do this: a line after `continue` arrives while
the program runs, and the pause rule (section 5) stops it wherever it is. Use the page's
endpoints for anything that waits on a stop.
````

- [ ] **Step 2: Correct the spec's §5 as built** — in the design doc: the gate drives `47_debug_pause_target` (the image `test_custos_debug.py` already builds), `break 37`, `n` in locals, and `break 1` as the refused line — not `46_debug_target`/26/`sum`; the answer's keys are Go's field names `{OK, Error, State}`, as `State`'s own are. Mark both *As built (2026-10-06)*.

- [ ] **Step 3: `docs/development.md`** — `rg -n "debug-port" docs/development.md`; where the custos row names `--debug-port`, add `--debug-host`. If nothing matches, change nothing.

- [ ] **Step 4: Run the tutorial gate alone**

Run: `bash docs/tutorials/gate.sh docs/tutorials/04-debugger.md` (read `run_tutorial_gate` in `nodus/nsl/tools/run_tests_oevm.py` for the exact invocation if this differs)
Expected: PASS — §8 adds no gate blocks, so this proves only that nothing else moved.

- [ ] **Step 5: Commit**

```bash
git add docs/tutorials/04-debugger.md docs/design/2026-10-06-custos-debug-page-design.md docs/development.md
git commit -m "docs: the agent path over /debug (tutorial 04 §8), §5 as built, refs #362"
```

---

### Task 6: The gates, and the close-out

- [ ] **Step 1:** `for m in protocol porta custos; do (cd $m && go test -count=1 ./...); done` → all `ok`.
- [ ] **Step 2:** The nsl suite, in the background (`free -g` first; nothing else touching `/tmp/oevm_img`):
  `cd nodus/nsl && python3 -u tools/run_tests_oevm.py > /tmp/nsl-362.log 2>&1` → `Failed: 0`, and the aux line shows `custos-debug: PASSED` and `tutorial-debugger` PASS (footprint is #358's known RED; any other non-PASS is a finding).
- [ ] **Step 3:** Comment on #362 (≤ 5 lines): the commits, the gate's `http:` line, the falsification seen red, Task 4 Step 3's page check. Promotion target: `docs/tutorials/04-debugger.md` §8 and the design doc §5.
