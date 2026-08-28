---
description: Conclude the session — rete addenda to the global conclude base (tracker → docs → status page → git report → memories → handoff)
---

This extends the global base at `~/.claude/commands/conclude.md`. Same order: **tracker → docs → git report → memories → handoff**. If the global file is missing, that sequence IS the base; apply it with the rete specifics below.

## rete addenda, by step

1. **Tracker** = GitHub issues on `davidg238/rete`; the pinned tracker is **#26**. Every open issue carries exactly one band `P0`–`P4` (P0 blocker · P1 pre-fleet · P2 · P3 · P4 hygiene). Comments cite commits or `file:line`. An issue found mid-session that was NOT the session's focus is filed as P3 with `found-during #N` in the body and is not worked on — it waits for the weekly review.

2. **Docs — promotion rule.** An issue is not closed until its durable knowledge is in a mainline document of record (`review/program-sequence.md`, `review/tests/CLAIMS.md`, `review/lib/`, `protocol/PROTOCOL-NSL*.md`, `porta/docs/ARCHITECTURE.md`). Name which one absorbed it in the close-out comment. **Do not write a new dated design doc at conclude time**; if a decision was made, add an addendum to the doc that already owns it.

3. **Status page — mandatory, every conclude, before the git report.**
   `docs/status/dev-status.html` is the ONE view of "what is top of mind". Refresh it:
   - update the stamp line (local date via `date`, current `git rev-parse --short HEAD`)
   - update the headline / "what first" block so it names the *next* concrete action and the focus issue
   - publish: `scp docs/status/dev-status.html gw85224-01:porta-v2/docs/status/dev-status.html`
     (served at `http://gw85224-01:6971/dev/status/dev-status.html`; a re-scp, never a restart)
   - verify: `ssh gw85224-01 md5sum porta-v2/docs/status/dev-status.html` matches the local file
   If the page cannot be published, say so in the handoff — do not skip silently.

4. **Git**: commit everything the session touched (`refs #N`, ≤5-line body) and `git push -u origin HEAD` — every session (David, 2026-08-28: the session end is the milestone and the backup). Ask first only for secrets, binaries, or files that were dirty before the session began. Never force-push.

5. **Memories**: preferences and gotchas ONLY. Anything about *state* (what is done, what is next, how to publish something) goes in this file, `CLAUDE.md`, or a doc — never memory. A memory that says "remember to X at close-out" is a bug: move X into this file.

6. **Handoff**: end with **Next session starts with** (branch, focus issue, first concrete action) and **Awaiting David**. Also write the same "Next session starts with" line into the `## Current focus` block at the top of `CLAUDE.md`, so the next `/clear` starts warm.
