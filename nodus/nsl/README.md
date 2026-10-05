# nsl — the dialect, its compiler and the kernel

nsl is a Newspeak-derived language for the oevm VM (`../oevm/`): keyword messages,
blocks, nested classes with implicit-receiver sends, modules as objects. Source files end
in `.nsl`. It compiles **on the host** to an `.oimg` object-graph image that the flashed
VM loads and executes in place — there is no compiler on the device.

What the language does **not** have, on purpose: non-local return (`^` out of an escaping
closure is a compile error), fibers, growable stacks. Execution is run-to-completion over
a reactor; see `../oevm/docs/vm.md`.

## Layout

    grammar/   the Ohm grammar (ns.ohm) and AST builder (ns-parse.js), with node tests
    lib/       kernel.nsl — the kernel IS nsl: space, channels, timers, CBOR, the porta bridge
               supervisor.nsl — the let-it-crash supervisor
    tools/     ns_compile_oevm.py   .nsl → .oimg (+ its .oimg.map.json image map)
               run_tests_oevm.py   THE gate: the whole suite, the C unit tests, the aux gates
               run-one.py          one test file: run-one.py <file.nsl> B
               oimg_serialize.py, gen_builtins.py, image_map.py, and their test_*.py

The suite itself lives in `../tests/`, one directory per area. A test is a `.nsl` file
with its expected output inline at the top (`(* expect: … *)`), a class with
`main:platform args:`, and `println`.

## Running

    cd grammar && npm install && cd ..          # once
    python3 tools/run_tests_oevm.py             # builds the VM, runs everything
    python3 tools/run-one.py ../tests/<dir>/<file>.nsl B

Read the last lines: `Passed: N / Failed: 0` **and** the aux-gate line after it. The aux
line can be red while the count reads zero failures. `B` means the `.oimg` path, which is
the one that ships; the solidify-to-C path (`A`) is the appliance build and rewrites a
tracked fixture if you run it by accident.

An app's `platform` is the kernel instance, not the VM — most VM builtins are reachable
only through it. The gotchas that cost sessions are collected in the Claude memory
namespace for this repo and in `../../CLAUDE.md`; the grammar's own answer to "is this
legal?" is `grammar/test/`.
