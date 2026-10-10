#!/usr/bin/env python3
"""lift_comments.py — nsl source for a paper: comments lifted into numbered references.

    python3 tools/lift_comments.py <file.nsl> [<file.nsl> ...]  > appendix.md

For each file, prints the code with every (* comment *) removed and a `(* Ref n *)` marker
left where it stood — at the end of the line for a trailing comment, on the next code line
for a block comment that preceded it — followed by "References for <file>": one entry per
comment, headed by the line of code it belonged to, numbered in order of appearance.
Nothing else is touched: the code shown is the code that runs. Written for paper E's
Appendix A (rete#380, David 2026-10-09: "it is really hard to read the code with all the
comments"); the source files keep their comments inline, this is a view.
"""
import re, sys

COMMENT = re.compile(r"\(\*.*?\*\)", re.S)


def lift(src):
    refs = []
    # 1. find comments with their spans
    spans = [(m.start(), m.end(), m.group(0)) for m in COMMENT.finditer(src)]
    # 2. walk them in reverse so offsets stay valid; decide block vs trailing
    out = src
    pending_block = []  # (ref_no) to attach to the next code line, recorded by offset
    markers = {}        # line-start offset in the ORIGINAL text -> list of ref numbers
    blocks = []         # (start, end) spans to delete (whole lines)
    inline = []         # (start, end, n)
    n = 0
    # lines occupied by block comments are not code: a block comment that precedes
    # another block comment attaches to the code line after BOTH
    comment_lines = set()
    for s, e, text in spans:
        ls = src.rfind("\n", 0, s) + 1
        le = src.find("\n", e)
        if le < 0:
            le = len(src)
        if src[ls:s].strip() == "" and src[e:le].strip() == "":
            p = ls
            while p <= e:
                comment_lines.add(p)
                q = src.find("\n", p)
                if q < 0:
                    break
                p = q + 1
    for s, e, text in spans:
        n += 1
        line_start = src.rfind("\n", 0, s) + 1
        line_end = src.find("\n", e)
        if line_end < 0:
            line_end = len(src)
        before = src[line_start:s]
        after = src[e:line_end]
        is_block = before.strip() == "" and after.strip() == ""
        body = text[2:-2]
        if is_block:
            # the next code line after the comment
            nxt = line_end + 1
            while nxt < len(src):
                le = src.find("\n", nxt)
                if le < 0:
                    le = len(src)
                if src[nxt:le].strip() and nxt not in comment_lines:
                    break
                nxt = le + 1
            ctx = src[nxt:src.find("\n", nxt) if src.find("\n", nxt) >= 0 else len(src)].strip()
            markers.setdefault(nxt, []).append(n)
            blocks.append((line_start, min(line_end + 1, len(src))))
        else:
            ctx = (before + after).strip()
            inline.append((s, e, n))
        refs.append((n, ctx, body))
    # 3. rebuild: process the text line by line from the original
    edits = []  # (start, end, replacement)
    for s, e in blocks:
        edits.append((s, e, ""))
    for s, e, k in inline:
        edits.append((s, e, f"(* Ref {k} *)"))
    for off, ks in markers.items():
        le = src.find("\n", off)
        if le < 0:
            le = len(src)
        edits.append((le, le, "   " + " ".join(f"(* Ref {k} *)" for k in ks)))
    edits.sort(key=lambda t: (t[0], t[1]))
    pieces, pos = [], 0
    for s, e, rep in edits:
        if s < pos:
            continue
        pieces.append(src[pos:s])
        pieces.append(rep)
        pos = e
    pieces.append(src[pos:])
    code = "".join(pieces)
    # tidy: collapse runs of >1 blank lines left by removed blocks
    code = re.sub(r"\n{3,}", "\n\n", code)
    # trailing whitespace from "code  (* Ref *)" joins
    code = "\n".join(l.rstrip() for l in code.splitlines()) + "\n"
    return code, refs


def reflow(body):
    """Comment text -> Markdown: prose paragraphs re-flowed, lines indented deeper than
    the comment's first text line kept verbatim as a code block."""
    lines = body.strip("\n").splitlines()
    if not lines:
        return ""
    base = min((len(l) - len(l.lstrip()) for l in lines if l.strip()), default=0)
    out, para, code = [], [], []

    def flush_para():
        if para:
            out.append(" ".join(x.strip() for x in para))
            para.clear()

    def flush_code():
        if code:
            out.append("```\n" + "\n".join(code) + "\n```")
            code.clear()

    for l in lines:
        if not l.strip():
            flush_para(); flush_code()
            continue
        indent = len(l) - len(l.lstrip())
        if indent >= base + 2 and not l.strip().startswith(("- ", "* ")) and (code or len(para) > 0 or out):
            flush_para()
            code.append(l[base:])
        else:
            flush_code()
            para.append(l)
    flush_para(); flush_code()
    return "\n\n".join(out)


def render(path, label):
    src = open(path, encoding="utf-8").read()
    code, refs = lift(src)
    md = [f"**`{label}`**\n", "```", code.rstrip(), "```", "", f"**References for `{label}`**\n"]
    for n, ctx, body in refs:
        ctx = ctx.replace("`", "'")
        if len(ctx) > 72:
            ctx = ctx[:69] + "..."
        md.append(f"**Ref {n}** · `{ctx}`\n")
        md.append(reflow(body) + "\n")
    return "\n".join(md), len(refs), code.count("\n")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        text, nrefs, nlines = render(p, p)
        print(text)
        print(f"<!-- {nrefs} refs, {nlines} code lines -->", file=sys.stderr)
