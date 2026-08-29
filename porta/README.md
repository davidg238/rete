# porta

porta (Latin "gate") is the gateway: the northbound half of the fleet system in this
repository. Every node owns a tree of paths — its **space** — and porta holds a mirror of
that tree. Operators write intent under `goal/**`; the node writes what it sees under
`obs/**`, `tel/**` and `sys/**`. Desired-versus-observed is a tree diff, so the
self-heal loop and the operator's view are the same query.

The two sides exchange the mirror over **plain UDP carrying CBOR**, in two
node-initiated idempotent messages: a `pull` (writes porta has for the node) and a
`push` (the node's entries). **There are no verbs on the wire and none in the store**;
the operator vocabulary is CLI sugar over path writes and tree reads.

    porta serve                      run the gateway: UDP space listener + operator HTTP surface
    porta put <node> <path> <value>  write a value to a node's goal tree
    porta get <node> [prefix]        dump a node's cell tree
    porta log <node>                 the write log for a node, newest first
    porta blob put <file.oimg>       store an image blob, sign it, print the goal/apps write
    porta key gen                    the signing keypair for image push
    porta snapshot                   a consistent copy of the store

- `cmd/porta/` + `internal/portacli/` — the cobra CLI
- `internal/cval/` — the nsl message algebra's CBOR codec, hand-rolled and golden-tested
  against the node's own encoder
- `internal/space/` — the store: four sqlite tables (`nodes`, `cells`, `events`,
  `exchanges`); a new feature adds rows, not tables
- `internal/wire/` — pull/push framing as a pure `Handle([]byte) []byte` under a thin UDP loop
- `internal/gatekeeper/`, `internal/blob*/` — the query surface and the bulk plane
- `devsdk/` — the public Go SDK node-repo tooling imports; porta imports no node code

Documents: `docs/README.md` is the map. The decision record is
`docs/design/2026-08-01-porta-space-sync-rewrite-design.md`; the wire contract is
`../protocol/`. Gate: `go test ./...`, and `gofmt -l .` prints nothing.

## License

- The porta gateway (everything here except `devsdk/`) is licensed under the
  **GNU Affero General Public License v3.0** — see [`LICENSE`](LICENSE). Run it, modify
  it, self-host it freely; if you offer a modified porta to others over a network, offer
  them the source too. For uses the AGPL doesn't fit, contact Ekorau LLC about a
  commercial license.
- **`devsdk/`** is licensed under the **MIT License** — see
  [`devsdk/LICENSE`](devsdk/LICENSE) — so node-repo tooling in any language can import
  it without copyleft obligations.
- `../protocol/` is a published specification: implementing the wire protocol carries
  no license obligation in either direction.

porta v1 — commands, a queue, TFTP delivery, a report schema — was deleted on
2026-08-01; the last v1 tree is tagged `porta-v1-final` and its protocol is frozen under
`../protocol/v1/`. The Toit nodes that still speak it live in the `nodus-v1` repository.
