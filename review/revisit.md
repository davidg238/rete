# REVISIT — things learned in passing that are not yet anyone's work item

A holding place for findings that surfaced *while doing something else*, are worth acting
on eventually, and would otherwise be lost between a closed issue and a design doc nobody
re-reads.

**What belongs here.** A thing that is true, was verified, and has no owner. Typically: a
guarantee that turns out to be weaker than assumed; an option considered and not taken,
where the reasoning would have to be rebuilt from scratch next time; a constraint that will
bind later but does not bind today.

**What does not.** Anything actionable now — that is a GitHub issue, and the tracker is the
status of record. Anything already promoted to a mainline document of record (the ladder,
`~/workspace/rete/review/tests/CLAIMS.md`, the owning doc under `~/workspace/rete/review/lib/`).
An entry here is a note that promotion has *not* happened and a decision is still owed.

**Entry format.** Dated heading, the finding stated as a claim, the evidence that supports
it, and what would make it matter. Cite absolute paths — the program spans five repos
across two workspace roots. Entries are append-only; when one is acted on, say so in place
rather than deleting it, so the reasoning survives the resolution.

---

## 2026-07-22 — the G2 503 shed cannot engage under connection-rate overload

**Claim.** The node's designed graceful-degradation path — answer 503 when the in-flight
table is full — is unreachable when the overload is in the *connection rate* rather than in
concurrent work. At that layer the guarantee is a paper one.

**Why.** The refusal happens in the TCP stack *below* `zsock_accept()`. `http_accept_request`
(`~/workspace/rete/nodus/tuvm/src/http_core.c`) can only shed a connection that has already been
accepted, so a client turned away by an RST never reaches the shed at all. This is why every
failure observed in tuvm#65 was a connect-level error (`ECONNREFUSED`/`EHOSTUNREACH`) and
never an `HTTPError` — a fact the issue body had already noticed under "What it is not"
without yet having the explanation.

**Evidence.** tuvm#65 investigation, 2026-07-21/22. Pre-fix the node had a hard ceiling of
4 contexts ÷ 1.5 s TIME_WAIT = 2.67 req/s, and everything above it was RST; measured
achieved throughput was pinned at 2.44–2.82/s at every offered rate from 4/s to unpaced,
with zero 503s emitted. Full record:
`~/workspace/rete/review/2026-07-21-issue-65-http-load-wedge/README.md`.

**What would make it matter.** Any claim that the node "degrades gracefully under load", any
supervisor/OOM policy that expects to *see* shed events as its overload signal (cf. tuvm#22),
and the unattended-node flight gate in Arc 3 of the ladder. The shed counts what the node
chose to refuse; it is blind to what the stack refused on its behalf, and those are the
majority under connection-rate overload. If shed rate is ever used as a health metric, it
will read healthy at exactly the wrong moment.

**Not yet decided.** Whether the node should observe connection-level refusals at all. It
now counts accept *failures* (`http_accept_failures()`, tuvm#65), but a SYN that the stack
RSTs before the socket layer sees it is not counted anywhere and may not be countable
without stack support.

---

## 2026-07-22 — capping `OEVM_HTTP_INFLIGHT` remains the only lever that makes the RX trap impossible

**Claim.** The tuvm#65 starvation deadlock is now *escapable* but not *impossible*. Capping
concurrently-accepted connections below the RX descriptor count would make it arithmetically
impossible, and that option was not taken.

**Why it was not taken.** Keep-alive on the listening socket (tuvm#65, `be2e961`) reaps the
idle ESTABLISHED connections that pinned the RX slab, which ends the failure once entered —
so the cap was not *needed*, and adding it at the same time would have muddied attribution
for the fix that was being verified. Backlog 4 → 2 and `NET_PKT_RX_COUNT` 8 → 16 reduce the
likelihood but preserve the shape: enough simultaneous connections can still pin the pool,
and escape then depends on the reaper running.

**Evidence.** The measured chain is in
`~/workspace/rete/review/2026-07-21-issue-65-http-load-wedge/README.md` — backlog fills with
ESTABLISHED connections, each pins RX `net_pkt`s, the slab hits 0, `zsock_accept()` can no
longer succeed, and freeing requires receiving. A 130-cycle / 60-minute soak on the fixed
build stayed healthy throughout, and the reaper was observed firing once
(`accept failed errno=113`, `ECONNABORTED`).

**What would make it matter.** A node where the ~25 s reaper latency is itself unacceptable
— a control loop rather than a page server — or any port to a part with a smaller packet
pool, where the margin the widened slab bought disappears. Also: the fix as it stands has
a dependency worth remembering, in that correctness under overload now rests on
`CONFIG_NET_TCP_KEEPALIVE` remaining enabled. A future RAM squeeze that switches it off to
recover a few hundred bytes would silently restore a permanent, silent, reset-only failure.
That is the kind of regression no test currently catches.

**ACTED ON 2026-07-22 — the keep-alive dependency now has an owner: tuvm#71.** Filed after
checking all 15 `~/workspace/rete/nodus/tuvm/zephyr/prj*.conf`: `CONFIG_NET_TCP_KEEPALIVE` is set by
`prj_esp32.conf` alone, while `prj_esp32_nowifi.conf` has `CONFIG_NET_TCP=y` **without** it
— so the exposure is present today, not merely future, and `prj_esp32c6.conf` carries the
pre-fix `NET_PKT_RX_COUNT=8` into the board #57 wants to make the dev-loop target. Proposed
guard is a `BUILD_ASSERT` in `~/workspace/rete/nodus/tuvm/src/backend/zephyr/http_zephyr.c` gated on
`CONFIG_NET_TCP` (that file is compiled for every board, `zephyr/CMakeLists.txt:41`). The
rest of this entry — the `OEVM_HTTP_INFLIGHT` cap — remains unowned.

**Cheap mitigation if it ever bites.** `OEVM_HTTP_INFLIGHT` is a compile-time constant in
`~/workspace/rete/nodus/tuvm/src/http_backend.h`; 4 → 2 costs nothing and needs no RAM.

**IT BIT, 2026-07-22 — and the reaper does not save you. Now tuvm#73.** A single browser tab
on the node's own page pins the whole connection budget and the node answers nothing at all.
The entry above says escape "depends on the reaper running"; the measurement says the reaper
**cannot** run for this case: **keep-alive reaps a DEAD peer, not an IDLE one** — a browser
answers the probes, so the connection is healthy by definition. Tightening the probes
10/5/3 → 3/2/2 changed nothing (four held-open idle sockets, no recovery in 60 s). Recovery
came only when the peer closed. So the "~25 s reaper latency" caveat above understates it:
for a live-but-idle peer the latency is unbounded. The candidate fix is draining the accept
backlog per tick so the existing 200 ms `SO_RCVTIMEO` close can reach those sockets;
`OEVM_HTTP_INFLIGHT` capping is still not the lever, because these connections are
un-accepted.
