# docs/status — the orientation deck

A dated snapshot answering *where is this project right now*: what works, what is
broken, and every open issue by priority. Written 2026-08-12, when 136 open issues
had stopped being navigable by reading them.

```
docs/status/2026-08-12-where-we-are.html    the deck — open it in a browser
docs/status/make-deck.py                    the generator
docs/status/issues.json                     the last `gh issue list` dump (cache)
```

Rebuild:

```bash
python3 docs/status/make-deck.py            # refetch issues, rebuild
python3 docs/status/make-deck.py --offline  # rebuild from the cached dump
```

Arrow keys, space, or scroll. It renders in light and dark.

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
