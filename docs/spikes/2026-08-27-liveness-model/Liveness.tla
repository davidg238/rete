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
    MaxBoots      \* stop counting reboots here, for the same reason

VARIABLES
    state,          \* "running" | "wedged"   — the GROUND TRUTH, which porta never sees
    sinceProgress,  \* since the reactor last drained a tick (pulse_drained advancing)
    sinceWedge,     \* since it wedged; what the observer measures
    tickIn,         \* countdown to the next check-in attempt
    sweepIn,        \* countdown to porta's next sweep
    sinceSeen,      \* porta's age since the last arrival — `now - last_seen`
    verdict,        \* "neverSeen" | "live" | "silent"
    boots           \* how many times the detector has rebooted this node

vars == <<state, sinceProgress, sinceWedge, tickIn, sweepIn, sinceSeen, verdict, boots>>

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
    /\ sinceProgress' = 0           \* the reactor ran: pulse_drained() moved
    /\ UNCHANGED <<state, sinceWedge, sweepIn, verdict, boots>>

(***************************************************************************)
(* The ISR keeps firing into a mailbox nobody drains. This is what makes    *)
(* the pulse-undrained condition the right one, and it is why a wedged node *)
(* is not simply an absent one.                                            *)
(***************************************************************************)
WedgedTick ==
    /\ state = "wedged"
    /\ tickIn = 0
    /\ tickIn' = Cad
    /\ UNCHANGED <<state, sinceProgress, sinceWedge, sweepIn, sinceSeen, verdict, boots>>

Wedge ==
    /\ state = "running"
    /\ state' = "wedged"
    /\ sinceWedge' = 0
    /\ UNCHANGED <<sinceProgress, tickIn, sweepIn, sinceSeen, verdict, boots>>

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
    /\ UNCHANGED <<sweepIn, sinceSeen, verdict>>

(***************************************************************************)
(* porta. THE RULE IS ONE LINE AND THERE ARE NO CONSTANTS IN IT:            *)
(* now - last_seen > bud. Note what it does not consult: boot_id, which is  *)
(* NOT NULL on every exchanges row.                                        *)
(***************************************************************************)
DoSweep ==
    /\ sweepIn = 0
    /\ sweepIn' = Sweep
    /\ verdict' = IF sinceSeen = Cap /\ verdict = "neverSeen"
                    THEN "neverSeen"
                    ELSE IF sinceSeen <= Bud THEN "live" ELSE "silent"
    /\ UNCHANGED <<state, sinceProgress, sinceWedge, tickIn, sinceSeen, boots>>

Tock ==
    /\ tickIn  > 0
    /\ sweepIn > 0
    /\ ~(state = "wedged" /\ sinceWedge >= WedgeDeadline)   \* the detector goes first
    /\ tickIn'        = tickIn - 1
    /\ sweepIn'       = sweepIn - 1
    /\ sinceProgress' = Inc(sinceProgress)
    /\ sinceSeen'     = Inc(sinceSeen)
    /\ sinceWedge'    = IF state = "wedged" THEN Inc(sinceWedge) ELSE 0
    /\ UNCHANGED <<state, verdict, boots>>

Next == CheckIn \/ WedgedTick \/ Wedge \/ Detector \/ DoSweep \/ Tock

Spec == Init /\ [][Next]_vars /\ WF_vars(Tock) /\ WF_vars(DoSweep) /\ WF_vars(Detector)

TypeOK ==
    /\ state   \in {"running", "wedged"}
    /\ verdict \in {"neverSeen", "live", "silent"}
    /\ sinceProgress \in 0..Cap
    /\ sinceSeen     \in 0..Cap
    /\ boots         \in 0..MaxBoots

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
(* H2: porta cannot form a verdict faster than its own sweep, so a budget   *)
(* shorter than SweepTick is a number the node cannot actually get.         *)
(***************************************************************************)
BudgetIsHonoured == Bud >= Sweep

=============================================================================
