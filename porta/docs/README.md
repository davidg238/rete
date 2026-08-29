# porta/docs — the gateway's documents

porta was rewritten from scratch on 2026-08-01 (**v2**: a mirrored space synced over
UDP + CBOR; no verbs on the wire). v1 — the command vocabulary, the command queue, TFTP
image delivery, the report schema — was deleted in one commit; its last tree is tagged
`porta-v1-final`. Read every document here with that date in hand.

## Current

- `../README.md` — what porta is now, the CLI verbs, the license.
- `ARCHITECTURE.md` — **the system picture**: the three planes, the two transports, what
  porta is and is not, modes and app lifecycle as paths, the manifest gate, the store's
  two open doors, and the repo topology. Rewritten for v2 2026-08-29 (#279); §7 records
  the Toit lineage rather than describing it as current.
- `CLI.md` — **the v2 command reference**: `serve`, `put`, `get`, `log`, `key gen`,
  `blob put`, `snapshot`, every flag, and what each of v1's deleted verbs became.
  Rewritten 2026-08-29 and checked against the binary's own `--help` by the nsl suite's
  `porta-cli` gate, so a command or flag can no longer disappear from under it (#279).
- `../../docs/tutorials/03-porta.md` — **the executable introduction**: `serve`,
  `put`, `get` and `log` driven against a real node on loopback, with every command
  block run by the nsl suite (`tutorial-porta`). Where GETTING-STARTED.md used to be
  read, and it cannot rot the way that page did.
- `design/2026-08-01-porta-space-sync-rewrite-design.md` — **the v2 decision record**
  (D1–D8), the store schema, the wire shapes, and the §4.2 capability audit that gave
  every v1 feature a fate.
- `design/` (2026-08 onward) — the rungs since: gatekeeper, boot-anchored reads, the
  durability trio.
- `../../protocol/PROTOCOL-NSL.md`, `PROTOCOL-NSL-RECORDS.md`, `MANIFEST.cddl` — the
  wire contract. Not in this directory on purpose: both halves answer to it.
- `superpowers/plans/` — execution scaffolding; the 2026-08 ones are v2.

## Stale in parts — banners say which

- `GETTING-STARTED.md` — corrected against the code on 2026-08-05; §3–§4's verb
  walkthrough is v1 and no longer exists (#279). §1–§2 and §5 — the `.deb`, the systemd
  unit, `/health`, and the operator-surface story — are current.
- `DEVSDK.md` — the `devsdk/` contract; `exec` and `provision` survive, `apiclient` went
  with v1's HTTP surface.
- `demo/` — a show-and-tell script for the v1 gateway (2026-06). A presentation, dated;
  not a document of record.

## Archive — read the banner first

`archive/plans/` and `archive/specs/` are the **v1 design and plan records** (2026-05 to
2026-06), 52 files. Every one opens with a `HISTORICAL` banner. They are kept, not
deleted, so the issues and commits that cite them by path still resolve; the §4.2 audit
in the v2 design is the map from any of them to what replaced it.
