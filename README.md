# ns-oevm — Newspeak on the Open Extensible VM

The Newspeak language frontend targeting **oevm** (Ian Piumarta's Open Extensible
Object Model, realised as a small register VM). This is the *way forward* for
running Newspeak on oevm; it supersedes the Newspeak→Berry path in `ns-berry`
(now legacy).

## The four repos

| Repo | Role |
|---|---|
| **oevm** | The bedrock: the C VM (dispatch, GC, object model). |
| **ns-oevm** | *(this)* Newspeak frontend → solidified C image for oevm. |
| **ns-berry** | Legacy Newspeak→Berry compiler. Source of the shared grammar. |
| **ns-tests** | Black-box conformance + benchmark suite (ported from SOMns). |

`ns-oevm` carries its **own copy** of the Ohm grammar (`grammar/`, copied out of
ns-berry) so it does not depend on the legacy repo. At runtime it expects
`oevm` and `ns-tests` as sibling checkouts:

```
workspace/
  oevm/        # the VM (built by the test runner)
  ns-oevm/     # this repo
  ns-tests/    # the .ns conformance suite
```

## Layout

```
grammar/   Ohm grammar (ns.ohm) + AST builder (ns-parse.js) + node tests
tools/     ns_compile_oevm.py  — .ns → oevm solidified C image
           run_tests_oevm.py   — black-box harness over ../ns-tests
           solidify.py         — image solidification helpers
```

## Usage

```bash
# one-time: install the grammar's node deps
cd grammar && npm install && cd ..

# compile one .ns file into the oevm image and run the suite
python3 tools/run_tests_oevm.py          # uses ../oevm and ../ns-tests
OEVM_DIR=/path/to/oevm python3 tools/run_tests_oevm.py   # explicit VM location

# grammar regression gate
cd grammar && node --test test/
```

## Status (2026-06-19)

Against `ns-tests`: **47 / 47 passing** (1 skipped — `scratch_class.ns`, no
expectations). The suite runs on **both execution paths** by default
(`OEVM_PATH=AB`): Path A (solidified-C image) and Path B (`.oimg` native-layout
image-VM).

- ✅ expressions (4/4), control flow (3/3), blocks (4/4)
- ✅ SOMns basics — all 23, incl. `super_sends`, `outer_sends`, class-side
  methods, float literals, nested-class slot init, grandparent init
- ✅ benchmarks — all 7: `queens`, `sieve`, `towers`, `mandelbrot`, `nbody`,
  `richards`, `deltablue`
- ✅ limit tests — all 5, incl. `integer_overflow` (bignum) and `gc_stress`

Every gap the earlier status listed as blocking is closed: implicit/`super`
sends, nested-class outer-sends, class-body initializer statements, class-side
method sections, `Array>>do:`, the full block-inlining stack (if-family,
`and:`/`or:`, loops), the `towers`/`mandelbrot` runtime bugs, and bignum /
integer-overflow semantics.

The compute core (OEOM dispatch, closures, NLR, inline caching, field layout) is
working and produces correct results on both image paths.

The block-inlining track is complete (Phases A–E all landed; see
`docs/superpowers/plans/block-inlining-status.md`).

### Remaining work

- **Closure representation** (tracked as `oevm#1`) — replace the reflective
  `home`/`regAt:` outer-variable access for non-inlined blocks with a direct
  outer-frame pointer + indexed access, on top of the scope/frame model.
