# Tutorial 1 — the VM on your host: build it, run the suite, write ten lines of nsl

No board, no radio. By the end you will have built oevm, run the whole suite, written
two programs and run them, compiled one by hand to see what the compiler emits, and
watched the compiler refuse the one thing this language does not allow.

**This page is executed.** Every block marked `bash gate` below is extracted and run, in
order, in a fresh empty directory, by `docs/tutorials/gate.sh` — which the nsl suite
runs as its `tutorial-host` aux gate. If a command here stops working, the suite goes
red. That is the only reason to trust a tutorial that is more than a week old.

## 0. What you need

- Linux, `gcc`, `python3` (3.9+), `node` (18+; the grammar is Ohm, in JavaScript).
- This repository, cloned. Everything below assumes `RETE` points at the clone:

```bash
export RETE=~/workspace/rete        # wherever you cloned it
cd $RETE/nodus/nsl/grammar && npm install && cd -    # once: the parser's node deps
```

## 1. Run the suite first

The suite builds the VM from source (there is no Makefile — the runner *is* the build)
and runs every test on the path that ships, the `.oimg` image path:

```bash
cd $RETE/nodus/nsl && python3 tools/run_tests_oevm.py
```

About four minutes. The last lines are what you read:

```
Test Execution Summary:
  Passed:  278
  Failed:  0
  Skipped: 14
  c-units: PASS  grant: PASS  debugger-integration: PASS  portability-32: PASS  ...
  mprof-tools-py: PASS  ...  tutorial-host: PASS  tutorial-porta: PASS
```

Two things matter: `Failed: 0`, **and the aux line under it** — it can be red while the
count reads zero, and it is where the C unit tests, the footprint ratchet and this very
page are scored. The `Passed` number only ever drifts upward as tests land.

The skips are structural, not a to-do list: the benchmarks under `tests/00_perf/` declare
no expected output because they are timed rather than asserted, and the rest are marked
`skip-oevm` because they exercise fibers, growable stacks and non-local return — three
features this VM had, measured, and deleted (§5).

## 2. Ten lines of nsl

Work in an empty directory. A program is a file ending in `.nsl`; its expected output
goes at the top in a comment, which is how every test in `nodus/tests/` is written —
there are no `.expected` files.

```bash gate
cat > hello.nsl <<'EOF'
(*
expect:
7
hello from nsl
*)
(3 + 4) println.
'hello from nsl' println
EOF
```

Run it on the image path — the `B` is not optional, see below:

```bash gate
python3 $RETE/nodus/nsl/tools/run-one.py hello.nsl B | tee out.txt
grep -q '^Path B: PASS' out.txt
```

```
Path B: PASS  Output matches expectations.
```

What just happened: the Ohm grammar parsed the file to an AST (`node`), the compiler
(`ns_compile_oevm.py`) emitted an `.oimg` object-graph image, the runner built the VM
with `gcc` if it had not already, and `oevm_img` loaded the image and sent it its
entry message. Everything is in `/tmp` (`OEVM_BUILD_DIR` to move it).

**Why `B`.** `A` is the appliance path — it solidifies the image *to C* and links it
into the VM, and doing that rewrites a tracked file, `nodus/oevm/src/solidified_image.c`.
Fine for the suite, which restores it; wrong for you. `B` is what a board runs.

## 3. A class, a slot, a block

nsl is Newspeak-shaped: a class has a slot section, a method section, and a class-side
section; keyword messages read left to right; blocks are `[ :arg | … ]`. The kernel
hands your class its `platform` when it sends `main:args:`.

```bash gate
cat > counter.nsl <<'EOF'
(*
expect:
bumped 3
squares 1 4 9
*)
class Counter packageUsing: manifest = (
  | count ::= 0. |
) (
  bump = ( count:: count + 1. ^ count )

  public main: platform args: args = (
    | line |
    1 to: 3 do: [ :i | self bump ].
    ('bumped ' , count printString) println.
    line:: 'squares'.
    1 to: 3 do: [ :i | line:: line , ' ' , (i * i) printString ].
    line println.
    ^ self )
) : ( )
EOF
python3 $RETE/nodus/nsl/tools/run-one.py counter.nsl B | tee out.txt
grep -q '^Path B: PASS' out.txt
```

