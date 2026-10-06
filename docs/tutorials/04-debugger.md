# Tutorial 4 — the debugger: stop a program at a line, step it, look inside

A debugger for nsl programs, on your desktop and on an ESP32-C6 over its console cable.
You name a line of your `.nsl` file; the program stops there and shows you the line.
From there you can step, look at the frame, and let it run. A program that is already
running can also be stopped where it is.

custos serves the debugger over HTTP, and that is how an agent drives it: every command
is a `POST` that answers JSON, and a stop is waited for by long-polling, never by
sleeping. The same session is a page for a person (§6). HTTP is the only surface this
page advertises (custos peer design D12); the command-line verbs are kept, in §9.

**The desktop half of this page is executed.** Every block marked `bash gate` is run, in
order, in a fresh empty directory, by `docs/tutorials/gate.sh`. The nsl suite runs it as
its `tutorial-debugger` aux gate, so a stale step here turns the suite red. The board half
(§7) needs a board and is prose.

## 0. What you need

Tutorial 1 done, or the suite run once. Either way the VM and its layout file are in
`/tmp` (`/tmp/oevm_img`, `/tmp/oevm_layout.json`). `RETE` points at the clone. The
debugger is served by `custos`, the developer's tool, so you also need Go, `curl` and
`jq` (the suite SKIPs this page, by name, without Go).

## 1. A program to stop

```bash gate
cat > average.nsl <<'EOF'
(*
expect:
mean 4
*)
class Average packageUsing: manifest = () (
  sum: xs = (
    | total |
    total:: 0.
    xs do: [ :x | total:: total + x ].
    ^ total )

  public main: platform args: args = (
    | xs mean |
    xs:: Array new: 3.
    xs at: 1 put: 2. xs at: 2 put: 4. xs at: 3 put: 6.
    mean:: (self sum: xs) / xs size.
    ('mean ' , mean printString) println.
    ^ self )
) : ( )
EOF
python3 $RETE/nodus/nsl/tools/ns_compile_oevm.py average.nsl average.oimg \
        --format=oimg --layout=/tmp/oevm_layout.json
ls average.oimg average.oimg.map.json
```

The compiler writes `average.oimg.map.json` beside the image: for every method, its
name, its source file and which bytecode offset starts which line, and which class the
image runs. That file is how the debugger knows where line 9 is. It also records the
image's hash, so the debugger refuses a map from another build instead of pointing at the
wrong bytes.

## 2. Start custos on the program

