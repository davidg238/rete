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
    sinceStoredProgress \* porta's age since the STORED wak last increased — the exact
                        \* analogue of the node's own pulse_drained() deadline

vars == <<state, sinceProgress, sinceWedge, tickIn, sweepIn, sinceSeen, verdict, boots,
          wak, seenWak, sweepWak, sinceStoredProgress>>

Inc(x) == IF x < Cap THEN x + 1 ELSE Cap
Min2(a, b) == IF a < b THEN a ELSE b
Max2(a, b) == IF a > b THEN a ELSE b

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

(***************************************************************************)
(* The node's check-in. Two legs, and the whole of H1 is in `tickIn'`:      *)
(* when nothing comes back the node SPINS both reply budgets, so the        *)
(* DELIVERED period is Cad + 2*ReplyBudget while every deadline downstream  *)
(* is still derived from Cad.                                              *)
(***************************************************************************)
CheckIn ==
    /\ state = "running"
    /\ tickIn = 0
    /\ \E delivered \in BOOLEAN :
         /\ sinceSeen' = IF delivered THEN 0 ELSE Inc(sinceSeen)
         (* max, NOT sum, and the board is what settled it: the base tick is a
            PERIODIC timer whose posts coalesce, so a tick that overruns its own
            period does not push the next one out — the delivered period is
            max(Cad, work), not Cad + work. Modelled as a sum this predicted a
            6 s delivered tick under a silent uplink; c6spare measured 4.007 s,
            which is max(2000, 2 x 2000). *)
         /\ tickIn'    = IF delivered THEN Cad ELSE Max2(Cad, 2 * ReplyBudget)
         (* obs/health rides the PUSH leg, so porta learns the counter only on a
            check-in that lands — H5 in the same line as H3. *)
         /\ seenWak'   = IF delivered THEN Inc(wak) ELSE seenWak
         (* The age resets only on a sample that is GREATER. A reboot restarts the
            counter, so the post-reboot sample does not clear it — which is how the
            reboot loop is caught without ever reading boot_id. *)
         /\ sinceStoredProgress' = IF delivered /\ Inc(wak) > seenWak
                                     THEN 0 ELSE sinceStoredProgress
    /\ sinceProgress' = 0           \* the reactor ran: pulse_drained() moved
    /\ wak'           = Inc(wak)
    /\ UNCHANGED <<state, sinceWedge, sweepIn, verdict, boots, sweepWak>>

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

Wedge ==
    /\ state = "running"
    /\ state' = "wedged"
    /\ sinceWedge' = 0
    /\ UNCHANGED <<sinceProgress, tickIn, sweepIn, sinceSeen, verdict, boots,
                   wak, seenWak, sweepWak, sinceStoredProgress>>

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

DoSweep ==
    /\ sweepIn = 0
    /\ sweepIn' = Sweep
    /\ verdict' = IF sinceSeen = Cap /\ verdict = "neverSeen"
                    THEN "neverSeen"
                    ELSE IF IsLive THEN "live" ELSE "silent"
    /\ sweepWak' = seenWak
    /\ UNCHANGED <<state, sinceProgress, sinceWedge, tickIn, sinceSeen, boots,
                   wak, seenWak, sinceStoredProgress>>

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
    /\ UNCHANGED <<state, verdict, boots, wak, seenWak, sweepWak>>

Next == CheckIn \/ WedgedTick \/ Wedge \/ Detector \/ DoSweep \/ Tock

Spec == Init /\ [][Next]_vars /\ WF_vars(Tock) /\ WF_vars(DoSweep) /\ WF_vars(Detector)

TypeOK ==
    /\ state   \in {"running", "wedged"}
    /\ verdict \in {"neverSeen", "live", "silent"}
    /\ sinceProgress \in 0..Cap
    /\ sinceSeen     \in 0..Cap
    /\ boots         \in 0..MaxBoots
    /\ wak           \in 0..Cap
    /\ seenWak       \in 0..Cap
    /\ sweepWak      \in 0..Cap
    /\ sinceStoredProgress \in 0..Cap
    /\ Rule \in {"arrival", "sweepDelta", "progressAge"}

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

=============================================================================
