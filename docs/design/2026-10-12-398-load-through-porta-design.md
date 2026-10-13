# Load through porta: porta's first write routes, custos's porta target (rete#398)

> Status: **PROPOSED 2026-10-12, for David** (questions Q1–Q5 at the foot). The node side is built and closed
> (#233, code-push design §10 and its 2026-10-12 addendum); this is the front. Decided upstream and not reopened:
> the push front is in 0.1, for consistency with debug (§10 T8 amendment, David 2026-10-10); agent first, HTTP the
> only agent surface (peer design D11–D12); porta is language-neutral and custos is the nsl peer (D1–D2); the two
> share only `protocol/` (#338).

## Where it stands

A push today is two commands on the gateway, run by a person with a shell there:

```
porta blob put --db soak.db F.oimg                       # store, sign the SHA-256, print the App record
porta put --db soak.db <node> goal/apps/main <hex> --raw # write it as the node's goal
```

and the proof is read back the same way (`porta get <node> obs/apps/main`). porta's HTTP listener answers
`/health` and `/manifest` and nothing else, so nothing off the gateway can push. custos has never written to
porta: its porta half reads a sqlite store on the host it runs on (the fleet pages, `/q/convergence`), and
`--porta-http` only proxies `/health`. The debug cells on the two bells (#379, #380) were written with the same
CLI. So this issue makes porta's first operator write route and custos's first write to porta.

## Proposal

**P1 — porta gains two general routes, not a push verb.** Each one is the HTTP twin of a CLI command that exists:

| Route | Twin of | Body | Answers |
|---|---|---|---|
| `POST /blob` | `porta blob put` | the image bytes | JSON `{Hash, Size, Sig, Record}`: the blob's SHA-256, the signature over it, and the `App` record as hex, ready to be a goal |
| `PUT /node/{id}/{path...}` | `porta put --raw` | one closed cval encoding (`application/cbor`) | JSON `{Seq}`; 400 if the body fails `cval.Validate` or the path is not under `goal/` |
| `GET /node/{id}/{path...}` | `porta get` | — | the cell's current value as raw CBOR, its seq in an `X-Porta-Seq` header; 404 if never written |

A push is then `POST /blob` and a `PUT` of `Record` to `goal/apps/main`, which is exactly what the CLI does. The
same `PUT` writes `goal/debug/main`, so debug through porta (and #415's profile row) reuses it rather than growing a
route of its own. The `GET` is what lets a custos that is not on the gateway read the proof (F1's "reads through
porta's HTTP interface"); it is one cell at a time, not the store, so it does not settle F1's monitor question.
porta stays language-neutral: it signs bytes and stores cval, and never parses an `.oimg`.

**P2 — custos gains a third kind of board: a porta node.** `--board <name>=porta:<node-id>`, using the porta base
URL `--porta-http` already names. Such a board serves `load` and nothing else for now (`GET /boards` says so). Its
`load.Target` is:

1. `POST /blob` with the `.oimg` (custos has already checked it with `oimg.Read`, as for a cable load);
2. `PUT /node/<id>/goal/apps/main` with the returned `Record`;
3. answer at once: `{Hash, Seq, Running, Converged}`, where `Running` is `obs/apps/main`'s image hash read in the
   same call. The load does **not** wait for the node: a deep-sleep node may not ask for half an hour.

`GET /load/last` on that board re-reads `obs/apps/main` each time it is asked, so an agent polls it until
`Converged` is true (the bell over Thread: ~3 min; the MagTag: its next wake). The web page shows the same JSON.

**P3 — the real-VM gate is `host_ota`'s clean deploy, driven through custos.** `nodus/oevm/tests/host_ota/run.sh`
already runs a real `porta serve` and three runner-wrapped host VMs. Scenario 1 pushes by `porta blob put` +
`porta put`; it would push through a `custos` serving three porta boards instead, and assert as it does now (the
runner's lines in order, and porta's tree). Scenarios 2–4 keep the CLI, so the CLI stays covered and the gate keeps
its four claims. Go unit tests for each route and for the porta target against an `httptest` porta.

## What is not in this issue

- **The cable-flash rule from #233** (write a node's goal to the cable's image before a cable flash). A cable board
  in custos does not know its porta node id; tying the two is a board-config change that waits for #426's fix,
  which may make the rule unnecessary.
- **Authentication.** porta's HTTP listener admits callers by CIDR (`--http-allow-cidr`, default the private
  ranges, loopback and Tailscale's `100.64.0.0/10`) and nothing else. With P1, anything inside that range can have an image signed and pushed. Today the
  same power needs a shell on the gateway. See Q3.
- **Firmware OTA** (peer design D4, still deferred) and a progress stream during staging (the node reports none
  today; `tel/log` has the `ota staging N` rows for anyone who wants them).

## Questions for David

1. **Two general routes plus a read (P1), or one push route** (`POST /node/{id}/apps/main` with the image as the
   body, store + sign + write in one call)? Recommended: the general pair. It is one mechanism for load, debug and
   profile, each route is a twin of a CLI command, and porta needs no notion of what an app is.
2. **Load answers at the goal write and is polled for convergence (P2), or blocks until `obs/apps/main` matches?**
   Recommended: answer at once. A block cannot be bounded for a sleepy node, and the debug page already polls
   (`/debug/state?after=N`).
3. **Is CIDR-only admission acceptable for 0.1's write routes?** Recommended: yes for 0.1, recorded as a known gap
   in the release's security note (#253's draft), because the bench's LAN and the tailnet are the trust boundary today. The
   alternative is a bearer token in a file beside `porta.key`, about 30 lines on each side.
4. **The gate: move `host_ota` scenario 1 onto custos (P3), or add a fifth scenario?** Recommended: move it. A fifth
   would push the same image the same way, and the gate's claim count stays four.
5. **`custos load --porta` as a CLI flag too**, as the issue title says, or the HTTP route only (D12: a CLI wrapper
   once it has earned its place)? Recommended: HTTP only, and the issue retitled to match.
