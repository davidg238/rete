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
`/tmp` (`/tmp/oevm_img`, `/tmp/oevm_layout.json`). `RETE` points at the clone.

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

`nodus/nsl/tools/dbg.py` starts the VM with its debugger on and gives you a prompt. You
can type at it, or pipe it a file of commands like the block below, which is what this
page's gate does:

```bash gate
cat > session.txt <<'EOF'
break 16
continue
step
next
list
finish
next
next
continue
EOF
python3 $RETE/nodus/nsl/tools/dbg.py --map average.oimg.map.json \
        -- /tmp/oevm_img average.oimg < session.txt | tee out.txt
grep -q '^-- stopped (break) at average.nsl:16  main:args:$' out.txt
grep -q '^-- stopped (step) at average.nsl:8  sum:$' out.txt
grep -q '^=>    9 |' out.txt
grep -q '^-- stopped (step) at average.nsl:18  main:args:$' out.txt
grep -q '^mean 4$' out.txt
grep -q '^-- the program ended$' out.txt
```

```
-- stopped at boot, before the program's first line
(dbg) break 16
-- break average.nsl:16  main:args:
dbg:ok break
(dbg) continue
-- stopped (break) at average.nsl:16  main:args:
  16 |     mean:: (self sum: xs) / xs size.
(dbg) step
-- stopped (step) at average.nsl:8  sum:
   8 |     total:: 0.
(dbg) next
-- stopped (step) at average.nsl:9  sum:
   9 |     xs do: [ :x | total:: total + x ].
(dbg) list
      6 |   sum: xs = (
      7 |     | total |
      8 |     total:: 0.
=>    9 |     xs do: [ :x | total:: total + x ].
     10 |     ^ total )
     11 |
     12 |   public main: platform args: args = (
(dbg) finish
-- stopped (step) at average.nsl:16  main:args:
  16 |     mean:: (self sum: xs) / xs size.
(dbg) next
-- stopped (step) at average.nsl:17  main:args:
  17 |     ('mean ' , mean printString) println.
(dbg) next
mean 4
-- stopped (step) at average.nsl:18  main:args:
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
| `inspect` | `i` | the frame's registers (§4) |
| `methods` | | the methods a breakpoint can go in, with their line ranges |
| `dbg:...` | | sent to the VM unchanged, e.g. `dbg:step` for one bytecode |
| `quit` | `q` | leave. The VM is told nothing more and runs on |

## 4. What it can and cannot do

- **A breakpoint goes on a line inside a method of the class the image runs** (`Average`
  here). The VM's list of methods (`methods`) is that class's own methods, then the
  kernel's `Object`. A line in another class of the same file, or inside a block that
  the compiler kept as a block (a `whenever: ... do: [ ... ]` handler, say), is refused
  with its name: `line 26 is in DebugPauseTarget_block2>>value:`. A block the compiler
  inlines, like the `do:` body on line 9, is part of its method and fine.
- **A line with no code of its own** (a method header, `| total |`, a comment) is
  refused with the nearest lines that have code: `line 7 has no code; the nearest lines
  that do: 8 (sum:)`. Inside a method, a line with no code moves forward to the next line
  that has some, and the tool tells you.
- **`step` and `next` move a line at a time** by sending the VM single-bytecode steps
  until the line changes. A loop on one line is many of those, so a long one takes a
  while. After 5000 the tool stops and says where it is (`LINE_STEP_LIMIT` in `dbg.py`).
- **A breakpoint does not fire during `next` or `finish`.** While the VM steps, the
  Supervisor decides each stop by frame depth alone and never checks the breakpoints. So
  `next` over a call passes a breakpoint inside that call, and `finish` out of a method
  that never returns (a `main:args:` that loops forever) leaves the program stepping:
  it runs on, a breakpoint never fires, and a typed line is not seen. `dbg:continue`
  (sent raw, or `continue`) ends the step. A line sent during a long `next` or `finish`
  lands when the step does (rete#321).
- **`inspect` shows registers, not names.** `dbg:stack off=3 r0=an Object r1=an Object
  r2=[a VM object] r3=nil r4=0 …`: `r0` is the receiver, then the arguments, then
  locals and temporaries, in frame order. The source map has no names for them yet.
- **A stop outside the app is reported as one**: `outside the registry (in a block or
  the kernel)`. A pause usually lands there, because a program that is waiting for its
  next event is waiting inside the kernel. So does `finish` out of `main:args:`: what
  called it is the kernel.

## 5. Stopping a running program

Typing anything while the program runs stops it at its next bytecode. That is the VM's
rule, not the tool's: the line arriving is what stops it. So `pause`, and also `break 37`
typed while it runs, both leave the program stopped, and `continue` resumes it. A program
that is asleep with no timer never reaches a bytecode, so nothing stops it until it
wakes up.

The suite's `debugger-integration` gate drives this through `dbg.py`, against
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
3. Attach:

   ```
   python3 nodus/nsl/tools/dbg.py --map nodus/oevm/zephyr/YourApp.oimg.map.json \
           --port /dev/serial/by-id/usb-Silicon_Labs_CP2102N_...
   ```

   Because opening the port reboots the board, you meet it at its boot stop. The rest is
   §2 and §3, over the cable. A board held at a breakpoint is not rebooted as wedged: the
   observer knows the VM is held and restarts its clock when you continue.

From `c6bench`, 2026-10-02 22:36 MST, `DebugProbeC6.nsl` (a 1 s ticker whose ticks call
`countTo:`), a script piped in with pauses between groups of commands. The boot lines,
`methods`, and the `where`/`list` and second `inspect` steps are left out; the rest is
the log (`~/rete-bench/311-fe/dbg-fe-session2.log` on the gateway):

```
-- stopped at boot, before the program's first line
(dbg) break 54
-- break DebugProbeC6.nsl:54  countTo:
dbg:ok break
(dbg) continue
dbg-probe: ticking
-- stopped (break) at DebugProbeC6.nsl:54  countTo:
  54 |     1 to: n do: [ :i | sum:: sum + i ].
(dbg) inspect
dbg:stack off=2 r0=an Object r1=1 r2=nil r3=0 r4=0 r5=nil r6=nil r7=nil r8=nil r9=nil
(dbg) next
-- stopped (step) at DebugProbeC6.nsl:55  countTo:
  55 |     ^ sum )
(dbg) next
-- stopped (step) at DebugProbeC6.nsl:46  tick
  46 |     sum:: self countTo: ticks.
(dbg) clear 54
-- clear DebugProbeC6.nsl:54  countTo:
dbg:ok clear
(dbg) continue
(dbg) pause
dbg-probe tick 1 sum 1
dbg-probe tick 2 sum 3
-- stopped (pause) outside the registry (in a block or the kernel), offset 2
dbg:ok pause
(dbg) continue
dbg-probe tick 3 sum 6
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
