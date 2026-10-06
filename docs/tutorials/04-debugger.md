# Tutorial 4 — the debugger: stop a program at a line, step it, look inside

A debugger for nsl programs, on your desktop and on an ESP32-C6 over its console cable.
You name a line of your `.nsl` file; the program stops there and shows you the line.
From there you can step, look at the frame, and let it run. A program that is already
running can also be stopped where it is.

**The desktop half of this page is executed.** Every block marked `bash gate` is run, in
order, in a fresh empty directory, by `docs/tutorials/gate.sh`. The nsl suite runs it as
its `tutorial-debugger` aux gate, so a stale step here turns the suite red. The board half
(§6) needs a board and is prose.

## 0. What you need

Tutorial 1 done, or the suite run once. Either way the VM and its layout file are in
`/tmp` (`/tmp/oevm_img`, `/tmp/oevm_layout.json`). `RETE` points at the clone. The
debugger is a verb of `custos`, the developer's tool, so you also need Go (the suite
SKIPs this page, by name, without it).

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

## 2. A session

`custos debug` starts the VM with its debugger on and gives you a prompt. You can type
at it, or pipe it a file of commands like the block below, which is what this page's
gate does. First build custos:

```bash gate
go build -C "$RETE/custos" -o "$PWD/custos" ./cmd/custos
```

```bash gate
cat > session.txt <<'EOF'
break 16
continue
step
next
bt
locals
locals 1
list
finish
next
next
continue
EOF
./custos debug -map average.oimg.map.json \
        -- /tmp/oevm_img average.oimg < session.txt | tee out.txt
grep -q '^-- stopped (break) at average.nsl:16  Average>>main:args:$' out.txt
grep -q '^-- stopped (step) at average.nsl:8  Average>>sum:$' out.txt
grep -q '^  #1 at average.nsl:16  Average>>main:args:$' out.txt
grep -q '^  total = 0$' out.txt
grep -q '^  mean = nil$' out.txt
grep -q '^=>    9 |' out.txt
grep -q '^-- stopped (step) at average.nsl:18  Average>>main:args:$' out.txt
grep -q '^mean 4$' out.txt
grep -q '^-- the program ended$' out.txt
```

```
-- stopped at boot, before the program's first line
(dbg) break 16
-- break average.nsl:16  Average>>main:args:
dbg:ok break
(dbg) continue
-- stopped (break) at average.nsl:16  Average>>main:args:
  16 |     mean:: (self sum: xs) / xs size.
(dbg) step
-- stopped (step) at average.nsl:8  Average>>sum:
   8 |     total:: 0.
(dbg) next
-- stopped (step) at average.nsl:9  Average>>sum:
   9 |     xs do: [ :x | total:: total + x ].
(dbg) bt
  #0 at average.nsl:9  Average>>sum:
  #1 at average.nsl:16  Average>>main:args:
  #2 at supervisor.nsl:38  Supervisor>>run:platform:args:debug:
(dbg) locals
-- frame 0 at average.nsl:9  Average>>sum:
  self = an Object
  xs = an Object
  total = 0
(dbg) locals 1
-- frame 1 at average.nsl:16  Average>>main:args:
  self = an Object
  platform = an Object
  args = 0
  xs = an Object
  mean = nil
(dbg) list
      6 |   sum: xs = (
      7 |     | total |
      8 |     total:: 0.
=>    9 |     xs do: [ :x | total:: total + x ].
     10 |     ^ total )
     11 |
     12 |   public main: platform args: args = (
(dbg) finish
-- stopped (step) at average.nsl:16  Average>>main:args:
  16 |     mean:: (self sum: xs) / xs size.
(dbg) next
-- stopped (step) at average.nsl:17  Average>>main:args:
  17 |     ('mean ' , mean printString) println.
(dbg) next
mean 4
-- stopped (step) at average.nsl:18  Average>>main:args:
  18 |     ^ self )
(dbg) continue
-- the program ended
```

The program waits before its first line: a debug run always does, so you can set
breakpoints first. Lines starting `--` are the tool talking. Lines starting `dbg:` are
the VM's own answers, passed through. Everything else is your program's output.

