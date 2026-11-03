# Kind+Schema Manifest (#243) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `PROTOCOL-NSL-RECORDS.md`'s catalogue machine-readable as `protocol/MANIFEST.cddl` and enforce it at every gate — build, test, and porta ingest — so kind/shape divergence is a loud error instead of a silent field incident.

**Architecture:** One annotated stock-CDDL file is the contract. A Python parser/lint (`protocol/tools/`) and zcbor validate it at build and test time; a ~100-line Go parser (`porta/internal/manifest`) compiles it to a check table enforced in `Store.Write`, `Store.WriteGateway` and `Store.Ingest`; the pin (SHA-256 of file bytes) is stamped into the #256 archive, baked into the image beside `buildId`, and published as `obs/build.man`.

**Tech Stack:** Python 3 (stdlib + zcbor 0.9.1), Go 1.26 (stdlib + existing cval), CMake/bash on the Zephyr side, nsl for the node.

**Spec:** `docs/design/2026-08-24-kind-schema-manifest-design.md` (read it first; this plan implements it and records four addenda in Task 11 where the code's reality refined it).

## Global Constraints

- Copyright header `// Copyright (c) 2026 Ekorau LLC` on every new Go file; `gofmt -l .` prints nothing before every commit (run inside `porta/`).
- Kind numbers are shared with the VM: `stream = 1`, `cell = 2` (`cval.KindStream/KindCell`, `SPACE_KIND_*` in `nodus/oevm/src/space.h`). Never renumber.
- zcbor pins, probed 2026-08-24: **zcbor==0.9.1**, **cbor2<6** (cbor2 6.x breaks zcbor's import), `regex` required; CBOR input must be **piped via stdin** (`-i -`) around 0.9.1's file-open bug; a pipeline's verdict is **zcbor's exit code, never the pipe's** (`${PIPESTATUS[0]}`, or avoid pipes).
- Never run two nsl suites concurrently (shared `/tmp` paths). Single tests: `nodus/nsl/tools/run-one.py <file.nsl> B` — always pass `B`.
- **Never call `ns_compile_oevm.py` without an explicit output argument** — the default output overwrites the tracked `src/solidified_image.c`.
- nsl gotchas that will bite here: a wrong-arity class-side send silently answers an uninitialised instance (grep the constructor arity before using it); a prim-built value needs `== nil`, never `isNil`; symbols print with `#` but `at:`/`size` read the bare name.
- Commit style: `type(#243): summary`, e.g. `feat(#243): ...`, ending with the Co-Authored-By/Claude-Session trailer used by this session's earlier commits (`git log -2` shows it).
- The manifest is **closed**: every check added here refuses what it cannot match. Matching is segment-wise; `<var>` = exactly one segment; a literal entry **shadows** a wildcard entry (legal); two overlapping **wildcard** entries are a lint error.
- Directions: `up` = node writes (wire push), `down` = porta writes (Write / `porta put`), `gw` = gateway-authored verdicts (`WriteGateway`, never on the wire).

## The manifest row table (the content Task 2 authors)

Every row below has its shape already pinned in `protocol/PROTOCOL-NSL-RECORDS.md` §4 (field lists there are normative for Task 2; abbreviations are post-#197). `goal/forward`/`obs/forward` are **excluded** — `Forward {…}` has no pinned shape; writing it first requires pinning it (Task 11 adds that note to the catalogue).

| path | rule | kind | class | dir |
|---|---|---|---|---|
| `goal/name` | `name-cell = tstr` | cell | routine | down |
| `goal/apps/<name>` | `app` (App; nested `blobref`) | cell | routine | down |
| `goal/mode` | `mode` (Mode) | cell | routine | down |
| `goal/config/<app>/<key>` | `config-cell = scalar` | cell | routine | down |
| `goal/debug/<app>` | `debug` (Debug; action `#attach/#detach`) | cell | routine | down |
| `goal/profile/<app>` | `profile` (Profile) | cell | routine | down |
| `goal/liveness` | `liveness` (Liveness) | cell | routine | down |
| `goal/do/reboot` | `reboot` (Reboot, empty map) | stream | routine | down |
| `dbg/req` | `dbg-req` (DbgReq) | stream | routine | down |
| `obs/name` | `name-cell` | cell | routine | up |
| `obs/mode` | `mode` | cell | routine | up |
| `obs/apps/<name>` | `app-obs` (AppObs; most fields `?`) | cell | routine | up |
| `obs/config/<app>/<key>` | `config-cell` | cell | routine | up |
| `obs/health` | `health` (Health: up,bid,wak,pto,sdr,ref) | cell | routine | up |
| `obs/build` | `build` (Build: chip,build,?sdk,?image,**?man**) | cell | routine | up |
| `obs/liveness` | `liveness` | cell | routine | up |
| `tel/print` | `print` (Print) | stream | drop-first | up |
| `tel/log` | `log` (Log; level enum) | stream | drop-first | up |
| `tel/m/<name>` | `metric` (Metric: value int/float, monoMs) | stream | drop-first | up |
| `sys/reset` | `reset` (Reset: reason sym, code int, bootId) | stream | routine | up |
| `sys/overrun` | `overrun` (Overrun) | stream | routine | up |
| `sys/panic` | `panic` (Panic; reason enum; core blobref/nil) | stream | must-deliver | up |
| `sys/metrics` | `metrics` (Metrics, 12 uint fields) | cell | routine | up |
| `sys/cheap` | `cheap` (CHeap, 5 uint fields) | cell | routine | up |
| `sys/latency` | `latency-span` (LatencySpan, 13 fields) | cell | routine | up |
| `dbg/resp` | `dbg-resp` (DbgResp) | stream | routine | up |
| `diag/spans/ctl` | `span-ctl` (SpanCtl; state `#running/#stopped`) | cell | routine | up |
| `diag/spans/result` | `span-capture` (SpanCapture: n uint, ms **int** — −1 legal) | cell | routine | up |
| `diag/spans/<span>` | `latency-span` | cell | routine | up |
| `gw/liveness` | `gw-verdict = #6.39("live") / #6.39("silent")` | cell | routine | gw |

Shared-shape rows (`mode`, `liveness`, `name-cell`, `config-cell`, `latency-span`) get **stacked annotations** — several `;#` lines above one rule.

---

### Task 1: `protocol/tools/manifestlib.py` — the shared parser

**Files:**
- Create: `protocol/tools/manifestlib.py`
- Test: `protocol/tools/test_manifestlib.py`

**Interfaces:**
- Produces: `parse(text: str) -> Manifest` where `Manifest` has `.entries` (list of `Entry(path_pattern: list[str], kind: str, cls: str, dir: str, rule: str)`), `.rules` (dict name → body text), `.syms` (dict rule-name → literal string, from `sym-* = #6.39("...")` rules), and `Manifest.match(path: str) -> Entry | None` (literal-shadows-wildcard). Raises `ManifestError(msg)` on malformed input. Pure stdlib.

- [ ] **Step 1: Write the failing tests**

```python
# protocol/tools/test_manifestlib.py
import pytest
from manifestlib import parse, ManifestError

SAMPLE = """\
sym-Health = #6.39("Health")
sym-up = #6.39("up")

;# path=obs/health  kind=cell  class=routine  dir=up
health = #6.27([sym-Health, { sym-up => uint }])

;# path=goal/config/<app>/<key>  kind=cell  class=routine  dir=down
;# path=obs/config/<app>/<key>  kind=cell  class=routine  dir=up
config-cell = scalar

;# path=diag/spans/ctl  kind=cell  class=routine  dir=up
span-ctl = #6.27([sym-Health, { sym-up => uint }])

;# path=diag/spans/<span>  kind=cell  class=routine  dir=up
latency-span = #6.27([sym-Health, { sym-up => uint }])

scalar = int / tstr / bool / #6.39(tstr)
"""

def test_parse_entries_and_stacked_annotations():
    m = parse(SAMPLE)
    assert len(m.entries) == 5  # config-cell carries TWO stacked annotations
    e = m.match("obs/health")
    assert e.rule == "health" and e.kind == "cell" and e.dir == "up"
    assert m.match("obs/config/a/k").rule == "config-cell"
    assert m.match("obs/config/a/k").dir == "up"
    assert m.match("goal/config/a/k").dir == "down"

def test_wildcard_matches_one_segment_only():
    m = parse(SAMPLE)
    assert m.match("goal/config/app1/key1").rule == "config-cell"
    assert m.match("goal/config/app1") is None
    assert m.match("goal/config/app1/key1/extra") is None

def test_literal_shadows_wildcard():
    m = parse(SAMPLE)
    assert m.match("diag/spans/ctl").rule == "span-ctl"
    assert m.match("diag/spans/pullSend").rule == "latency-span"

def test_unmatched_path_is_none():
    assert parse(SAMPLE).match("bogus/path") is None

def test_annotation_without_rule_raises():
    with pytest.raises(ManifestError):
        parse(";# path=a/b kind=cell class=routine dir=up\n\nsym-x = #6.39(\"x\")\n")

def test_bad_vocab_raises():
    with pytest.raises(ManifestError):
        parse(";# path=a/b kind=blob class=routine dir=up\nr = tstr\n")

def test_syms_extracted():
    assert parse(SAMPLE).syms["sym-Health"] == "Health"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd ~/workspace/rete/protocol/tools && python3 -m pytest test_manifestlib.py -q`
Expected: FAIL / collection error — `manifestlib` not found.

- [ ] **Step 3: Implement `manifestlib.py`**

```python
# protocol/tools/manifestlib.py
"""Parser for protocol/MANIFEST.cddl's restricted subset + ;# annotations.
Shared by manifest_lint.py and the test gates. Stdlib only."""
import re
from dataclasses import dataclass

class ManifestError(Exception):
    pass

ANNOT_RE = re.compile(r"^;#\s+(.*)$")
SYM_RE = re.compile(r'^(sym-[A-Za-z0-9-]+)\s*=\s*#6\.39\("([^"]+)"\)\s*$')
RULE_RE = re.compile(r"^([a-z][a-z0-9-]*)\s*=\s*(.*)$")
KINDS = {"cell", "stream"}
CLASSES = {"routine", "must-deliver", "drop-first"}
DIRS = {"up", "down", "gw"}

@dataclass
class Entry:
    path_pattern: list  # segments; "<...>" is a one-segment wildcard
    kind: str
    cls: str
    dir: str
    rule: str

class Manifest:
    def __init__(self, entries, rules, syms):
        self.entries, self.rules, self.syms = entries, rules, syms
        self._literal = {}
        self._wild = []
        for e in entries:
            if any(s.startswith("<") for s in e.path_pattern):
                self._wild.append(e)
            else:
                self._literal["/".join(e.path_pattern)] = e

    def match(self, path):
        if path in self._literal:
            return self._literal[path]
        segs = path.split("/")
        for e in self._wild:
            if len(e.path_pattern) != len(segs):
                continue
            if all(p.startswith("<") or p == s
                   for p, s in zip(e.path_pattern, segs)):
                return e
        return None

def _parse_annot(line, lineno):
    fields = dict()
    for tok in line.split():
        if "=" not in tok:
            raise ManifestError(f"line {lineno}: malformed annotation token {tok!r}")
        k, v = tok.split("=", 1)
        fields[k] = v
    for req in ("path", "kind", "class", "dir"):
        if req not in fields:
            raise ManifestError(f"line {lineno}: annotation missing {req}=")
    if fields["kind"] not in KINDS:
        raise ManifestError(f"line {lineno}: kind {fields['kind']!r} not in {sorted(KINDS)}")
    if fields["class"] not in CLASSES:
        raise ManifestError(f"line {lineno}: class {fields['class']!r} not in {sorted(CLASSES)}")
    if fields["dir"] not in DIRS:
        raise ManifestError(f"line {lineno}: dir {fields['dir']!r} not in {sorted(DIRS)}")
    return fields

def parse(text):
    entries, rules, syms = [], {}, {}
    pending = []  # annotations awaiting their rule
    # Continuation lines: a rule body runs until brackets balance.
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].rstrip()
        i += 1
        m = ANNOT_RE.match(line)
        if m:
            pending.append((_parse_annot(m.group(1), i), i))
            continue
        if not line or line.startswith(";"):
            if line.startswith(";") or not pending:
                continue
            continue  # blank lines between stacked annotations are fine
        m = SYM_RE.match(line)
        if m:
            if pending:
                raise ManifestError(f"line {i}: annotation may not bind a sym-* rule")
            syms[m.group(1)] = m.group(2)
            continue
        m = RULE_RE.match(line)
        if not m:
            raise ManifestError(f"line {i}: not a rule, annotation, or comment: {line!r}")
        name, body = m.group(1), m.group(2)
        depth = body.count("[") + body.count("{") + body.count("(") \
              - body.count("]") - body.count("}") - body.count(")")
        while depth > 0 and i < len(lines):
            nxt = lines[i]; i += 1
            body += " " + nxt.strip()
            depth += nxt.count("[") + nxt.count("{") + nxt.count("(") \
                   - nxt.count("]") - nxt.count("}") - nxt.count(")")
        rules[name] = body
        for fields, lineno in pending:
            entries.append(Entry(fields["path"].split("/"), fields["kind"],
                                 fields["class"], fields["dir"], name))
        pending = []
    if pending:
        raise ManifestError(f"line {pending[0][1]}: annotation has no rule beneath it")
    return Manifest(entries, rules, syms)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd ~/workspace/rete/protocol/tools && python3 -m pytest test_manifestlib.py -q`
Expected: all PASS. Fix `test_parse_entries_and_stacked_annotations` if its awkward sorted-comparison needs cleanup — assert on `m.match(...)` results instead; the behavior, not the listing, is the contract.

- [ ] **Step 5: Commit**

```bash
cd ~/workspace/rete && git add protocol/tools/manifestlib.py protocol/tools/test_manifestlib.py
git commit -m "feat(#243): manifestlib — the annotated-CDDL parser both gates share"
```

---

### Task 2: `protocol/MANIFEST.cddl` + `protocol/tools/manifest_lint.py`

**Files:**
- Create: `protocol/MANIFEST.cddl`, `protocol/tools/manifest_lint.py`
- Test: `protocol/tools/test_manifest_lint.py`

**Interfaces:**
- Consumes: `manifestlib.parse` / `Manifest` from Task 1.
- Produces: `lint(text: str) -> list[str]` (empty = clean) and a CLI (`python3 manifest_lint.py ../MANIFEST.cddl`, exit 1 with findings on stderr). And the manifest file itself — the contract every later task reads.

- [ ] **Step 1: Write the failing lint tests**

```python
# protocol/tools/test_manifest_lint.py
from pathlib import Path
from manifest_lint import lint

GOOD = Path(__file__).resolve().parents[1] / "MANIFEST.cddl"

def test_real_manifest_is_clean():
    assert lint(GOOD.read_text()) == []

def test_wildcard_overlap_is_error():
    text = GOOD.read_text() + '\n;# path=diag/spans/<x>  kind=cell  class=routine  dir=up\ndup-span = tstr\n'
    assert any("overlap" in f for f in lint(text))

def test_unreferenced_sym_is_error():
    text = GOOD.read_text() + '\nsym-orphan = #6.39("orphan")\n'
    assert any("orphan" in f for f in lint(text))

def test_undefined_sym_reference_is_error():
    text = GOOD.read_text() + '\n;# path=x/y  kind=cell  class=routine  dir=up\nbad = #6.27([sym-Nope, { sym-up => uint }])\n'
    assert any("sym-Nope" in f for f in lint(text))
```

- [ ] **Step 2: Run to verify failure** — `python3 -m pytest test_manifest_lint.py -q` → import error / missing file.

- [ ] **Step 3: Implement the lint**

```python
# protocol/tools/manifest_lint.py
"""Lint for protocol/MANIFEST.cddl (design §2.2). Errors, never warnings."""
import re, sys
from manifestlib import parse, ManifestError

def _overlaps(a, b):
    # Two WILDCARD patterns overlap iff same length and every position is
    # literal-equal or at least one side is a wildcard. Literal-vs-wildcard
    # is legal shadowing, checked by the caller.
    if len(a) != len(b):
        return False
    return all(x.startswith("<") or y.startswith("<") or x == y
               for x, y in zip(a, b))

def lint(text):
    findings = []
    try:
        m = parse(text)
    except ManifestError as e:
        return [str(e)]
    wild = [e for e in m.entries if any(s.startswith("<") for s in e.path_pattern)]
    for i, a in enumerate(wild):
        for b in wild[i + 1:]:
            if _overlaps(a.path_pattern, b.path_pattern):
                findings.append(f"wildcard overlap: {'/'.join(a.path_pattern)} vs {'/'.join(b.path_pattern)}")
    seen = set()
    for e in m.entries:
        p = "/".join(e.path_pattern)
        if p in seen:
            findings.append(f"duplicate path: {p}")
        seen.add(p)
    annotated = {e.rule for e in m.entries}
    referenced = set()
    for name, body in m.rules.items():
        referenced |= set(re.findall(r"\bsym-[A-Za-z0-9-]+", body))
        for ref in re.findall(r"\b[a-z][a-z0-9-]*\b", body):
            if ref in m.rules and ref != name:
                annotated.add(ref)  # a rule used by another rule needs no annotation
    for s in m.syms:
        if s not in referenced:
            findings.append(f"unreferenced symbol rule: {s}")
    for s in referenced:
        if s not in m.syms:
            findings.append(f"undefined symbol rule referenced: {s}")
    for name in m.rules:
        if name not in annotated and not name.startswith("sym-"):
            findings.append(f"rule with no annotation and no referrer: {name}")
    return findings

if __name__ == "__main__":
    fs = lint(open(sys.argv[1]).read())
    for f in fs:
        print(f"manifest lint: {f}", file=sys.stderr)
    sys.exit(1 if fs else 0)
```

- [ ] **Step 4: Author `protocol/MANIFEST.cddl`**

Author every row of the plan-header table, taking field lists and types **verbatim from `protocol/PROTOCOL-NSL-RECORDS.md` §4** (post-#197 short names: `Health{up,bid,wak,pto,sdr,ref}`, `Metrics{hsz,lb,hhw,lfb,gcc,hnl,hnhw,qhw,ovr,pdr,pco,pst}`, `CHeap{ob,ohw,alc,fre,fal}`, `LatencySpan{span,n,fails,minUs,maxUs,sumUsLo,sumUsHi,b0..b5}`). Full-wire spelling per design §2/§4:

```cddl
; protocol/MANIFEST.cddl — the machine-readable half of PROTOCOL-NSL-RECORDS.md.
; Machine-NORMATIVE for path/kind/class/dir/shape (design D8, 2026-08-24).
; 100%% stock CDDL; the ;# lines are the path binding (design §2.1).

sym-Health = #6.39("Health")
sym-up  = #6.39("up")
sym-bid = #6.39("bid")
; ... one sym-* rule per distinct key/record name/enum value ...

;# path=obs/health  kind=cell  class=routine  dir=up
health = #6.27([sym-Health, {
  sym-up  => uint,
  sym-bid => uint,
  sym-wak => uint,
  sym-pto => uint,
  sym-sdr => uint,
  sym-ref => uint,
}])

;# path=obs/build  kind=cell  class=routine  dir=up
build = #6.27([sym-Build, {
  sym-chip  => #6.39(tstr),
  sym-build => tstr,
  ? sym-sdk   => tstr,
  ? sym-image => bstr,
  ? sym-man   => bstr,        ; NEW (#243 D4): truncated 8-byte manifest pin
}])

blobref = #6.27([sym-BlobRef, {
  sym-hash => bstr,
  ? sym-size => uint,
  ? sym-alg  => #6.39(tstr),
}])

;# path=tel/log  kind=stream  class=drop-first  dir=up
log = #6.27([sym-Log, {
  sym-level => #6.39("debug") / #6.39("info") / #6.39("warn") / #6.39("error"),
  sym-text  => tstr,
}])

;# path=goal/config/<app>/<key>  kind=cell  class=routine  dir=down
;# path=obs/config/<app>/<key>  kind=cell  class=routine  dir=up
config-cell = scalar

scalar = int / tstr / bool / #6.39(tstr)

;# path=gw/liveness  kind=cell  class=routine  dir=gw
gw-verdict = #6.39("live") / #6.39("silent")
```

Enum fields: pin the symbol choice ONLY where the catalogue enumerates it (`Log.level`, `Panic.reason`, `Debug.action`, `SpanCtl.state`, `gw-verdict`); everything else is `#6.39(tstr)`. `Panic.core` is `blobref / nil`. `Metric.value` is `int / float`. `SpanCapture.ms` and `Reset.code` are `int` (−1 / raw causes). `App.arguments` is `{ * anysym => scalar }` with `anysym = #6.39(tstr)` — **if Task 3's zcbor validation rejects the non-literal tagged key, change it to `any` and leave a comment citing catalogue §6.2** (arguments is already the one unbounded-shape field).

- [ ] **Step 5: Lint until clean, tests pass**

Run: `python3 manifest_lint.py ../MANIFEST.cddl && python3 -m pytest test_manifest_lint.py test_manifestlib.py -q`
Expected: exit 0, all PASS.

- [ ] **Step 6: Cross-check the row set against reality**

Run: `rg -n "space put: '" ~/workspace/rete/nodus/nsl/lib ~/workspace/rete/nodus/oevm/zephyr/*.nsl | rg -v "test"` and `rg -n '"(goal|obs|sys|tel|dbg|diag|gw)/' ~/workspace/rete/porta/internal --type=go | rg -v _test`
Every path a writer names must match a manifest entry (`python3 -c "from manifestlib import parse; m=parse(open('../MANIFEST.cddl').read()); print(m.match('<path>'))"`). A miss is a finding: add the row (with its catalogue shape) or record why it is off-wire (local channels like `sys/timer`/`sys/tick` never reach the store and stay out).

- [ ] **Step 7: Commit**

```bash
git add protocol/MANIFEST.cddl protocol/tools/manifest_lint.py protocol/tools/test_manifest_lint.py
git commit -m "feat(#243): MANIFEST.cddl — the catalogue, machine-readable and linted"
```

---

### Task 3: zcbor golden gate — node encoder vs manifest

**Files:**
- Create: `protocol/tools/zcbor_env.py`, `nodus/nsl/tools/fixtures/manifest_goldens.nsl`, `nodus/nsl/tools/test_manifest_gate.py`
- Modify: `nodus/nsl/tools/run_tests_oevm.py` (wire `manifest-py` as an aux gate)

**Interfaces:**
- Consumes: `manifestlib` (Task 1), `MANIFEST.cddl` (Task 2).
- Produces: `zcbor_env.validate(cddl_path: Path, rule: str, cbor_bytes: bytes) -> None` (raises `CalledProcessError` on invalid) and `zcbor_env.ensure() -> Path` (venv python), used again by Task 8. Plus the aux-gate line `manifest-py` in the suite summary.

- [ ] **Step 1: Write `zcbor_env.py`**

```python
# protocol/tools/zcbor_env.py
"""Bootstrap a cached venv holding zcbor 0.9.1 and validate CBOR against
MANIFEST.cddl. Pins per the 2026-08-24 probe: cbor2<6 (6.x breaks zcbor's
import), CBOR piped via stdin (0.9.1 CLI file-open bug)."""
import subprocess, sys
from pathlib import Path

VENV = Path.home() / ".cache" / "rete-manifest-venv"

def ensure():
    py = VENV / "bin" / "python"
    if py.exists():
        return py
    subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)
    subprocess.run([str(VENV / "bin" / "pip"), "-q", "install",
                    "zcbor==0.9.1", "cbor2<6", "regex"], check=True)
    return py

def validate(cddl_path, rule, cbor_bytes):
    py = ensure()
    subprocess.run(
        [str(VENV / "bin" / "zcbor"), "validate", "-c", str(cddl_path),
         "-i", "-", "--input-as", "cbor", "-t", rule],
        input=cbor_bytes, check=True, capture_output=True)
```

- [ ] **Step 2: Write the golden fixture**

First read `nodus/tests/14_cbor/02_golden_hex.nsl` for the suite's hex-printing and record/bytes-literal idioms, and `nodus/nsl/tools/test_single_file_mode.py` for how a pytest gate compiles and runs one file in isolation (`OEVM_BUILD_DIR`, explicit `.oimg` output). Then write `nodus/nsl/tools/fixtures/manifest_goldens.nsl`: a class whose `main:args:` builds **one instance of every manifest rule** with small deterministic legal values (enum fields use a pinned enum value; optional fields: emit one instance WITH and one WITHOUT where the catalogue says absence is legal, e.g. `build` with and without `man`) and prints one line per instance:

```
GOLDEN <path> <hex>
```

where `<path>` is a concrete path matching the rule's entry (wildcards instantiated, e.g. `goal/apps/main`, `diag/spans/pullSend`) and `<hex>` is the record's `Cbor` encoding, printed with 02_golden_hex.nsl's helper. Mirror that file's structure exactly — nsl has sharp edges (no `#()` literals, wrong-arity constructor sends fail silently), so copy working idioms rather than inventing.

The fixture runs through the single-file harness (#252), so it carries the house `(* expect: *)` block **pinning the GOLDEN lines themselves**: write it with a placeholder first, run once, copy the actual GOLDEN output into the expect block, run again to green. From then on encoder byte-drift is a harness FAIL (a golden), while shape-vs-manifest drift is the zcbor gate's FAIL — two different failures with two different names.

- [ ] **Step 3: Write the failing gate test**

```python
# nodus/nsl/tools/test_manifest_gate.py
"""#243: every record the node's own encoder produces validates against
protocol/MANIFEST.cddl via zcbor. The two codecs were golden-tested only
against each other before this; now both answer to one declared contract."""
import subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "protocol" / "tools"))
from manifestlib import parse           # noqa: E402
import zcbor_env                        # noqa: E402

CDDL = ROOT / "protocol" / "MANIFEST.cddl"

HARNESS = ROOT / "nodus" / "nsl" / "tools" / "run_tests_oevm.py"
FIXTURE = ROOT / "nodus" / "nsl" / "tools" / "fixtures" / "manifest_goldens.nsl"

def _golden_lines(tmp_path):
    # Single-file mode (#252), isolated exactly as test_single_file_mode.py
    # isolates: never inherit a suite run's path selection, never share the
    # /tmp Path B artifacts.
    import os
    env = dict(os.environ)
    env.pop("OEVM_PATH", None)
    env["OEVM_BUILD_DIR"] = str(tmp_path / "bd")
    proc = subprocess.run(
        [sys.executable, str(HARNESS), str(FIXTURE)],
        capture_output=True, text=True, timeout=120, env=env)
    assert proc.returncode == 0, proc.stdout + proc.stderr  # expect-block golden holds
    out = []
    for line in proc.stdout.splitlines():
        if line.startswith("GOLDEN "):
            _, path, hexs = line.split()
            out.append((path, bytes.fromhex(hexs)))
    return out

def test_lint_clean():
    from manifest_lint import lint
    assert lint(CDDL.read_text()) == []

def test_every_golden_validates(tmp_path):
    m = parse(CDDL.read_text())
    lines = _golden_lines(tmp_path)
    assert lines, "fixture printed no GOLDEN lines"
    covered = set()
    for path, blob in lines:
        e = m.match(path)
        assert e is not None, f"fixture path {path} matches no manifest entry"
        zcbor_env.validate(CDDL, e.rule, blob)
        covered.add(e.rule)
    rules_needing_cover = {e.rule for e in m.entries}
    assert rules_needing_cover <= covered, f"rules with no golden: {rules_needing_cover - covered}"

def test_negative_control(tmp_path):
    # A Health golden re-tagged as a Metrics instance must FAIL — a gate
    # that has never been seen red is the false-green shape (memory: 30 ways).
    m = parse(CDDL.read_text())
    lines = dict(_golden_lines(tmp_path))
    import subprocess as sp, pytest
    with pytest.raises(sp.CalledProcessError):
        zcbor_env.validate(CDDL, "metrics", lines["obs/health"])
```

Fill in `_golden_lines` from `test_single_file_mode.py`'s recipe — it is the one piece that must be copied from working code, not sketched here.

- [ ] **Step 4: Run until green** — `cd nodus/nsl && python3 -m pytest tools/test_manifest_gate.py -q`. First run bootstraps the venv (needs network once). Iterate on the fixture until every rule is covered and the negative control is red-then-caught. If `app`'s `arguments` map fails zcbor parse, apply Task 2 Step 4's fallback now.

- [ ] **Step 5: Wire the aux gate**

In `run_tests_oevm.py`, beside `single_py = run_pytest_gate(...)`: add `manifest_py = run_pytest_gate("manifest-py", "nodus/nsl/tools/test_manifest_gate.py")`, add `and manifest_py != "FAIL"` to `suite_ok`, and add `f"  manifest-py: {_mark(manifest_py)}"` to the aux print block. SKIP (no network, no cached venv) stays yellow and loud, per the suite's own rule that a skip is a finding.

- [ ] **Step 6: Run the full suite** — `cd nodus/nsl && python3 tools/run_tests_oevm.py`. Expected: pass count unchanged, `manifest-py: PASS` on the aux line, no other aux gate moved.

- [ ] **Step 7: Commit**

```bash
git add protocol/tools/zcbor_env.py nodus/nsl/tools/fixtures/manifest_goldens.nsl nodus/nsl/tools/test_manifest_gate.py nodus/nsl/tools/run_tests_oevm.py
git commit -m "feat(#243): manifest-py aux gate — node goldens validate against MANIFEST.cddl via zcbor"
```

---

### Task 4: `porta/internal/manifest` — the Go table

**Files:**
- Create: `porta/internal/manifest/manifest.go`
- Test: `porta/internal/manifest/manifest_test.go`

**Interfaces:**
- Consumes: `protocol/MANIFEST.cddl`; `cval.Encode`, `cval.Record`, `cval.Symbol`, `cval.Kind`.
- Produces: `type Dir int` (`DirUp`, `DirDown`, `DirGw`); `type Table struct { Raw []byte; SHA256 string; ... }`; `Load(path string) (*Table, error)`; `(t *Table) Check(path string, kind cval.Kind, dir Dir, value []byte) error` — nil receiver returns nil (gate off); errors begin `manifest:path`, `manifest:kind`, `manifest:head`, `manifest:dir`.

- [ ] **Step 1: Write the failing tests** — against the REAL file (the test is also the Go parser's completeness check):

```go
// Copyright (c) 2026 Ekorau LLC

package manifest

import (
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/davidg238/porta/internal/cval"
)

func load(t *testing.T) *Table {
	t.Helper()
	_, this, _, _ := runtime.Caller(0)
	p := filepath.Join(filepath.Dir(this), "..", "..", "..", "protocol", "MANIFEST.cddl")
	tbl, err := Load(p)
	if err != nil {
		t.Fatal(err)
	}
	return tbl
}

func encRecord(t *testing.T, name string, fields cval.Map) []byte {
	t.Helper()
	b, err := cval.Encode(cval.Record{Tag: cval.Symbol(name), Fields: fields})
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestHealthAccepted(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Health", cval.Map{{K: cval.Symbol("up"), V: int64(5)}})
	if err := tbl.Check("obs/health", cval.KindCell, DirUp, v); err != nil {
		t.Fatalf("well-formed Health refused: %v", err)
	}
}

func TestKindMismatchRefused(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Health", nil)
	err := tbl.Check("obs/health", cval.KindStream, DirUp, v)
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:kind") {
		t.Fatalf("want manifest:kind error, got %v", err)
	}
}

func TestWrongRecordAtPathRefused(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Metrics", nil)
	err := tbl.Check("obs/health", cval.KindCell, DirUp, v)
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:head") {
		t.Fatalf("want manifest:head error, got %v", err)
	}
}

func TestUnknownPathRefused(t *testing.T) {
	tbl := load(t)
	err := tbl.Check("bogus/path", cval.KindCell, DirUp, []byte{0x01})
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:path") {
		t.Fatalf("want manifest:path error, got %v", err)
	}
}

func TestDirectionRefused(t *testing.T) {
	tbl := load(t)
	v := encRecord(t, "Health", nil)
	err := tbl.Check("obs/health", cval.KindCell, DirDown, v)
	if err == nil || !strings.HasPrefix(err.Error(), "manifest:dir") {
		t.Fatalf("want manifest:dir error, got %v", err)
	}
}

func TestLiteralShadowsWildcard(t *testing.T) {
	tbl := load(t)
	ctl := encRecord(t, "SpanCtl", cval.Map{{K: cval.Symbol("state"), V: cval.Symbol("running")}})
	if err := tbl.Check("diag/spans/ctl", cval.KindCell, DirUp, ctl); err != nil {
		t.Fatalf("ctl literal not shadowing wildcard: %v", err)
	}
	span := encRecord(t, "LatencySpan", nil)
	if err := tbl.Check("diag/spans/pullSend", cval.KindCell, DirUp, span); err != nil {
		t.Fatalf("wildcard row broken: %v", err)
	}
}

func TestScalarRow(t *testing.T) {
	tbl := load(t)
	s, _ := cval.Encode("bell") // tstr at goal/name
	if err := tbl.Check("goal/name", cval.KindCell, DirDown, s); err != nil {
		t.Fatalf("tstr at goal/name refused: %v", err)
	}
	b, _ := cval.Encode(cval.Bytes{1, 2})
	if err := tbl.Check("goal/name", cval.KindCell, DirDown, b); err == nil {
		t.Fatal("bytes at goal/name accepted")
	}
}

func TestGwDoor(t *testing.T) {
	tbl := load(t)
	v, _ := cval.Encode(cval.Symbol("live"))
	if err := tbl.Check("gw/liveness", cval.KindCell, DirGw, v); err != nil {
		t.Fatalf("gw/liveness refused: %v", err)
	}
}

func TestNilTableIsOpen(t *testing.T) {
	var tbl *Table
	if err := tbl.Check("anything", cval.KindCell, DirUp, nil); err != nil {
		t.Fatal("nil table must gate nothing")
	}
}
```

- [ ] **Step 2: Run to verify failure** — `cd porta && go test ./internal/manifest/` → does not compile.

- [ ] **Step 3: Implement.** Mirror Task 1's parser in Go (annotation lines, `sym-*` rules, rule heads; multi-line rule bodies until brackets balance). Head classification per entry:
  - rule body starts `#6.27([sym-X` → record row; expected head = `enc[:len(enc)-1]` where `enc, _ = cval.Encode(cval.Record{Tag: cval.Symbol(lit)})` and `lit` is `sym-X`'s literal (the trailing byte is the empty map's `0xa0`; assert this in an internal test: `head` for Health ends `...48 65 61 6c 74 68`, begins `d8 1b 82 d8 27 66`).
  - otherwise resolve the body's `/`-choice tokens (one level of named-rule indirection, e.g. `scalar`) to a set of first-byte classes: int (majors 0/1), tstr (0x60–0x7b), bool (0xf4/0xf5), sym (`d8 27`), float (0xf9–0xfb), nil (0xf6), bstr (0x40–0x5b).
  - `Check`: literal map lookup, then wildcard scan (same-length, `<`-prefixed segments match one segment); then kind, dir, head in that order, each failing with its named prefix and a detail naming path/got/want.
  - `Load` also fills `Raw` (file bytes) and `SHA256` (hex).

- [ ] **Step 4: Run until green** — `go test ./internal/manifest/`; then `gofmt -l .` prints nothing.

- [ ] **Step 5: Commit** — `git add porta/internal/manifest && git commit -m "feat(#243): porta manifest table — the ~100-line subset parser and Check"`

---

### Task 5: the store gates

**Files:**
- Modify: `porta/internal/space/store.go` (`Write` ~line 157, `WriteGateway` ~line 228, `Ingest` ~line 315; add `SetManifest`)
- Test: `porta/internal/space/store_test.go` (append)

**Interfaces:**
- Consumes: `manifest.Table`, `manifest.DirUp/DirDown/DirGw` (Task 4).
- Produces: `(s *Store) SetManifest(t *manifest.Table)`; refusal behavior relied on by Task 6's seam test: operator door → error return; wire door → events row `actor="gate"`, seq acked, not applied.

- [ ] **Step 1: Write the failing tests.** store_test.go is `package space` and already provides `testStore(t)` and `enc(t, v)` — use them, unqualified names throughout. Add one helper:

```go
func loadRealManifest(t *testing.T) *manifest.Table {
	t.Helper()
	_, this, _, _ := runtime.Caller(0)
	p := filepath.Join(filepath.Dir(this), "..", "..", "..", "protocol", "MANIFEST.cddl")
	tbl, err := manifest.Load(p)
	if err != nil {
		t.Fatal(err)
	}
	return tbl
}

func TestManifestGateOperatorDoor(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	v := enc(t, "bell")
	// wrong kind at a manifest path: refused with the named check
	if _, err := s.Write("n1", "goal/name", cval.KindStream, v, "op", "test"); err == nil ||
		!strings.HasPrefix(err.Error(), "manifest:kind") {
		t.Fatalf("want manifest:kind, got %v", err)
	}
	// unknown path: refused
	if _, err := s.Write("n1", "goal/bogus", cval.KindCell, v, "op", "test"); err == nil ||
		!strings.HasPrefix(err.Error(), "manifest:path") {
		t.Fatalf("want manifest:path, got %v", err)
	}
	// clean write passes
	if _, err := s.Write("n1", "goal/name", cval.KindCell, v, "op", "test"); err != nil {
		t.Fatal(err)
	}
}

func TestManifestGateWireDoor(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	health := enc(t, cval.Record{Tag: "Health", Fields: cval.Map{{K: cval.Symbol("up"), V: int64(1)}}})
	bad := enc(t, cval.Record{Tag: "Metrics"}) // wrong record at obs/health
	hw, skipped, err := s.Ingest("n1", 7, []Entry{
		{Seq: 1, Path: "obs/health", Kind: cval.KindCell, Value: health},
		{Seq: 2, Path: "obs/health", Kind: cval.KindCell, Value: bad},
		{Seq: 3, Path: "obs/health", Kind: cval.KindCell, Value: health},
	})
	if err != nil {
		t.Fatal(err)
	}
	if hw != 3 || skipped != 0 {
		t.Fatalf("refused entry must still ack: hw=%d skipped=%d", hw, skipped)
	}
	// the refused bytes are KEPT: an events row with actor=gate naming the check
	evs, _ := s.Events("n1", 10, 0)
	var gate int
	for _, e := range evs {
		if e.Actor == "gate" && strings.HasPrefix(e.Reason, "manifest:head") {
			gate++
		}
	}
	if gate != 1 {
		t.Fatalf("want exactly one actor=gate refusal row, got %d", gate)
	}
	// and the cell tree holds the LAST GOOD value, not the refused one
	cells, _ := s.Tree("n1", "obs/health")
	if len(cells) != 1 || cells[0].Seq != 3 {
		t.Fatalf("cell tree took the refused write: %+v", cells)
	}
}

func TestManifestGateDirection(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	v := enc(t, "x")
	// a node writing goal/** is refused-but-kept at the wire door
	_, _, err := s.Ingest("n1", 1, []Entry{{Seq: 1, Path: "goal/name", Kind: cval.KindCell, Value: v}})
	if err != nil {
		t.Fatal(err)
	}
	evs, _ := s.Events("n1", 10, 0)
	if len(evs) != 1 || evs[0].Actor != "gate" || !strings.HasPrefix(evs[0].Reason, "manifest:dir") {
		t.Fatalf("node→goal must be a gate row: %+v", evs)
	}
}

func TestManifestGateGateway(t *testing.T) {
	s := testStore(t)
	s.SetManifest(loadRealManifest(t))
	live := enc(t, cval.Symbol("live"))
	if err := s.WriteGateway("n1", PathLiveness, live, "sweep"); err != nil {
		t.Fatalf("gw/liveness refused: %v", err)
	}
	if err := s.WriteGateway("n1", "gw/bogus", live, "sweep"); err == nil {
		t.Fatal("unknown gw path accepted")
	}
}

func TestNoManifestNoGate(t *testing.T) {
	s := testStore(t) // no SetManifest: every pre-manifest test keeps passing
	v := enc(t, "x")
	if _, err := s.Write("n1", "goal/anything", cval.KindCell, v, "op", "t"); err != nil {
		t.Fatal(err)
	}
}
```

Check the real field names on `space.Event`/`space.Cell` (`Events`, `Tree` return types in store.go:574–645) and adjust selectors; the behaviors asserted are the contract.

- [ ] **Step 2: Run to verify failure** — `go test ./internal/space/` → `SetManifest` undefined.

- [ ] **Step 3: Implement.** Add to `Store`: `manifest *manifest.Table` and `func (s *Store) SetManifest(t *manifest.Table) { s.manifest = t }`. Then:
  - `Write` (after the existing kind check, before the tx): `if err := s.manifest.Check(path, kind, manifest.DirDown, value); err != nil { return 0, err }`. Keep the existing `gw/` guard above it — it is a second fence, not a casualty.
  - `WriteGateway` (beside its existing size/validate checks): `if err := s.manifest.Check(path, cval.KindCell, manifest.DirGw, value); err != nil { return err }`.
  - `Ingest`, inside the entry loop after `cval.Validate` (store.go:366) and before the cell upsert:

```go
		if err := s.manifest.Check(e.Path, e.Kind, manifest.DirUp, e.Value); err != nil {
			// Refused-but-kept (#243 D7): not applied, seq acked so the node
			// advances instead of wedging, bytes retained under actor=gate.
			// A manifest mismatch is a build error to fix at the bench, not
			// a runtime condition to negotiate with.
			if aerr := appendEvent(tx, nodeID, e.Path, e.Seq, e.Kind, e.Value, now, "in", "gate", err.Error(), bootID); aerr != nil {
				return 0, 0, aerr
			}
			hw = e.Seq
			continue
		}
```

- [ ] **Step 4: Run until green** — `cd porta && go test ./...` (the whole module: nothing pre-existing may move). `gofmt -l .` prints nothing.

- [ ] **Step 5: Commit** — `git add porta/internal/space && git commit -m "feat(#243): the manifest gate at all three store doors — subsumes #180's enforcement half"`

---

### Task 6: serve wiring, `GET /manifest`, and the seam test

**Files:**
- Modify: `porta/internal/portacli/serve.go` (flag, load, SetManifest, route on `srv.Mux`), `nodus/oevm/tests/porta_sync_host.sh`
- Test: `porta/internal/portacli/serve_test.go` or a small handler test beside it

**Interfaces:**
- Consumes: `manifest.Load`, `Store.SetManifest`, `httpsrv.Server.Mux` (`porta/internal/httpsrv/server.go:74`).
- Produces: `porta serve --manifest <path>`; `GET /manifest` → file bytes, header `X-Manifest-SHA256: <hex>`; empty flag = gate off with a loud startup line (Task 8's pin fetch consumes the endpoint).

- [ ] **Step 1: Write the failing handler test** — `httptest.NewServer` around a mux with the handler registered; assert body bytes equal the file, header equals the hex of their sha256, and `GET /manifest` on a manifest-less server 404s.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.** In `serve.go` beside the existing flags: `--manifest` (string, default `""`). After `space.Open` (serve.go:87): if set, `manifest.Load`, `st.SetManifest(tbl)`, log `manifest gate ON (<sha256 first 8> …)`; if empty, log a loud `MANIFEST GATE OFF — no --manifest given; kind divergence is unchecked (rete#243)`. Where the httpsrv is constructed (serve.go:156): register

```go
			srv.Mux.HandleFunc("/manifest", func(w http.ResponseWriter, r *http.Request) {
				if tbl == nil {
					http.NotFound(w, r)
					return
				}
				w.Header().Set("Content-Type", "text/plain; charset=utf-8")
				w.Header().Set("X-Manifest-SHA256", tbl.SHA256)
				w.Write(tbl.Raw)
			})
```

- [ ] **Step 4: Extend the seam gate.** In `porta_sync_host.sh`: pass `--manifest "$(git rev-parse --show-toplevel)/protocol/MANIFEST.cddl"` to the `porta serve` invocation, and add one assertion near the existing CLI legs: a deliberately mis-kinded `porta put --kind stream ... goal/name ...` must FAIL with `manifest:kind` in stderr (the gate's negative control at the seam). Every path the real node pushes now passes the closed manifest — this run is the completeness proof for the row table.
- [ ] **Step 5: Run it** — `cd nodus/oevm && bash tests/porta_sync_host.sh` → `PORTA SYNC OK`. A `manifest:path` refusal in porta's log means Task 2 Step 6 missed a writer: add the row, re-lint, re-run.
- [ ] **Step 6: Run all three repo gates** (nsl suite, `go test ./...`, seam) and commit — `feat(#243): serve --manifest + GET /manifest; seam gate runs manifest-on`.

---

### Task 7: `pin_manifest.py` + `check_provision.py`

**Files:**
- Create: `protocol/tools/pin_manifest.py`, `protocol/tools/check_provision.py`
- Test: `protocol/tools/test_pin_manifest.py`

**Interfaces:**
- Consumes: `GET /manifest` (Task 6); repo `protocol/MANIFEST.cddl`.
- Produces: `pin_manifest.py --out-dir <dir> [--url <porta>/manifest] [--emit-c <file>]` writing `MANIFEST.pinned.cddl`, `oevm_manifest_pin.txt` (line 1: full sha256 hex, line 2: `source=porta|repo`), and with `--emit-c` a TU defining `const unsigned char oevm_manifest_pin[8]` + `const unsigned int oevm_manifest_pin_len = 8` + `const char oevm_manifest_src[]`. `check_provision.py <build-id> --url <porta>/manifest` exits 0 on pin==served, 1 with a named reason otherwise (missing archive entry included).

- [ ] **Step 1: Write the failing tests** — pytest with `http.server` on a thread serving a fake `/manifest` (body + `X-Manifest-SHA256` header): fetch path pins from URL and records `source=porta`; hash-vs-header mismatch is an error; unreachable URL falls back to the repo file, records `source=repo`, and prints a loud note to stderr; `--emit-c` output contains the first 8 hash bytes as a C array; `check_provision` passes on a temp archive dir (`RETE_ELF_ROOT`) whose `<build-id>/manifest.sha256` matches the served hash, fails on mismatch, fails on missing entry.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** (stdlib: `urllib.request`, `hashlib`, `argparse`). Fetch verifies `sha256(body) == X-Manifest-SHA256` before pinning — an endpoint that lies about its own hash is refused, not trusted. And `pin_manifest.py` runs `manifest_lint.lint` on whatever it is about to pin (fetched or fallback), refusing a dirty file — this is design §5.1's build-time lint seat.
- [ ] **Step 4: Run until green; commit** — `feat(#243): pin_manifest + check_provision — fetch-and-pin with a loud repo fallback`.

---

### Task 8: bake the pin into the Zephyr build and the archive

**Files:**
- Create: `nodus/oevm/zephyr/cmake/gen_manifest_pin.cmake`
- Modify: `nodus/oevm/zephyr/CMakeLists.txt`, `nodus/oevm/zephyr/tools/console/archive.sh`

**Interfaces:**
- Consumes: `pin_manifest.py --emit-c` (Task 7).
- Produces: build dir artefacts `oevm_manifest_pin.c` (compiled in), `oevm_manifest_pin.txt`, `MANIFEST.pinned.cddl`; archive entries `MANIFEST.pinned.cddl`, `manifest.sha256`, `manifest.source` beside the ELF. Symbols `oevm_manifest_pin`/`oevm_manifest_pin_len`/`oevm_manifest_src` for Task 9.

- [ ] **Step 1: Read how `gen_build_id.cmake` is invoked** — `rg -n "gen_build_id" nodus/oevm/zephyr/CMakeLists.txt` — and mirror that hook exactly (build-time `execute_process` wrapper TU added to the target). `gen_manifest_pin.cmake` is a thin wrapper: `execute_process(COMMAND ${PYTHON_EXECUTABLE} ${SRC}/../../protocol/tools/pin_manifest.py --out-dir ${BUILD} --emit-c ${OUT_C} ...)`, honoring `PORTA_MANIFEST_URL` from the environment (absent → repo fallback, which the script already makes loud).
- [ ] **Step 2: Extend `archive.sh`** after the sha256 line: if `oevm_manifest_pin.txt` exists in `$BUILD_DIR`, copy `MANIFEST.pinned.cddl` into `$DEST`, write line 1 to `$DEST/manifest.sha256` and line 2's value to `$DEST/manifest.source`; if absent, print `[archive] no manifest pin in this build` — visible, not fatal, while pre-manifest build dirs exist.
- [ ] **Step 3: Verify without a board.** A full Zephyr build needs the west env; the falsifiable host-side checks are: `cmake -P`-parse of the new file, the suite's existing `cmake-parse` aux gate staying green, and running the exact `pin_manifest.py` command line by hand against a temp out-dir. Run all three. The first real board build (next hardware session) is the end-to-end witness — note that in the commit message rather than claiming it.
- [ ] **Step 4: Commit** — `feat(#243): the pin rides the build — gen_manifest_pin.cmake + archive stamp (board-unverified)`.

---

### Task 9: the `manifestPin` prim and `obs/build.man`

**Files:**
- Modify: `nodus/oevm/src/prim_names.def`, `nodus/oevm/src/prim_ids.h`, `nodus/oevm/src/platform.c`, `nodus/nsl/lib/kernel.nsl` (forwarder beside `buildId`, ~line 3971), `nodus/oevm/zephyr/PortaBridgeNode.nsl:114`, `nodus/oevm/zephyr/PortaBridgeNodeC6.nsl`, `nodus/oevm/zephyr/ProfileCostNodeC6.nsl:183`
- Test: a new `nodus/tests/22_spacesync/23_build_man.nsl`

**Interfaces:**
- Consumes: `oevm_manifest_pin`/`oevm_manifest_pin_len` (Task 8).
- Produces: `platform manifestPin` → Bytes(8) on a pinned build, nil otherwise; `obs/build` records carry `#man` iff the pin exists.

- [ ] **Step 1: The prim id.** Read the tail of `prim_ids.h` (155 = `nodeId`), then **count PRIM_NAMES entries in `prim_names.def`** (the standing lesson: counted, never `155+1` arithmetic) and cross-check the parity guard. Append `manifestPin` to `prim_names.def` and `#define PRIM_pf_manifestPin <counted>` to `prim_ids.h` with a comment in the file's house style (sibling of buildId 144: buildId names the image, this names the contract the image was built against; host answers nil).
- [ ] **Step 2: The C case.** In `platform.c`, mirror `PRIM_pf_buildId`'s define-split exactly (read platform.c:24–33 and the 1473 case; the nodeId comment records why a weak symbol is forbidden here): Zephyr builds see the generated TU's symbols; host builds take a `#define` path where `oevm_manifest_pin_len` is 0 and the prim answers nil. Non-zero: answer a Bytes of the 8 pin bytes (mirror however `buildId`'s case allocates its answer).
- [ ] **Step 3: The forwarder.** In `kernel.nsl` beside `public buildId = ( ^ platform buildId )`: `public manifestPin = ( ^ platform manifestPin )` with a one-line comment in the sibling style.
- [ ] **Step 4: The publishers.** First `rg -n "tag:.*with:.*with:.*with:" nodus/nsl` to learn whether a three-pair `Record tag:with:with:with:` constructor exists — a wrong-arity send silently answers an uninitialised instance. If it exists, each `obs/build` put becomes:

```
pin:: platform manifestPin.
(pin == nil)
  ifTrue: [ space put: 'obs/build' value: (platform Record tag: #Build
      with: #chip -> self chip with: #build -> platform buildId) ]
  ifFalse: [ space put: 'obs/build' value: (platform Record tag: #Build
      with: #chip -> self chip with: #build -> platform buildId
      with: #man -> pin) ].
```

If not, build the field map with whatever multi-field constructor `Metrics` uses (read `Kernel>>metrics`) — same conditional, different spelling. Apply identically in all three app files.
- [ ] **Step 5: The test.** `nodus/tests/22_spacesync/23_build_man.nsl`, house convention (inline `(* expect: *)` at the top, a class with `main:platform args:`, `println`): assert `platform manifestPin == nil` on host, build both Build variants, print field presence via `at:ifAbsent:`. Also add a `Build`-with-`man` golden to Task 3's fixture if Task 3 didn't already include it.
- [ ] **Step 6: Run** — `nodus/nsl/tools/run-one.py nodus/tests/22_spacesync/23_build_man.nsl B`, then the full suite (`python3 tools/run_tests_oevm.py` — count goes up by one, aux gates unmoved), then the seam gate (any `kernel.nsl` edit is board-relevant; host gates are the pre-board fence and the memory says a board must eventually witness kernel edits — say so in the commit).
- [ ] **Step 7: Commit** — `feat(#243): platform manifestPin + obs/build.man — the pin the image itself reports (board-unverified)`.

---

### Task 10: docs of record + design addenda

**Files:**
- Modify: `protocol/PROTOCOL-NSL-RECORDS.md`, `protocol/PROTOCOL-NSL.md`, `porta/docs/ARCHITECTURE.md`, `docs/design/2026-08-24-kind-schema-manifest-design.md`

- [ ] **Step 1: RECORDS.md.** Each §4 table row names its manifest rule (a `rule` column or an inline backtick name). §5 is rewritten per design §7: a field exists when `MANIFEST.cddl` says so; gates validate closed; additive-vs-breaking is a manifest-diff question; old rules 3–4 retired with a dated note. `obs/build` row gains `man` (kept optional; absence = pre-manifest image). `goal/forward`/`obs/forward` rows get: *no manifest rule — `Forward{…}` has no pinned shape; writing it first requires pinning it there.*
- [ ] **Step 2: PROTOCOL-NSL.md.** One pointer paragraph in §§1–4's conformance-target area: kind/shape/dir per path are machine-normative in `protocol/MANIFEST.cddl`; this document holds the model.
- [ ] **Step 3: ARCHITECTURE.md.** Add the ingest gate (three doors, refused-but-kept) and `GET /manifest` where the store/wire are described (that doc is marked stale-in-parts; add the new sections without undertaking the full rewrite).
- [ ] **Step 4: Design addenda.** Append a dated addendum section to the design doc recording the four places the code refined it, each one line: the ingest seat is a shared `Check` called from `Write`/`WriteGateway`/`Ingest` (the inbound door was never `Write`); `dir=gw` exists because the store's third direction does; literal-shadows-wildcard was earned immediately by `diag/spans/ctl` under `diag/spans/<span>` (§8's own trigger); annotations stack because shapes are shared (`obs/X` = `goal/X` by design).
- [ ] **Step 5: Commit** — `docs(#243): the catalogue names its rules; §5 rewritten for the manifest; design addenda`.

---

### Task 11: close-out

- [ ] **Step 1: Run all three repo gates one final time** (nsl suite with its aux line read, `go test ./...`, `porta_sync_host.sh`) and paste the actual outputs into the close-out comment — evidence, not assertion.
- [ ] **Step 2: Comment on #243**: what landed, the gate evidence, the two board-unverified items (Task 8's build hook, Task 9's prim — first C6 flash of the next hardware session witnesses both, and the flash itself exercises pin→archive→`obs/build.man`), and that gatekeeper's own plan absorbs `check_provision.py` when it exists.
- [ ] **Step 3: Promotion check (the repo's close-out rule)**: durable knowledge has left this plan — the manifest+lint live in `protocol/`, the gate behavior in ARCHITECTURE.md and RECORDS.md, the design addenda in the design doc. Name those in the comment. Close #243 only after the board witness; until then it stays open with the two items named.
