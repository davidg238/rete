# MagTag console capture

`capture.py` is tracked here **verbatim** as it existed in `~/magtag-captures/` on
2026-08-03. It is not maintained, not tested, and not the pattern to copy.

It is here because #56 sub-project 1 diagnosed untracked out-of-repo tooling as a
problem (design §2) and it would be incoherent to ship that diagnosis while leaving
the specimen in a home directory. Tracking it is not endorsing it.

It already encodes one hard-won lesson — it strips NUL bytes — which is why the
C6 collector does the same (custos, `custos/internal/console`).

**It is deleted when a MagTag collector lands.** The MagTag needs its own design
work first: its PL2303 header does not reset the board on open, so a collector may
attach to a running board mid-life and the log will start mid-stream (design §10).

For the C6, use custos instead (`custos/docs/console.md`).
