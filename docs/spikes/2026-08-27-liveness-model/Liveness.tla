------------------------------ MODULE Liveness ------------------------------
(***************************************************************************)
(* rete — the liveness contract between ONE node and porta, over a channel *)
(* that may drop anything, as an executable model.                         *)
(*                                                                         *)
(* WHY. The failures this program keeps hitting are not logic errors in a  *)
(* single function; they are relations BETWEEN two independently-clocked   *)
(* halves — the node's declared cadence, its reply spin, the wedge         *)
(* detector's deadline, and porta's sweep. Each half is reviewed on its    *)
(* own and each is defensible on its own. This model is where they are     *)
(* put in one room.                                                        *)
(*                                                                         *)
(* UNITS ARE SCALED: one model step = 2 s of wall clock, so the bench      *)
(* node's 2 s cadence is Cad = 1. Every constant below is stated in those  *)
(* units and its wall-clock original is named beside it in the .cfg.       *)
(*                                                                         *)
(* WHAT IS DELIBERATELY *NOT* MODELLED: the CBOR wire shapes, the store,   *)
(* cursors and replay, more than one node. This is about time and health,  *)
(* and every variable that does not bear on those is state-space it would  *)
(* be paid for twice.                                                      *)
(***************************************************************************)
EXTENDS Integers

CONSTANTS
    Cad,          \* the node's DECLARED cadence
    ReplyBudget,  \* awaitReply, per leg — there are TWO legs per check-in
    Bud,          \* the silence budget the NODE declares; porta judges only this
    Sweep,        \* porta's liveness sweep period (SweepTick)
    Slack,        \* WEDGE_SLACK — the observer's deadline is Cad * Slack
    Cap,          \* ages saturate here; keeps the state space finite
    MaxBoots,     \* stop counting reboots here, for the same reason
    Rule          \* WHICH VERDICT RULE porta applies. Three, and the point of the
                  \* constant is that all three live in one spec so the shipped one
                  \* stays runnable beside its replacement (rete#272):
                  \*   "arrival"     now - last_seen <= bud     — what ships today
                  \*   "sweepDelta"  ... AND wak advanced between the last two sweeps
                  \*   "progressAge" now - last_PROGRESS <= bud  — the pulse_drained()
                  \*                 shape, one layer up
                  \*   "rateAge"     now - last_seen <= bud, AND wak moved at the
                  \*                 DECLARED rate between porta's last two samples, or
                  \*                 the verdict is "stalled" (rete#272 D3, 2026-09-14)

VARIABLES
    state,          \* "running" | "wedged"   — the GROUND TRUTH, which porta never sees
    sinceProgress,  \* since the reactor last drained a tick (pulse_drained advancing)
    sinceWedge,     \* since it wedged; what the observer measures
    tickIn,         \* countdown to the next check-in attempt
    sweepIn,        \* countdown to porta's next sweep
    sinceSeen,      \* porta's age since the last arrival — `now - last_seen`
    verdict,        \* "neverSeen" | "live" | "silent"
    boots,          \* how many times the detector has rebooted this node
    wak,            \* the node's own progress counter — obs/health.wak, the bridge's
                    \* `ticks`. Monotonic WITHIN a boot and RESET BY ONE, which is the
                    \* whole reason a reboot loop cannot hide inside it.
    seenWak,        \* the last wak porta has stored in `cells` — learned only when a
                    \* push leg actually lands
    sweepWak,       \* what seenWak read at the PREVIOUS sweep; the delta is the evidence
    sinceStoredProgress, \* porta's age since the STORED wak last increased — the exact
                        \* analogue of the node's own pulse_drained() deadline
    (* rete#272 D3 — the rate rule. Δwak is carried as a COUNT of ticks rather than as a
       difference of raw values, so it cannot saturate at Cap and it ignores the reboot
       reset. Both are OPTIMISTIC for porta: a real Δwak across a reboot is smaller, so a
       stale-live found here is one porta would have too. *)
    pendingTicks,   \* node: ticks since its last check-in that landed
    markAge,        \* porta: age of the sample it held at the previous sweep; Cap = none
    seenDelta,      \* porta: ticks between that sample and the latest one
    (* GHOSTS — ground truth porta never sees, only for the invariants. *)
    slowPending,    \* the gap that ends at the node's next arrival is slow
    windowFault,    \* a slow gap (or a wedge) lies inside porta's current window
    judgedFault,    \* windowFault as it stood when the current verdict was formed
    sinceOk         \* since the node last began a tick it delivered on cadence

vars == <<state, sinceProgress, sinceWedge, tickIn, sweepIn, sinceSeen, verdict, boots,
          wak, seenWak, sweepWak, sinceStoredProgress,
          pendingTicks, markAge, seenDelta, slowPending, windowFault, judgedFault, sinceOk>>
rateVars == <<pendingTicks, markAge, seenDelta, slowPending, windowFault, judgedFault, sinceOk>>

Inc(x) == IF x < Cap THEN x + 1 ELSE Cap
Min2(a, b) == IF a < b THEN a ELSE b
Max2(a, b) == IF a > b THEN a ELSE b
Inc2(x, n) == Min2(x + n, Cap)

WedgeDeadline == Cad * Slack

Init ==
    /\ state         = "running"
    /\ sinceProgress = 0
    /\ sinceWedge    = 0
    /\ tickIn        = Cad
    /\ sweepIn       = Sweep
    /\ sinceSeen     = Cap          \* never heard from
    /\ verdict       = "neverSeen"
    /\ boots         = 0
    /\ wak           = 0
    /\ seenWak       = 0
    /\ sweepWak      = 0
    /\ sinceStoredProgress = Cap
    /\ pendingTicks  = 0
    /\ markAge       = Cap
    /\ seenDelta     = 0
    /\ slowPending   = FALSE
    /\ windowFault   = FALSE
    /\ judgedFault   = FALSE
    /\ sinceOk       = 0

(***************************************************************************)
(* The node's check-in. Two legs, and the whole of H1 is in `tickIn'`:      *)
(* when nothing comes back the node SPINS both reply budgets, so the        *)
(* DELIVERED period is Cad + 2*ReplyBudget while every deadline downstream  *)
(* is still derived from Cad.                                              *)
(***************************************************************************)
CheckIn ==
    /\ state = "running"
    /\ tickIn = 0
    (* THREE outcomes. "deaf" is #271's measured case: the node's datagrams LAND but no
       reply comes back, so porta hears it on every tick and the node spins both reply
       budgets — arriving, but at half its declared rate. *)
    /\ \E outcome \in {"ok", "deaf", "lost"} :
       LET delivered == outcome # "lost" IN
         (* A lost check-in leaves porta's age ALONE — only Tock is a clock. Until
            2026-10-02 this read `ELSE Inc(sinceSeen)`, which aged porta's view a step
            per lost datagram with no time passing: the age ran ahead of the clock and
            the verdict went silent early, which can only HIDE a stale live. *)
         /\ sinceSeen' = IF delivered THEN 0 ELSE sinceSeen
         (* max, NOT sum, and the board is what settled it: the base tick is a
            PERIODIC timer whose posts coalesce, so a tick that overruns its own
            period does not push the next one out — the delivered period is
            max(Cad, work), not Cad + work. Modelled as a sum this predicted a
            6 s delivered tick under a silent uplink; c6spare measured 4.007 s,
            which is max(2000, 2 x 2000). *)
         /\ tickIn'    = IF outcome = "ok" THEN Cad ELSE Max2(Cad, 2 * ReplyBudget)
         (* obs/health rides the PUSH leg, so porta learns the counter only on a
            check-in that lands — H5 in the same line as H3. *)
         /\ seenWak'   = IF delivered THEN Inc(wak) ELSE seenWak
         (* The age resets only on a sample that is GREATER. A reboot restarts the
            counter, so the post-reboot sample does not clear it — which is how the
            reboot loop is caught without ever reading boot_id. *)
         /\ sinceStoredProgress' = IF delivered /\ Inc(wak) > seenWak
                                     THEN 0 ELSE sinceStoredProgress
         /\ pendingTicks' = IF delivered THEN 0 ELSE Inc(pendingTicks)
         /\ seenDelta'    = IF delivered THEN Inc2(seenDelta, pendingTicks + 1) ELSE seenDelta
         /\ windowFault'  = IF delivered THEN windowFault \/ slowPending ELSE windowFault
         (* "behind" is the DELIVERED period exceeding the declared one — not the
            outcome: with Cad >= 2 * ReplyBudget a deaf tick's spin fits inside the
            period and the node is on cadence. *)
         /\ slowPending'  = IF delivered THEN tickIn' > Cad   \* a new gap opens here
                            ELSE slowPending \/ tickIn' > Cad   \* the open gap goes on
         /\ sinceOk'      = IF tickIn' > Cad THEN sinceOk ELSE 0
    /\ sinceProgress' = 0           \* the reactor ran: pulse_drained() moved
    /\ wak'           = Inc(wak)
    /\ UNCHANGED <<state, sinceWedge, sweepIn, verdict, boots, sweepWak, markAge, judgedFault>>

(***************************************************************************)
(* The ISR keeps firing into a mailbox nobody drains. This is what makes    *)
(* the pulse-undrained condition the right one, and it is why a wedged node *)
(* is not simply an absent one.                                            *)
(***************************************************************************)
WedgedTick ==
    /\ state = "wedged"
    /\ tickIn = 0
    /\ tickIn' = Cad
    /\ UNCHANGED <<state, sinceProgress, sinceWedge, sweepIn, sinceSeen, verdict, boots,
                   wak, seenWak, sweepWak, sinceStoredProgress>>
    /\ UNCHANGED rateVars

Wedge ==
    /\ state = "running"
    /\ state' = "wedged"
    /\ sinceWedge' = 0
    /\ slowPending' = TRUE         \* the gap now open is the wedge itself
    /\ UNCHANGED <<sinceProgress, tickIn, sweepIn, sinceSeen, verdict, boots,
                   wak, seenWak, sweepWak, sinceStoredProgress>>
    /\ UNCHANGED <<pendingTicks, markAge, seenDelta, windowFault, judgedFault, sinceOk>>

(***************************************************************************)
(* The detector. It judges the node against the node's OWN declared period, *)
(* reboots, and the node comes back running — which is the part that makes  *)
(* a reboot loop look like speech.                                          *)
(***************************************************************************)
Detector ==
    /\ state = "wedged"
    /\ sinceWedge >= WedgeDeadline
    /\ state'         = "running"
    /\ boots'         = Min2(boots + 1, MaxBoots)
    /\ sinceWedge'    = 0
    /\ sinceProgress' = 0
    /\ tickIn'        = Cad
    (* THE RESET IS THE POINT. `ticks` is a per-boot counter, so a node that
       wedges and reboots comes back BELOW the value porta is holding — and a
       test for "did it advance" fails on it without ever reading boot_id. *)
    /\ wak'           = 0
    /\ UNCHANGED <<sweepIn, sinceSeen, verdict, seenWak, sweepWak, sinceStoredProgress>>
    /\ UNCHANGED rateVars

(***************************************************************************)
(* porta. THE RULE IS ONE LINE AND THERE ARE NO CONSTANTS IN IT:            *)
(* now - last_seen > bud. Note what it does not consult: boot_id, which is  *)
(* NOT NULL on every exchanges row.                                        *)
(***************************************************************************)
(* The three rules, side by side. Only `arrival` is what porta ships. *)
IsLive ==
    CASE Rule = "arrival"     -> sinceSeen <= Bud
      [] Rule = "sweepDelta"  -> sinceSeen <= Bud /\ seenWak > sweepWak
      [] Rule = "progressAge" -> sinceStoredProgress <= Bud
      [] Rule = "rateAge"     -> sinceSeen <= Bud

(* rateAge only: the elapsed time between porta's two samples, against the ticks the
   node counted in it. Short by a WHOLE declared period or more is "stalled". The one
   period of slack is the degenerate tolerance — the smallest that can absorb a real
   node's ms jitter — and is not tuned; a board run is what would earn a different one. *)
Behind ==
    /\ Rule = "rateAge"
    /\ markAge < Cap
    /\ (markAge - sinceSeen) - seenDelta * Cad >= Cad

DoSweep ==
    /\ sweepIn = 0
    /\ sweepIn' = Sweep
    /\ sweepWak' = seenWak
    /\ IF Rule = "rateAge" /\ seenDelta = 0 /\ markAge < Cap
       (* NO NEW SAMPLE SINCE THE MARK — no rate evidence. The window stays open across
          this sweep and the verdict keeps its last rate judgement; only the arrival age
          may still move it, to silent. The first version re-marked here, and TLC
          showed a stalled node flipping back to live on every empty window. *)
       THEN /\ verdict' = IF IsLive THEN verdict ELSE "silent"
            /\ UNCHANGED <<markAge, seenDelta, windowFault, judgedFault>>
       ELSE /\ verdict' = IF sinceSeen = Cap /\ verdict = "neverSeen"
                            THEN "neverSeen"
                            ELSE IF ~IsLive THEN "silent"
                            (* ONE sample says nothing about a rate: under rateAge porta
                               withholds live until it holds two. "neverSeen" stands in for
                               porta's #unknown — a verdict that already exists. *)
                            ELSE IF Rule = "rateAge" /\ markAge = Cap THEN "neverSeen"
                            ELSE IF Behind THEN "stalled" ELSE "live"
            (* the latest sample becomes the mark the next window is judged from *)
            /\ markAge'     = sinceSeen
            /\ seenDelta'   = 0
            /\ judgedFault' = windowFault
            /\ windowFault' = FALSE
    /\ UNCHANGED <<state, sinceProgress, sinceWedge, tickIn, sinceSeen, boots,
                   wak, seenWak, sinceStoredProgress, pendingTicks, slowPending, sinceOk>>

Tock ==
    /\ tickIn  > 0
    /\ sweepIn > 0
    /\ ~(state = "wedged" /\ sinceWedge >= WedgeDeadline)   \* the detector goes first
    /\ tickIn'        = tickIn - 1
    /\ sweepIn'       = sweepIn - 1
    /\ sinceProgress' = Inc(sinceProgress)
    /\ sinceSeen'     = Inc(sinceSeen)
    /\ sinceWedge'    = IF state = "wedged" THEN Inc(sinceWedge) ELSE 0
    /\ sinceStoredProgress' = Inc(sinceStoredProgress)   \* Tock is the only clock
    /\ markAge'      = Inc(markAge)
    /\ sinceOk'      = Inc(sinceOk)
    /\ UNCHANGED <<state, verdict, boots, wak, seenWak, sweepWak,
                   pendingTicks, seenDelta, slowPending, windowFault, judgedFault>>

Next == CheckIn \/ WedgedTick \/ Wedge \/ Detector \/ DoSweep \/ Tock

Spec == Init /\ [][Next]_vars /\ WF_vars(Tock) /\ WF_vars(DoSweep) /\ WF_vars(Detector)

TypeOK ==
    /\ state   \in {"running", "wedged"}
    /\ verdict \in {"neverSeen", "live", "silent", "stalled"}
    /\ sinceProgress \in 0..Cap
    /\ sinceSeen     \in 0..Cap
    /\ boots         \in 0..MaxBoots
    /\ wak           \in 0..Cap
    /\ seenWak       \in 0..Cap
    /\ sweepWak      \in 0..Cap
    /\ sinceStoredProgress \in 0..Cap
    /\ pendingTicks \in 0..Cap
    /\ markAge      \in 0..Cap
    /\ seenDelta    \in 0..Cap
    /\ slowPending \in BOOLEAN /\ windowFault \in BOOLEAN /\ judgedFault \in BOOLEAN
    /\ sinceOk      \in 0..Cap
    /\ Rule \in {"arrival", "sweepDelta", "progressAge", "rateAge"}

(***************************************************************************)
(* THE PROPERTY THAT MATTERS (rete H3).                                     *)
(*                                                                         *)
(* An operator reading `gw/liveness` believes it answers "is this node      *)
(* working?". This is that belief, written down: porta must not call a node *)
(* live while that node has made no progress for longer than its OWN wedge  *)
(* deadline — the very number the node published.                          *)
(*                                                                         *)
(* It is a SAFETY property, so a violation comes back as a short trace      *)
(* rather than a lasso.                                                    *)
(***************************************************************************)
NeverStaleLive == (verdict = "live") => (sinceProgress <= WedgeDeadline)

(***************************************************************************)
(* H1, as an invariant: the node's delivered period never exceeds the one   *)
(* it declared. Expected to FAIL the moment a reply goes missing — the      *)
(* point is the trace, which shows the spin extending the period that       *)
(* every downstream deadline was derived from.                              *)
(***************************************************************************)
DeliveredMatchesDeclared == tickIn <= Cad

(***************************************************************************)
(* VACUITY. A green run over a space where porta never says `live` would    *)
(* prove nothing at all, and that is exactly the false green this program   *)
(* keeps meeting. Checked as an invariant EXPECTED TO FAIL: a violation is  *)
(* the proof that `live` is reachable in the config being run.              *)
(***************************************************************************)
LiveIsReachable == verdict # "live"

(***************************************************************************)
(* H2: porta cannot form a verdict faster than its own sweep, so a budget   *)
(* shorter than SweepTick is a number the node cannot actually get.         *)
(***************************************************************************)
BudgetIsHonoured == Bud >= Sweep

(***************************************************************************)
(* rete#272 D3 — the three claims the rate rule makes, each checkable.      *)
(*                                                                         *)
(* NeverBehindLive: porta must not call a node live once it has gone       *)
(* Bud + Sweep + Cad without an on-cadence tick. That bound is TLC's answer, *)
(* not a choice: probed 2026-10-02 over seven settings of Cad/ReplyBudget/  *)
(* Bud/Sweep, it is the smallest that holds, tight in every one where a     *)
(* tick can run slow. The stall costs the age rule's own Bud + Sweep        *)
(* latency plus one period — the one period of slack in Behind.             *)
(*                                                                         *)
(* NoFalseStall: "stalled" is only ever said of a window that really held a *)
(* slow gap or a wedge. The per-exchange trap D3 names (a frozen wak on     *)
(* every push leg) is exactly a false stall, so this is the guard on it.    *)
(*                                                                         *)
(* StalledIsReachable: vacuity, as for LiveIsReachable — expected to FAIL.  *)
(***************************************************************************)
NeverBehindLive    == (verdict = "live") => (sinceOk <= Bud + Sweep + Cad)
NoFalseStall       == (verdict = "stalled") => judgedFault
StalledIsReachable == verdict # "stalled"

=============================================================================