Build custos, then start it with `--debug-host`: custos runs the host VM itself, with its
debugger on, and serves the session (rete#362). On a board the same flag is
`--debug-port` (§7).

```bash gate
go build -C "$RETE/custos" -o "$PWD/custos" ./cmd/custos
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./custos --bind 127.0.0.1 --port "$port" \
         --debug-host "/tmp/oevm_img average.oimg" --debug-map average.oimg.map.json \
         2> custos.log &
custos_pid=$!
trap 'kill $custos_pid' EXIT
base=http://127.0.0.1:$port
for _ in $(seq 100); do curl -sf "$base/debug/state" > /dev/null && break; sleep 0.1; done
curl -sf "$base/debug/state" | jq -e '.Run == "boot"'
```

The program waits before its first line: a debug run always does, so you can set
breakpoints first. That is `Run` = `boot`. (The `sleep` above waits for custos's listener
to come up; it is the only sleep on this page.)

There are three endpoints:

| endpoint | answers |
|---|---|
| `POST /debug/do` | one command (§3) as the body; answers `{OK, Error, State}` |
| `GET /debug/state?after=N` | the `State` once its `Seq` passes `N` (a long-poll, 25 s at most) |
| `GET /debug/source` | the app's lines, and which of them have code |

`OK` is false when the command did not do what it says (a line with no code, an unknown
word, the VM's `dbg:error`, or no answer at all), and `Error` says why. `State` holds
`Seq` (it rises on every change), `Run` (`boot`, `running`, `stopped`, `ended`, `lost`),
`Stop` (file, line, method and reason), `Frames` (the last `bt`), `Locals`, `Breaks` and
`Transcript`, the lines a person would have seen.

## 3. A session

Two small helpers: `dbg` sends one command, and `wait_stop` long-polls on `Seq` until the
program is no longer running.

```bash gate
dbg() { curl -s -d "$1" "$base/debug/do"; }
wait_stop() {   # $1 = the Seq to wait past; prints the State once Run is not running
  local seq=$1 st
  for _ in $(seq 20); do
    st=$(curl -s "$base/debug/state?after=$seq")
    if echo "$st" | jq -e '.Run | test("stopped|ended|lost")' > /dev/null; then
      echo "$st"; return 0
    fi
    seq=$(echo "$st" | jq .Seq)
  done
  return 1
}
```

Set a breakpoint, run to it, and step into `sum:`:

```bash gate
dbg 'break 16' | jq -e '.OK and (.State.Breaks == ["average.nsl:16"])'
dbg 'break 7' | jq -r '.Error'
seq=$(dbg continue | jq .State.Seq)
wait_stop "$seq" | jq -e '.Stop.Reason == "break" and .Stop.Line == 16'
dbg step | jq -e '.State.Stop.Line == 8 and .State.Stop.Method == "Average>>sum:"'
dbg next | jq -e '.State.Stop.Line == 9'
```

```
true
line 7 has no code; the nearest lines that do: 8 (sum:)
true
true
true
```

`continue` answers at once, with `Run` = `running`; the stop comes later, and
`wait_stop` is what sees it. `step`, `next`, `finish`, `bt` and `locals` answer when they
are done, so their own answer already holds the stop.

Look inside: the stack, this frame's names, and the caller's:

```bash gate
dbg bt | jq -r '.State.Frames[].Where' | tee frames.txt
dbg locals | jq -r '.State.Locals.Vars[] | "\(.Name) = \(.Value)"' | tee locals0.txt
dbg 'locals 1' | jq -r '.State.Locals.Vars[] | "\(.Name) = \(.Value)"' | tee locals1.txt
grep -qx 'at average.nsl:16  Average>>main:args:' frames.txt
grep -qx 'total = 0' locals0.txt
grep -qx 'mean = nil' locals1.txt
```

```
at average.nsl:9  Average>>sum:
at average.nsl:16  Average>>main:args:
at supervisor.nsl:38  Supervisor>>run:platform:args:debug:
self = an Object
xs = an Object
total = 0
self = an Object
platform = an Object
args = 0
xs = an Object
mean = nil
```

Finish `sum:`, step over the print, and let it end:

```bash gate
dbg finish | jq -e '.State.Stop.Line == 16'
dbg next | jq -e '.State.Stop.Line == 17'
dbg next | jq -e '.State.Stop.Line == 18 and (.State.Transcript | index("mean 4") != null)'
seq=$(dbg continue | jq .State.Seq)
wait_stop "$seq" | jq -e '.Run == "ended"'
```

The program's own output (`mean 4`) is in `Transcript`, beside the lines the debugger
adds: a line starting `--` is the debugger talking, a line starting `dbg:` is the VM's own
answer, passed through. The suite's `custos-debug` gate drives this path too, against a
program that ticks (§5).

## 4. The commands

Each is the body of one `POST /debug/do`; the page's command box (§6) takes the same.

| command | short | what it does |
|---|---|---|
| `break <line>` / `break <file>:<line>` | `b` | stop when the program reaches that line |
| `clear <line>` | | remove that breakpoint |
| `continue` | `c` | run until a breakpoint, a pause, or the end |
| `step` | `s` | run to the next line, going into a method that line calls |
| `next` | `n` | run to the next line in this method (or its caller, at a return) |
| `finish` | `f` | run until this method returns |
| `pause` | `p` | stop a running program where it is (§5) |
| `where` | `w` | the line it is stopped at |
| `list` | `l` | the lines around it |
| `bt` | | the call stack, innermost first, one `file:line` per frame |
| `locals [N]` | | frame N's `self`, arguments, temporaries and captured locals by name |
| `inspect` | `i` | the frame's raw registers |
| `methods` | | every method of the app's file, with its line range |
| `dbg:...` | | sent to the VM unchanged, e.g. `dbg:step` for one bytecode |
| `quit` | `q` | leave. The VM is told nothing more and runs on |

What they can and cannot do:

- **A breakpoint goes on any line with code in the app's file**: a method of any of its
  classes, or a line inside a block the compiler kept as a block (a `whenever: ... do:
  [ ... ]` handler, say), which is its own method. The VM names every method by its offset
  in the image, and the image map turns that offset into a name and a line (rete#323).
- **A line with no code of its own** (a method header, `| total |`, a comment) is
  refused with the nearest lines that have code, as `break 7` was above. Inside a method,
  a line with no code moves forward to the next line that has some, and the debugger
  says so.
- **`step` and `next` move a line at a time** by sending the VM single-bytecode steps
  until the line changes. A loop on one line is many of those, so a long one takes a
  while. After 5000 the debugger stops and says where it is (`LineStepLimit` in
  `custos/internal/debug/session.go`).
- **A breakpoint does not fire during `next` or `finish`.** While the VM steps, the
  Supervisor decides each stop by frame depth alone and never checks the breakpoints. So
  `next` over a call passes a breakpoint inside that call, and `finish` out of a method
  that never returns (a `main:args:` that loops forever) leaves the program stepping:
  it runs on, a breakpoint never fires, and a command is not seen. `dbg:continue`
  (sent raw, or `continue`) ends the step. A command sent during a long `next` or
  `finish` lands when the step does (rete#321).
- **`locals` names what the frame holds**, from the image map: `self`, the arguments,
  the temporaries, and the locals a kept block captured (`total` above, captured by the
  `do:` block). A captured local in a frame stopped before its method's first bytecode
  shows `(not yet set)`. A loop variable of an inlined loop (`1 to: n do: [ :i | ... ]`)
  is shown only while the stop is inside the loop body. `locals 1` and deeper need a `bt`
  at this stop first. `inspect` still shows the raw registers (`dbg:stack off=3 r0=…`).
- **A stop in the kernel is named by its `kernel.nsl` line**: `at kernel.nsl:3160
  Kernel_Space>>drain`. A pause usually lands there, because a program that is waiting
  for its next event is waiting inside the kernel; `bt` shows how the app got there.
  So does `finish` out of `main:args:`: what called it is the Supervisor.

## 5. Stopping a running program

Any command that reaches the VM while the program runs stops it at its next bytecode.
That is the VM's rule, not custos's: the line arriving is what stops it. So `pause`, and
also `break 37` sent while it runs, both leave the program stopped, and `continue`
resumes it. A program that is asleep with no timer never reaches a bytecode, so nothing
stops it until it wakes up.

This is why a stop is waited for with `wait_stop` and never with a command: a command
sent after `continue` to ask "stopped yet?" would itself stop the program, wherever it
had got to. The long-poll asks custos, not the VM.

The suite's `custos-debug` gate drives a running program over HTTP, against
`nodus/tests/04_somns_basic/47_debug_pause_target.nsl`, a program that ticks on a 200 ms
timer.

## 6. The same session as a page

The session custos serves is also a page, at `/debug` (rete#340): start custos as in §2
(or with `--debug-port`, §7) and open `http://localhost:6971/debug` (6971 is custos's
default `--port`).

The page shows the source, the stop, the stack and frame 0's locals, which are fetched
as soon as a stop is shown. Click a line to set or clear a breakpoint, and click a frame
for its locals. The buttons and the command box send the same commands as §4 to the same
`/debug/do`, and the Session pane shows `Transcript`. A board that was already waiting
when custos opened the cable shows *unknown* until you press pause; that is the rule
from §5.

## 7. On a board (ESP32-C6, one USB cable)

On a board the debugger is a separate image. The checks that make breakpoints possible
cost 11.6% per bytecode and ~500 B of flash, so the normal image leaves them out. An
image built with them is the debug image: it prints `dbg:ready` at boot and waits for
its cable, with no timeout.

1. Build and flash it with the hooks in:
   `OEVM_DEBUG_HOOKS=1 run.sh --node YourApp.nsl` (from `nodus/oevm/zephyr`, as in
   tutorial 2). The image map, `YourApp.oimg.map.json`, is written beside `YourApp.nsl`.
2. Stop anything else holding the console port (on the gateway, the collector). Only
   one program can read the port. A second reader gets half the bytes, and on this
   devkit every open of the port also reboots the C6 (rete#319).
3. Start custos on the cable. It holds the cable from start-up, with DTR and RTS low:

   ```bash
   ./custos --debug-port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_... \
            --debug-map nodus/oevm/zephyr/YourApp.oimg.map.json
   ```

   A board with this image waits at its boot stop until something answers it. The rest is
   §3 to §6, over the cable. A board held at a breakpoint is not rebooted as wedged: the
   observer knows the VM is held and restarts its clock when you continue.

From `c6spare`, 2026-10-05 21:48 MST, `DebugProbeC6.nsl` (a 1 s ticker whose ticks call
`countTo:` and `scaled:`, whose kept block captures `k` and `acc`), firmware built from
`93690a3` plus the probe's `scaled:`. This is the transcript (the page's Session pane
shows the same lines), captured with the command line of §9 and a script piped in with
pauses between commands. The first command arrived while the program was still counted
as running, so it was preceded by a pause (`dbg:ok pause`). Two such lines and ten tick
lines are left out; the rest is the log (`~/rete-bench/323/dbg-323-session.log` on the
desktop):

```
(dbg) break 63
-- break DebugProbeC6.nsl:63  DebugProbeC6_block3>>value:
dbg:ok break
(dbg) continue
dbg-probe: ticking
-- stopped (break) at DebugProbeC6.nsl:63  DebugProbeC6_block3>>value:
  63 |     add:: [ :x | acc:: acc + (x * k) ].
(dbg) bt
  #0 at DebugProbeC6.nsl:63  DebugProbeC6_block3>>value:
  #1 at DebugProbeC6.nsl:64  DebugProbeC6>>scaled:
  #2 at DebugProbeC6.nsl:47  DebugProbeC6>>tick
  #3 at DebugProbeC6.nsl:33  DebugProbeC6_block2>>value:
  #4 at kernel.nsl:3166  Kernel_Space>>drain
  #5 at DebugProbeC6.nsl:40  DebugProbeC6>>main:args:
  #6 at supervisor.nsl:38  Supervisor>>run:platform:args:debug:
(dbg) locals
-- frame 0 at DebugProbeC6.nsl:63  DebugProbeC6_block3>>value:
  self = [a block]
  x = 2
(dbg) locals 1
-- frame 1 at DebugProbeC6.nsl:64  DebugProbeC6>>scaled:
  self = an Object
  k = 1
  acc = 0
  add = [a block]
(dbg) clear 63
-- clear DebugProbeC6.nsl:63  DebugProbeC6_block3>>value:
dbg:ok clear
(dbg) continue
dbg-probe tick 1 sum 1 scaled 5
(dbg) pause
-- stopped (pause) at kernel.nsl:4070  Kernel>>pumpEvents
dbg:ok pause
(dbg) bt
  #0 at kernel.nsl:4070  Kernel>>pumpEvents
  #1 at DebugProbeC6.nsl:40  DebugProbeC6>>main:args:
  #2 at supervisor.nsl:38  Supervisor>>run:platform:args:debug:
(dbg) continue
dbg-probe tick 7 sum 28 scaled 35
```

**On a board, every bytecode step is a round trip over the cable** (about 0.2 s on the
C6), and a line step is many of them: the first `next` above took 3.6 s. `next` over a
long loop is better done with a breakpoint after it and `continue`.

The cable and the image are the whole requirement: no gateway, no mesh, no porta. The
measurements behind this section are `docs/results/2026-10-02-311-line-in-spike.md`
parts 2 and 3.

## 8. A profile

A profile has the same kind of agent path (rete#364). `--profile-host` starts the host VM
afresh for every capture, without its debugger, so the console feeds the kernel's
Profiler. The program must dispatch `sys/timer` and run for longer than the window.
Section 1's `average.nsl` is over before any window closes, so this example uses the
suite's ticking target:

```bash
./custos --profile-host "/tmp/oevm_img pause.oimg" --profile-map pause.oimg.map.json
curl -s -X POST 'localhost:6971/profile/capture?ms=200' | jq '.Report.Rows[:5]'
```

A capture answers 200 with the window ranked by method name. A program that ends before
its window closes answers 502 at once and says so; it does not wait out a timeout. On a
board the flag is `--profile-port`, and the page is `/profile`. The `custos-profile` gate
drives this path.

## 9. Also: the command line

`custos debug` and `custos profile` are kept, and still gated, but they are for a person
at a terminal, not for an agent. `custos debug` gives a prompt that takes §4's commands
and prints the transcript:

```bash
./custos debug -map average.oimg.map.json -- /tmp/oevm_img average.oimg
./custos debug -map nodus/oevm/zephyr/YourApp.oimg.map.json \
        -port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_...
```

Do not script it by piping it a file of commands. A command after `continue` arrives
while the program runs, and the pause rule (§5) stops the program wherever it has got
to; nothing on the command line can wait for a stop. That is the race the long-poll of §3
does not have.

`custos profile -map <name>.oimg.map.json` ranks a profile dump read from a log (or
stdin), and with `-port <tty> -ms N` captures one over the cable.

## Where next

- `nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md` — how the
  debugger works inside the image: the mirror, the Supervisor's command loop, the pause
  rule.
- `nodus/nsl/lib/supervisor.nsl` — the `dbg:*` verbs themselves, for when you want to
  talk to the VM without this tool.