## 3. The commands

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
| `locals [N]` | | frame N's `self`, arguments, temporaries and captured locals by name (§4) |
| `inspect` | `i` | the frame's raw registers |
| `methods` | | every method of the app's file, with its line range |
| `dbg:...` | | sent to the VM unchanged, e.g. `dbg:step` for one bytecode |
| `quit` | `q` | leave. The VM is told nothing more and runs on |

## 4. What it can and cannot do

- **A breakpoint goes on any line with code in the app's file**: a method of any of its
  classes, or a line inside a block the compiler kept as a block (a `whenever: ... do:
  [ ... ]` handler, say), which is its own method. The VM names every method by its offset
  in the image, and the image map turns that offset into a name and a line (rete#323).
- **A line with no code of its own** (a method header, `| total |`, a comment) is
  refused with the nearest lines that have code: `line 7 has no code; the nearest lines
  that do: 8 (sum:)`. Inside a method, a line with no code moves forward to the next line
  that has some, and the tool tells you.
- **`step` and `next` move a line at a time** by sending the VM single-bytecode steps
  until the line changes. A loop on one line is many of those, so a long one takes a
  while. After 5000 the tool stops and says where it is (`LineStepLimit` in
  `custos/internal/debug/session.go`).
- **A breakpoint does not fire during `next` or `finish`.** While the VM steps, the
  Supervisor decides each stop by frame depth alone and never checks the breakpoints. So
  `next` over a call passes a breakpoint inside that call, and `finish` out of a method
  that never returns (a `main:args:` that loops forever) leaves the program stepping:
  it runs on, a breakpoint never fires, and a typed line is not seen. `dbg:continue`
  (sent raw, or `continue`) ends the step. A line sent during a long `next` or `finish`
  lands when the step does (rete#321).
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

Typing anything while the program runs stops it at its next bytecode. That is the VM's
rule, not the tool's: the line arriving is what stops it. So `pause`, and also `break 37`
typed while it runs, both leave the program stopped, and `continue` resumes it. A program
that is asleep with no timer never reaches a bytecode, so nothing stops it until it
wakes up.

The suite's `custos-debug` gate drives this through `custos debug`, against
`nodus/tests/04_somns_basic/47_debug_pause_target.nsl`, a program that ticks on a 200 ms
timer: pause after `tick 1`, before `tick 2`.

## 6. On a board (ESP32-C6, one USB cable)

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
3. Attach (custos built as in §2; it opens the port with DTR and RTS held low):

   ```
   ./custos debug -map nodus/oevm/zephyr/YourApp.oimg.map.json \
           -port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_...
   ```

   A board with this image waits at its boot stop until something answers it. The rest is
   §2 and §3, over the cable. A board held at a breakpoint is not rebooted as wedged: the
   observer knows the VM is held and restarts its clock when you continue.

From `c6spare`, 2026-10-05 21:48 MST, `DebugProbeC6.nsl` (a 1 s ticker whose ticks call
`countTo:` and `scaled:`, whose kept block captures `k` and `acc`), firmware built from
`93690a3` plus the probe's `scaled:`, a script piped in with pauses between commands. The
first command arrived while custos still counted the program as running, so it was preceded
by a pause (`dbg:ok pause`). Two such lines and ten tick lines are left out; the rest is the
log (`~/rete-bench/323/dbg-323-session.log` on the desktop):

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
long loop is better done with a breakpoint after it and `continue`. In a script, leave
time after `continue` for the program to reach its breakpoint: the next line typed
stops it wherever it has got to.

The cable and the image are the whole requirement: no gateway, no mesh, no porta. The
measurements behind this section are `docs/results/2026-10-02-311-line-in-spike.md`
parts 2 and 3.

## Where next

- `nodus/nsl/docs/design/2026-06-23-ns-oevm-in-image-debugger-design.md` — how the
  debugger works inside the image: the mirror, the Supervisor's command loop, the pause
  rule.
- `nodus/nsl/lib/supervisor.nsl` — the `dbg:*` verbs themselves, for when you want to
  talk to the VM without this tool.
