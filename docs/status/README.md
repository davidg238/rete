# docs/status — the orientation deck

A dated snapshot answering *where is this project right now*: what works, what is
broken, and every open issue by priority. First written 2026-08-12, when 136 open
issues had stopped being navigable by reading them.

```
docs/status/2026-08-12-where-we-are.html    the deck — open it in a browser
docs/status/make-deck.py                    the generator
docs/status/issues.json                     the last `gh issue list` dump (cache)
```

The filename is `PROSE_DATE`, so a deck written on a new day lands beside the old
one rather than overwriting it. Two runs on the same day revise one file — which is
what the second 2026-08-12 pass did.

**Revised later on 2026-08-12** — the unlabelled sweep (#26): 15 issues closed
against verified evidence, 85 banded, and the `someday` label retired into a fifth
band, **P4**. The ladder is now one ordered axis, P0..P4, and every open issue
carries a band except the tracker itself.

> **A date trap, since it already caused one wrong staleness band.** `PROSE_DATE` is
> hand-typed and `FETCH_DATE` is `issues.json`'s mtime — **both local time**, and the
> bench runs MST. An agent session stamped in UTC is up to a day ahead after 17:00
> local, and typing that UTC date into `PROSE_DATE` makes the deck accuse itself of
> being stale against data fetched seconds earlier. Use the local date (`date`), not
> the session's. The sweep's GitHub comments read 2026-08-13 for the same reason —
> GitHub stamps UTC — and that is the one place the two conventions differ.

Rebuild:

```bash
python3 docs/status/make-deck.py            # refetch issues, rebuild
python3 docs/status/make-deck.py --offline  # rebuild from the cached dump
```

Arrow keys, space, or scroll.

**The deck is dark, pinned — it does not follow the reader's browser** (David,
2026-08-12). It used to: theming was a single `@media (prefers-color-scheme:dark)`
block and nothing else, so the palette was whatever the viewer's browser reported
that day, and the deck went from dark to white between two readings with its CSS
byte-identical. A document that silently flips palette reads as a change to the
document.

The dark values now sit on bare `:root` with `color-scheme:dark` (so the scrollbar
and form UI follow), and the media query is **deleted** rather than left beside them
— two ways to say the same thing is how a palette drifts apart.
`:root[data-theme="light"]` is the escape hatch; nothing sets it, so put
`data-theme="light"` on `<html>` by hand if you ever want the light one back.

Note the generated file is a **fragment** — no doctype, no `<html>`, no `<body>`;
the browser synthesises them. That is why the theme is pinned in the cascade rather
than as an attribute, and why there is no element to hang anything else on either.

## The one thing to understand before rerunning it

**The deck is two kinds of thing, and they age at different rates.**

- The **issue tables and counts are generated** from `gh issue list`. Rerun the
  script and they are current. Nothing in them is hand-transcribed, which is the
  point — the first draft of this deck was going to have 136 rows typed out.
- **Every prose slide is a dated judgement**, written by hand: what is green, what
  is broken, which P1s are real, what to do next. Rerunning refreshes none of it.

So a rerun in a month yields a deck whose tables are honest and whose opinions are a
month stale — a page that looks freshly generated and quietly misleads. That is the
same shape as the failures the deck itself catalogues on slide 14.

**The guard:** `PROSE_DATE` in `make-deck.py` is stamped into the deck beside the
issue-fetch date, and when they differ the title slide carries a band saying the
slides disagree with their own data. It was tested by moving the dump's date and
watching the band appear.

**So: fix staleness by editing the prose and then moving `PROSE_DATE`. Never by
moving `PROSE_DATE` alone.** Or start a new dated deck beside this one and leave
this one as the record of what was believed on 12 August.

## What is deliberately not in it

A chart grouping the issues by subject. One was built and cut: keyword-grouping the
titles gave counts that swung roughly threefold depending on the order of the
matching rules, because an issue can mention a board, a test and the protocol in one
sentence. A number that unstable is an instrument that reports confidently and
measures nothing. Slide 17 makes the same point as prose, where a judgement is
visibly a judgement.

The **era** breakdown stayed, because it is derived from issue numbers, which only
go up — it is arithmetic rather than opinion.

## Where the truth actually lives

The deck is a map, not a source. When it and any of these disagree, these win:

| | |
|---|---|
| `gh issue list` | the queue; close-out comments carry the verification detail |
| `docs/next-steps-overview.md` | direction |
| `review/program-sequence.md` | the ladder — the roadmap of record |
| `review/tests/CLAIMS.md` | every claim, with the test that enforces it |
| `docs/results/` | measurements, dated |
| `docs/design/` | decision records — the *why*, kept after the decision is superseded |
