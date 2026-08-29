#!/usr/bin/env bash
# porta/docs/cli-gate.sh — the CLI reference is checked against the binary, not trusted.
#
#   ./cli-gate.sh [CLI.md]
#
# Exit 0 = PASS, 1 = FAIL, 77 = SKIP (no Go toolchain). Run by the nsl suite as the
# `porta-cli` aux gate.
#
# WHY THIS EXISTS. porta/docs/CLI.md documented `scan`, `ping`, `device set` and
# `container install` for four weeks after the commit that deleted them, and nothing said
# so until someone read the page. A reference page rots the same way a tutorial does — it
# is read by outsiders and re-read by nobody — but unlike a tutorial its claims are not
# commands anyone runs, so executing it is not available. What IS available is the
# binary's own account of itself: cobra already knows every command and every flag.
#
# WHAT IT CHECKS — NAMES ONLY, in both directions:
#   1. every leaf command in `porta --help` has a `## <command>` section on the page;
#   2. every flag that command declares is named somewhere in that section;
#   3. every `--flag` the page names anywhere exists somewhere in the binary;
#   4. the page's copy of the root `Available Commands:` block lists exactly the
#      subcommands the binary registers.
#
# It does NOT score prose, and it must not: a stale sentence is a judgement call, a
# phantom flag is a bug, and a gate that tries to grade English is a gate that gets
# disabled. The page's transcripts are dated evidence and this gate does not run them.
#
# It falsifies itself twice before it will clear anything (a gate that has gone blind must
# fail, not pass): an EMPTY page must FAIL — vacancy is not agreement — and the real page
# plus one invented flag must FAIL.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
doc="${1:-$here/CLI.md}"
porta_dir="$(cd "$here/.." && pwd)"

if ! command -v go >/dev/null 2>&1; then
  echo "porta-cli: no go toolchain on PATH" >&2
  exit 77
fi
[ -f "$doc" ] || { echo "porta-cli: no such page: $doc" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
(cd "$porta_dir" && go build -o "$work/porta" ./cmd/porta) || {
  echo "porta-cli: porta does not build — nothing to check the page against" >&2; exit 1; }

python3 - "$work/porta" "$doc" "$work" <<'PYEOF'
import re, subprocess, sys

binary, docpath, work = sys.argv[1], sys.argv[2], sys.argv[3]

# Cobra's own commands: not porta's surface, and nobody documents them.
BUILTIN = {"help", "completion"}
# --help is on every command by construction; documenting it 7 times says nothing.
BORING = {"--help"}


def help_text(path):
    return subprocess.run([binary, *path, "--help"], capture_output=True, text=True).stdout


def section(text, header):
    """The indented body under `header:` in a cobra help page, to the first blank line."""
    out, on = [], False
    for line in text.splitlines():
        if line.strip() == header:
            on = True
            continue
        if on:
            if not line.strip():
                break
            out.append(line)
    return out


def subcommands(text):
    names = [l.split()[0] for l in section(text, "Available Commands:") if l.strip()]
    return [n for n in names if n not in BUILTIN]


def own_flags(text):
    """Flags the command declares itself — NOT the inherited Global Flags block."""
    flags = set()
    for line in section(text, "Flags:"):
        m = re.search(r"(--[a-z][a-z0-9-]*)", line)
        if m and m.group(1) not in BORING:
            flags.add(m.group(1))
    return flags


def walk(path=()):
    """Yield (command path, its own flags) for every LEAF command."""
    text = help_text(list(path))
    subs = subcommands(text)
    if not subs:
        yield path, own_flags(text)
        return
    for s in subs:
        yield from walk(path + (s,))


root_help = help_text([])
root_subs = set(subcommands(root_help))
global_flags = own_flags(root_help)          # --db, --version live on the root
leaves = list(walk())
every_flag = global_flags.union(*(f for _, f in leaves)) if leaves else set(global_flags)


def check(page_text, label):
    """Return the list of complaints about `page_text`. Empty list = the page agrees."""
    bad = []

    # Sections: '## <heading>' to the next '## '. A command's flags must be named in ITS
    # section, so a flag documented under the wrong command is still undocumented.
    secs, cur = {}, None
    for line in page_text.splitlines():
        if line.startswith("## "):
            cur = line[3:].strip()
            secs[cur] = []
        elif cur is not None:
            secs[cur].append(line)
    secs = {k: "\n".join(v) for k, v in secs.items()}

    for path, flags in leaves:
        name = " ".join(path)
        body = secs.get(name)
        if body is None:
            bad.append(f"command in the binary but NO '## {name}' section on the page")
            continue
        for f in sorted(flags):
            # A whole token, not a substring: '--db' must not be satisfied by '--dbg',
            # nor '--sweep-tick' by a typo'd '--sweep-tickX'. Found by falsifying this
            # gate against its own page — the substring form passed a page that had
            # lost the flag.
            if not re.search(re.escape(f) + r"(?![A-Za-z0-9_-])", body):
                bad.append(f"'{name}' declares {f} — not named in its section")

    # The root's own flags are inherited by every command, so they are checked against
    # the WHOLE page rather than one section — but they are still checked. A global flag
    # is the easiest one to assume everybody knows.
    for f in sorted(global_flags):
        if not re.search(re.escape(f) + r"(?![A-Za-z0-9_-])", page_text):
            bad.append(f"global flag {f} — not named anywhere on the page")

    # The reverse direction, over the WHOLE page including its transcripts: a flag the
    # page shows is a flag the page promises. This is what catches a deleted one.
    for f in sorted(set(re.findall(r"--[a-z][a-z0-9-]*", page_text))):
        if f not in BORING and f not in every_flag:
            bad.append(f"page names {f} — NO such flag in the binary")

    # The page's copy of the root help. An extracted set that is empty FAILS here rather
    # than passing quietly, because a reformatted block must not read as agreement.
    listed = set(subcommands(page_text))
    if listed != root_subs:
        for n in sorted(root_subs - listed):
            bad.append(f"root command {n!r} missing from the page's 'Available Commands:' block")
        for n in sorted(listed - root_subs):
            bad.append(f"page's 'Available Commands:' block lists {n!r} — not a porta command")
    return bad


page = open(docpath).read()

# Canary 1 — vacancy must not read as agreement.
if not check("", "empty"):
    print("porta-cli: FAIL — the check passes an EMPTY page; it is checking nothing")
    sys.exit(1)
# Canary 2 — an invented flag must be caught.
if not check(page + "\n`--not-a-real-flag`\n", "phantom"):
    print("porta-cli: FAIL — the check passes a page naming a flag that does not exist")
    sys.exit(1)

problems = check(page, "page")
if problems:
    print(f"porta-cli: FAIL — {docpath} disagrees with the binary")
    for p in problems:
        print(f"  {p}")
    sys.exit(1)
print(f"porta-cli: PASS ({len(leaves)} commands, {len(every_flag)} flags)")
PYEOF