Things to notice, because they differ from Smalltalk: assignment is `::` (`count::
count + 1`), a slot with an initialiser is `count ::= 0.`, `main:args:` is `public`
because everything else is private to the class, and `^ self` at the end is the
convention (a method's value is otherwise `self` anyway).

## 4. The same thing by hand

`run-one.py` hid two steps. Do them yourself — the VM and the layout file it built in
step 2 are still in `/tmp`:

```bash gate
python3 $RETE/nodus/nsl/tools/ns_compile_oevm.py counter.nsl counter.oimg \
        --format=oimg --layout=/tmp/oevm_layout.json
/tmp/oevm_img counter.oimg | tee out.txt
grep -q '^bumped 3$' out.txt
ls -l counter.oimg counter.stmap
```

```
OIMG written to counter.oimg (216288 bytes, entry=Counter)
bumped 3
squares 1 4 9
```

216 KB for a counter. **The kernel is in the image**: `nodus/nsl/lib/kernel.nsl` — the
space, channels, timers, the CBOR codec, the porta bridge — is nsl, and it is compiled
into every `.oimg` beside your class. Your program is a few hundred bytes of it. The
`.stmap` beside the image is the source map, bytecode offset → line, which the crash
reports and the profiler use to name your code.

`--layout` is the one thing that ties an image to a VM build: the object layout the VM
was compiled with, captured as JSON. A board target has its own, and an image built
against the wrong one is refused by name rather than loaded.

## 5. The thing you cannot write

There is no non-local return. A `^` inside a block that escapes its method — the
classic Smalltalk early-exit from `do:` — is a **compile error**, not a runtime one:

```bash gate
cat > nlr.nsl <<'EOF'
class Escape packageUsing: manifest = () (
  find: n in: xs = (
    xs do: [ :x | x = n ifTrue: [ ^ x ] ].
    ^ nil )
  public main: platform args: args = ( ^ self )
) : ( )
EOF
if python3 $RETE/nodus/nsl/tools/ns_compile_oevm.py nlr.nsl nlr.oimg \
        --format=oimg --layout=/tmp/oevm_layout.json > out.txt 2>&1; then
  echo "expected a compile error"; exit 1
fi
grep -q 'non-local return' out.txt
```

```
Compile error: nlr.nsl:3:35: non-local return: '^' inside an escaping closure is not
allowed (no-NLR model). Use a collection combinator or a sentinel-and-return.
  in Escape_block1>>value:
  3 |     xs do: [ :x | x = n ifTrue: [ ^ x ] ].
```

The VM had NLR, and fibers, and growable stacks; all three were built, measured and
deleted (`nodus/oevm/docs/MILESTONE_A.md`). What is left runs every block to completion, which is
what lets a reaction on a board be judged alive or wedged by whether it finished.
`nodus/oevm/docs/vm.md` is the one-page account.

## Where next

- `nodus/nsl/docs/programming_with_nsl.md` — the task-oriented guide: the mental model,
  state machines as the fiber replacement, channels, capabilities.
  `nodus/nsl/docs/NEWSPEAK_DIALECT.md` — every place nsl diverges from Newspeak, and why.
- `nodus/tests/` — the suite, one directory per language area, every file in the shape
  of step 2. When the guide and a test disagree, the test is right.
- `nodus/nsl/lib/kernel.nsl` — what `platform` can do: `space`, `timers`,
  `whenever:do:`, the porta bridge.
- `docs/tutorials/03-porta.md` — tutorial 3: the gateway half. A `porta serve` and a real
  node talking to each other over loopback UDP, and what "convergence is a tree diff"
  actually looks like at a prompt. Tutorial 2 (a board) is not written yet; until it is,
  `nodus/oevm/zephyr/run.sh` and `nodus/oevm/tests/target/` are the executable
  documentation.
