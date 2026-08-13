#!/usr/bin/env python3
"""Generate the rete orientation deck  —  docs/status/<date>-where-we-are.html

    python3 docs/status/make-deck.py            # refetch issues, rebuild
    python3 docs/status/make-deck.py --offline  # rebuild from the cached dump

THE DECK IS TWO DIFFERENT KINDS OF THING AND THEY AGE AT DIFFERENT RATES.

  * The issue tables and the counts are GENERATED, from `gh issue list`. Rerun
    this and they are current. Nothing in them is hand-transcribed.
  * Every prose slide is a DATED JUDGEMENT written by hand on PROSE_DATE --
    what is green, what is broken, what to do next. Rerunning does not refresh
    a word of it.

So a rerun a month from now produces a deck whose tables are honest and whose
opinions are a month stale, which is the exact shape of a lie this project has
been bitten by before. The guard: PROSE_DATE below is stamped into the deck
beside the fetch date, and when they differ the title slide says so in a band
you cannot miss. Fix it by editing the prose and moving PROSE_DATE -- never by
moving PROSE_DATE alone.
"""
import json, re, html, collections, pathlib, subprocess, sys, datetime

# The date the JUDGEMENTS in this deck were written. See the docstring: bump it
# only when the prose has actually been re-read against the tree.
PROSE_DATE = "2026-08-12"

HERE = pathlib.Path(__file__).parent
SRC = HERE / "issues.json"
OUT = HERE / f"{PROSE_DATE}-where-we-are.html"

GH = ["gh", "issue", "list", "--state", "open", "--limit", "400",
      "--json", "number,title,labels",
      "--jq", "[.[] | {n:.number, t:.title, l:[.labels[].name]}] | sort_by(.n)"]

if "--offline" not in sys.argv:
    try:
        SRC.write_text(subprocess.run(GH, cwd=HERE, capture_output=True,
                                      text=True, check=True).stdout)
    except Exception as e:                       # no gh, no network, no auth
        if not SRC.exists():
            sys.exit(f"could not fetch issues and no cached {SRC.name}: {e}")
        print(f"warning: refetch failed ({e}); using the cached dump", file=sys.stderr)

issues = json.load(open(SRC))
FETCH_DATE = datetime.date.fromtimestamp(SRC.stat().st_mtime).isoformat()
STALE = FETCH_DATE != PROSE_DATE

# ---- classification -------------------------------------------------------
# NO THEME CLASSIFIER, deliberately. One was written and deleted: grouping the
# titles by keyword produced counts that swung by 3x depending on the ORDER of
# the rules (an issue mentioning "test" on a board driver lands wherever the
# rules happen to reach it first). A number that unstable is an instrument that
# reports confidently and measures nothing, which is the failure mode this deck
# spends a whole slide on. Slide 17 states the same judgement as prose instead.

def band(i):
    for b in ("P1", "P2", "P3"):
        if b in i["l"]:
            return b
    return "someday" if "someday" in i["l"] else "unlabelled"

ERAS = [(60,  "VM &amp; milestones", "to June"),
        (110, "Boards &amp; bring-up", "July"),
        (160, "porta rewrite", "late July – early Aug"),
        (999, "Current work", "August")]
def era(i):
    for hi, name, when in ERAS:
        if i["n"] <= hi:
            return name
    return ERAS[-1][1]

for i in issues:
    i["band"], i["era"] = band(i), era(i)

by_band = collections.Counter(i["band"] for i in issues)
by_era = collections.Counter(i["era"] for i in issues)
TOTAL = len(issues)

def rows(items):
    out = []
    for i in items:
        extra = " ".join(l for l in i["l"] if l not in ("P1", "P2", "P3", "someday"))
        tag = f'<span class="tag">{html.escape(extra)}</span>' if extra else ""
        out.append(f'<tr><td class="num">#{i["n"]}</td>'
                   f'<td>{html.escape(i["t"])} {tag}</td></tr>')
    return "\n".join(out)

def table(items):
    return f'<div class="tw"><table class="issues"><tbody>{rows(items)}</tbody></table></div>'

def pick(b):
    return sorted([i for i in issues if i["band"] == b], key=lambda x: x["n"])

def chunk(lst, n):
    return [lst[k:k + n] for k in range(0, len(lst), n)]

# ---- slide helpers --------------------------------------------------------
SL = []
def slide(section, title, body, eyebrow=None, cls=""):
    SL.append((section, title, body, eyebrow, cls))

def bar(counter, order=None, hue="var(--accent)"):
    keys = order or [k for k, _ in counter.most_common()]
    mx = max(counter[k] for k in keys)
    out = ['<div class="bars">']
    for k in keys:
        v = counter[k]
        out.append(f'<div class="bar"><span class="bl">{k}</span>'
                   f'<span class="bt"><i style="width:{100*v/mx:.1f}%;background:{hue}"></i></span>'
                   f'<span class="bv">{v}</span></div>')
    out.append("</div>")
    return "".join(out)

def board(items):
    out = ['<div class="statusgrid">']
    for name, state, note in items:
        out.append(f'<div class="srow"><span class="dot {state}"></span>'
                   f'<span class="sname">{name}</span>'
                   f'<span class="sstate {state}">{state}</span>'
                   f'<span class="snote">{note}</span></div>')
    out.append("</div>")
    return "".join(out)

# ==========================================================================
#  SECTION 1 — THE SHAPE
# ==========================================================================
STALE_BAND = (f"""<div class="stale"><b>These slides disagree with their own data.</b>
The issue tables were fetched <b>{FETCH_DATE}</b>; every judgement in the prose was written
<b>{PROSE_DATE}</b> and has not been re-read since. Trust the tables; check the prose against the
tree before acting on it.</div>""" if STALE else "")

p1n = sum(1 for i in issues if i["band"] == "P1")
slide("shape", "rete", STALE_BAND + f"""
<p class="lede">One repository. Two halves that never import each other. One wire between them.</p>
<div class="titlefacts">
  <div><b>{PROSE_DATE}</b><span>branch <code>190-alloc-seam</code>, pushed</span></div>
  <div><b>{TOTAL} open issues</b><span>{p1n} labelled P1 &mdash; three of them inherited</span></div>
  <div><b>3 boards live</b><span>one of them is a bell in a house</span></div>
</div>
<p class="note">The system works end to end today. What is unresolved is mostly <b>safety and
observability</b>, not function. If you read four slides, read <b>05</b> (status), <b>10</b> (why the
tracker misleads), <b>11</b> (the one thing that can hurt something) and <b>19</b> (what to do next).</p>
""", eyebrow="where we are", cls="title")

DIAGRAM = """
<figure class="fig">
<svg viewBox="0 0 920 300" role="img" preserveAspectRatio="xMidYMid meet"
     aria-label="An operator writes goal paths into porta, the gateway. porta holds a mirror of each node's path tree. The node starts both exchanges: it pulls the writes waiting for it, and pushes what it has observed, over UDP carrying CBOR on a Thread mesh.">
  <defs>
    <marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">
      <path d="M0,0 L10,5 L0,10 z" fill="currentColor"/>
    </marker>
  </defs>
  <g fill="none" stroke="currentColor" stroke-width="1.4">
    <rect x="16" y="112" width="128" height="66" rx="4" opacity=".5"/>
    <rect x="248" y="52" width="212" height="186" rx="5"/>
    <rect x="668" y="90" width="232" height="110" rx="5"/>
    <line x1="248" y1="118" x2="460" y2="118" opacity=".35"/>
  </g>

  <g text-anchor="middle" fill="currentColor">
    <text x="80" y="142" font-size="13" font-weight="700">operator</text>
    <text x="80" y="160" font-size="11" opacity=".7">or agent</text>

    <text x="354" y="82" font-size="16" font-weight="700">porta</text>
    <text x="354" y="102" font-size="11" opacity=".7">the gateway &mdash; Go, on gw85224-01</text>
    <text x="354" y="146" font-size="11.5" font-weight="700">a mirror of every node&rsquo;s tree</text>
    <text x="354" y="166" font-size="11" opacity=".7">cells &middot; streams &middot; append-only log</text>
    <text x="354" y="196" font-size="10.5" font-family="ui-monospace,monospace">goal/** obs/** tel/** sys/**</text>

    <text x="784" y="126" font-size="16" font-weight="700">a node</text>
    <text x="784" y="146" font-size="11" opacity=".7">the VM in C, and an nsl app</text>
    <text x="784" y="168" font-size="11" opacity=".7">owns the tree porta mirrors</text>
  </g>

  <g stroke="currentColor" stroke-width="1.4" fill="none" marker-end="url(#ar)">
    <line x1="146" y1="145" x2="242" y2="145"/>
    <line x1="466" y1="122" x2="662" y2="122"/>
    <line x1="662" y1="176" x2="466" y2="176"/>
  </g>

  <g text-anchor="middle" fill="currentColor">
    <text x="194" y="136" font-size="10.5" font-family="ui-monospace,monospace">writes goal/</text>
    <text x="564" y="112" font-size="11" font-weight="700">pull</text>
    <text x="564" y="196" font-size="11" font-weight="700">push</text>
    <text x="564" y="150" font-size="10.5" opacity=".7">what porta has for it</text>
    <text x="564" y="216" font-size="10.5" opacity=".7">what it has seen</text>
    <text x="564" y="256" font-size="10.5" opacity=".7">UDP carrying CBOR, over a Thread mesh</text>
    <text x="564" y="273" font-size="10.5" opacity=".7">512 B batch &middot; 256 B value &mdash; radio limits, not preferences</text>
  </g>
</svg>
<figcaption><b>The node starts both exchanges.</b> porta never calls a node &mdash; it answers a pull, then
accepts a push. Three real boards run this today; they are on slide 09.</figcaption>
</figure>
"""
slide("shape", "The whole system, once", DIAGRAM, eyebrow="the map")

slide("shape", "The one idea worth holding", """
<p class="lede">Everything else is detail hanging off this.</p>
<div class="two">
  <div class="card">
    <h3>A node owns a tree of paths</h3>
    <p>The operator writes what they <b>want</b> under <code>goal/</code>. The node writes what it
    <b>sees</b> under <code>obs/</code>, <code>tel/</code>, <code>sys/</code>. porta holds a mirror of that tree.</p>
  </div>
  <div class="card">
    <h3>Convergence is not a mechanism</h3>
    <p><code>goal/X</code> versus <code>obs/X</code> is a tree diff. The desired-vs-observed view and
    the self-heal loop are <b>the same query</b> &mdash; nobody had to build a reconciler.</p>
  </div>
</div>
<p class="note"><b>There are no verbs on the wire and none in the store.</b> <code>porta put</code> is a write
to a path; <code>porta get</code> is a tree read. Adding a feature adds a <b>path</b> &mdash; not a table,
not an endpoint, not a command. That is the decision the whole rewrite turned on.</p>
""", eyebrow="the model")

slide("shape", "What is actually in the repo", """
<div class="tree">
<div class="trow"><code>porta/</code><span>the gateway &mdash; Go. CLI, store (4 tables), UDP wire, hand-rolled CBOR codec.</span></div>
<div class="trow"><code>nodus/tuvm/</code><span>the VM &mdash; C, plus the Zephyr board ports. ~35 node apps live here.</span></div>
<div class="trow"><code>nodus/nsl/</code><span>the language, its compiler, and the kernel the nodes are written against.</span></div>
<div class="trow"><code>nodus/tests/</code><span>the nsl suite &mdash; the main gate.</span></div>
<div class="trow"><code>protocol/</code><span>the wire contract both sides answer to. <code>v1/</code> is frozen and historical.</span></div>
<div class="trow"><code>review/</code><span>your review-agent tree &mdash; the program ladder and the claims ledger.</span></div>
<div class="trow"><code>docs/</code><span>designs (decision records), plans (scaffolding), results (measurements).</span></div>
</div>
<p class="note">Being in one repo does not couple the two halves: they are joined <b>only</b> over the wire.
A change that makes one side compile against the other is a bug, not a convenience.</p>
""", eyebrow="the tree")

slide("shape", "Status board", board([
    ("porta &mdash; store, wire, CLI", "green", "4 tables, pull/push, hand-rolled CBOR. Go tests all pass."),
    ("The wire &mdash; space sync", "green", "Node-initiated pull/push, live on hardware for weeks."),
    ("nodus &mdash; the VM", "green", "Runs; the malloc-halt class was fixed and re-measured this week."),
    ("nodus &mdash; nsl + kernel", "green", "261 tests, 0 failed. The language is stable enough to build on."),
    ("Thread mesh radio", "green", "Was the big blocker. Four driver bugs fixed; one merged upstream."),
    ("Measurement &amp; profiling", "green", "Per-method call counts, allocation attribution, span latency. All measured, not projected."),
    ("Node liveness under stress", "amber", "The known death is fixed on branch; #192 is still unexplained."),
    ("Observability honesty", "amber", "Recurring theme: the instruments have lied more than the code has."),
    ("Gateway store as a hazard", "amber", "Two ways a legal porta write silently bricks a node. Neither is fixed."),
    ("Actuator safety", "red", "A failed flash can hold the bell&rsquo;s striker on, unbounded. Not fixed."),
    ("OTA / code push", "amber", "Works; a stuck node refetches ~200 kB every 6 s."),
    ("Auth, multi-tenant, fleet ops", "grey", "Deliberately not started."),
]), eyebrow="one screen")

slide("shape", "Verified today, not remembered", """
<p class="lede">Everything green above was re-run or re-read on 12 August. Dates matter here &mdash;
a green goes stale at the next edit.</p>
<div class="checks">
  <div class="chk ok"><b>porta</b><code>go test ./...</code><span>all packages ok</span></div>
  <div class="chk ok"><b>c6spare</b><code>TAP bridge-health done pass=3 fail=0</code><span>reflashed to the plain bridge, misses 0, dropped 0</span></div>
  <div class="chk ok"><b>c6-bell</b><code>check-in 2612 acked 11234</code><span>~21 h continuous, 69 misses (2.6%)</span></div>
  <div class="chk ok"><b>gateway</b><code>porta serve --db soak-2026-08-04.db</code><span>up on gw85224-01, serving one shared store</span></div>
  <div class="chk ok"><b>nsl suite</b><code>Passed: 261  Failed: 0  Skipped: 14</code><span>aux gates all pass &mdash; grant, debugger, portability-32, boot-gc-asan, deepsleep, mprof-tools, console</span></div>
  <div class="chk ok"><b>node&harr;porta</b><code>PORTA SYNC OK</code><span>all 8 legs, including the profile window opening and closing itself</span></div>
</div>
<p class="note"><b>Two things that green does not cover, printed by the suite itself.</b>
<code>footprint(T0.3): SKIP</code> &mdash; the MCU half did not run, because the ARM toolchain is not on
this PATH. And <code>target gates: NOT RUN BY THIS SUITE</code> &mdash; the host timer backend bypasses
the pulse mailbox, so coalescing, shedding and stale handles are untested here and need a board.
<b>A suite that announces its own blind spots is the whole reason to trust the rest of the line.</b></p>
""", eyebrow="evidence")

# ==========================================================================
#  SECTION 2 — THE HALVES
# ==========================================================================
slide("halves", "porta &mdash; the gateway half", """
<p class="lede">Latin for <i>gate</i>. Go. Runs on gw85224-01. v1 was deleted in one commit and rebuilt.</p>
<div class="two">
  <div class="card ok"><h3>Working</h3><ul>
    <li><b>Four tables</b> where v1 had nine &mdash; nodes, cells, events, exchanges.</li>
    <li><b>The wire</b> is a pure <code>Handle([]byte) []byte</code> with a thin UDP loop around it.</li>
    <li><b>A hand-rolled CBOR codec</b>, golden-tested against the node&rsquo;s own output. Not a library, on purpose.</li>
    <li><b>The CLI</b> &mdash; <code>serve</code>, <code>get</code>, <code>put</code>, <code>log</code>. Verbs live here and nowhere else.</li>
  </ul></div>
  <div class="card bad"><h3>Not working</h3><ul>
    <li><b>#180</b> &mdash; the store does not pin a path&rsquo;s kind, so a wrong-kind write is legal here and fatal there.</li>
    <li><b>#176</b> &mdash; a port collision on startup becomes a gateway outage.</li>
    <li><b>Docs are stale</b> &mdash; CLI, GETTING-STARTED and ARCHITECTURE still describe v1.</li>
    <li><b>#171</b> &mdash; <code>/health</code> still reports a v1 field that no longer exists.</li>
  </ul></div>
</div>
<p class="note">Operationally: <code>porta.service</code> is <b>disabled</b>; v2 is hand-started from
<code>~/porta-v2</code>. Restarting it costs every node an outage.</p>
""", eyebrow="north")

slide("halves", "nodus &mdash; the device half", """
<p class="lede">The VM in C, the language on top of it, and ~35 node applications.</p>
<div class="two">
  <div class="card ok"><h3>Working</h3><ul>
    <li><b>The VM runs</b> on ESP32, ESP32-S2, ESP32-C6 and nRF52840, under Zephyr.</li>
    <li><b>Thread mesh networking</b> &mdash; the year&rsquo;s biggest blocker, now closed.</li>
    <li><b>Live instruments</b>: per-method call counts, per-method allocation attribution, per-job latency spans.</li>
    <li><b>Code push</b> &mdash; a new image over the air, no reflash.</li>
  </ul></div>
  <div class="card bad"><h3>Not working</h3><ul>
    <li><b>#192</b> &mdash; one call on the check-in path hangs a C6. Mechanism still unknown.</li>
    <li><b>#191</b> &mdash; vtable growth may realloc static storage. Latent.</li>
    <li><b>#49</b> &mdash; a bad field type segfaults instead of raising.</li>
    <li><b>#53</b> &mdash; the original ESP32 has 816 bytes of DRAM spare. Nothing new fits.</li>
  </ul></div>
</div>
<p class="note">The lineage matters for how much to defend: <code>st/berry</code> &rarr; <code>ns/berry</code> &rarr;
<code>ns/oevm</code> &rarr; <code>nsl/tuvm</code>. This system has been rewritten wholesale four times.
Nothing here is built to last; it is built to teach.</p>
""", eyebrow="south")

slide("halves", "The bench, right now", """
<div class="bench">
  <div class="bcard live"><h3>c6-bell</h3><p class="role">the ship&rsquo;s bell &mdash; an actuator in a house</p>
    <ul><li>Soaking: check-in 2612, ~21 hours</li><li>69 misses (2.6%), 11 234 acked</li>
    <li><b>Never flash this without meaning to</b> &mdash; the interlock refuses it by default</li></ul></div>
  <div class="bcard live"><h3>c6spare</h3><p class="role">the disposable one &mdash; all this week&rsquo;s measurements</p>
    <ul><li>Just reflashed to the plain bridge</li><li>TAP 3/3, misses 0, dropped 0</li>
    <li>This is the board to experiment on</li></ul></div>
  <div class="bcard off"><h3>c6bench</h3><p class="role">unplugged since 6 August</p>
    <ul><li>Still has a collector entry</li><li>Resolve boards through the env files, never a table</li></ul></div>
</div>
<p class="note">The three C6 devkits are physically identical. <b>Only the USB serial hash tells them apart</b>,
and mistaking one for another has cost whole mornings.</p>
""", eyebrow="hardware")

# ==========================================================================
#  SECTION 3 — WHAT'S BROKEN
# ==========================================================================
slide("broken", "Before the issue list: the labels lie", """
<p class="lede">This is most of why the tracker feels overwhelming. It is not 136 live problems.</p>
<div class="two">
  <div class="card"><h3>Three of the six P1s are inherited</h3>
  <p><b>#12</b>, <b>#22</b> and <b>#31</b> were labelled in the VM-milestone era, months and one whole
  rewrite ago. They have not been re-read against today&rsquo;s tree. They may be dead, live, or
  irrelevant &mdash; nobody has checked.</p></div>
  <div class="card"><h3>One P1 is an umbrella, largely done</h3>
  <p><b>#120</b> is the tracker for &ldquo;scrap the command model, channels win&rdquo;. All three plans under it
  landed. It stays open as a heading, not as a defect &mdash; but it reads as a top-priority bug.</p></div>
</div>
<p class="note"><b>Two P1s are genuinely live and both are on this bench: #201 and #199.</b>
They are the next two slides. Everything else in the tracker can wait for you to want it.</p>
""", eyebrow="read this first")

slide("broken", "RED &mdash; the bell can stick on", """
<p class="lede">The only issue here that can hurt something outside a computer.</p>
<div class="incident">
  <div class="when">11 Aug, 23:01</div>
  <div class="what">A flash of <code>c6-bell</code> failed. esptool reset the chip into the ROM
  downloader and lost it. <b>The striker stayed energised and the bell rang continuously for two
  minutes, at eleven at night</b>, stopping only because your son unplugged the cable.</div>
</div>
<div class="two">
  <div class="card"><h3>Why it happens</h3><p>The ROM downloader drives no GPIOs. Whatever state the pin
  was left in persists, unbounded, with no software anywhere in a position to intervene &mdash; the VM
  is not running.</p></div>
  <div class="card"><h3>What closes it</h3><p><b>#202</b>: bound the on-time <i>outside</i> the VM.
  AC-couple the drive so a stuck pin cannot hold the load on. <b>#201</b> asks the narrower question first
  &mdash; does a pull-down resistor close the two-minute case? One meter reading answers it.</p></div>
</div>
<p class="note">Your own priority correction, 12 Aug: <i>&ldquo;I don&rsquo;t care about 1 sec sound at flash,
it&rsquo;s when it sticks on I care.&rdquo;</i> That is what #202 is scoped to.</p>
""", eyebrow="#201 / #202 &middot; safety")

slide("broken", "RED &mdash; a node can die while reporting itself healthy", """
<div class="incident">
  <div class="when">11 Aug, 15:38</div>
  <div class="what">A node whose frames the gateway rejected ran out of GC arena and halted in
  <b>2 min 37 s</b>. For all 46 ticks before it died it printed <code>misses 0</code> &mdash;
  the number that is supposed to mean &ldquo;healthy&rdquo;.</div>
</div>
<div class="two">
  <div class="card ok"><h3>Half of it is fixed</h3><p>The allocation leak behind the death closed with
  <b>#161</b>&rsquo;s fix. On the same harness: dies at 114 s before, <b>alive at 900 s / 450 check-ins</b>
  after &mdash; which is the control&rsquo;s own count, so refused and accepted nodes now last equally long.</p></div>
  <div class="card bad"><h3>Half of it is not</h3><p>The observability half. <code>misses</code> counts
  replies that did not arrive; it cannot see a reply that arrived and was <i>rejected</i>. A node being
  refused looks identical to a node that is fine, right up until it halts.</p></div>
</div>
<p class="note">The general shape, and it recurs: <b>the instrument agreed with the happy case and had no
opinion about the failing one.</b> That is the theme two slides on.</p>
""", eyebrow="#199 &middot; liveness")

slide("broken", "AMBER &mdash; the gateway store can brick a node", """
<p class="lede">Two independent ways a perfectly legal write to porta silently kills a node at
<b>every</b> boot afterwards. Reflashing does not help. Only editing the store does.</p>
<div class="two">
  <div class="card bad"><h3>#180 &mdash; wrong kind</h3><p>The store does not pin whether a path is a
  cell or a stream, so a <code>put</code> of the wrong kind is accepted. The node it reaches hangs.</p></div>
  <div class="card bad"><h3>#206 &mdash; right kind, wrong count</h3><p>More than one replayable
  <code>dbg/req</code> profile request wedges the node when the window ends. Correctly typed rows.
  Proven four ways, including by <b>deleting</b> two rows and watching the node recover.</p></div>
</div>
<p class="note">Why this is worse than it sounds: <b>a cold boot replays every request ever written</b>,
so the hazard is permanent and arms itself on the next reset &mdash; long after whoever typed the command
has forgotten. Driving a profile window is <i>itself</i> the thing that arms #206.</p>
""", eyebrow="#180 / #206 &middot; the store")

slide("broken", "AMBER &mdash; the instruments lie more than the code does", """
<p class="lede">Not a bug list. A pattern, and the most expensive one in the project.</p>
<div class="pattern">
  <div class="prow"><code>misses 0</code><span>said healthy for the whole 2 min 37 s of a node dying (#199)</span></div>
  <div class="prow"><code>largestFreeBlock</code><span>reads whatever the <i>failed</i> allocation left behind &mdash; 26% of samples look alarming and mean nothing</span></div>
  <div class="prow"><code>GC_STRESS green</code><span>is not evidence an object was rooted; the collector sweeps in-arena so dangling pointers still read fine (#193)</span></div>
  <div class="prow"><code>gate passed</code><span>eighteen recorded ways a gate has gone green having tested nothing</span></div>
  <div class="prow"><code>14 rounds measured</code><span>was 8 &mdash; a per-day log plus a per-boot counter silently paired one build&rsquo;s data with another&rsquo;s, <b>yesterday</b></span></div>
</div>
<p class="note">The working rule that came out of it: <b>gate on the failure reproducing, never on the metric
improving</b> &mdash; a 9&times; win once still let the node die.</p>
""", eyebrow="the recurring theme")

slide("broken", "AMBER &mdash; the ceilings", """
<div class="two">
  <div class="card"><h3>Memory</h3><ul>
    <li><b>#53</b> the original ESP32: <b>816 bytes</b> of DRAM spare. Effectively full.</li>
    <li><b>#196</b> two board layout files are stale, so those targets now fail at bake.</li>
    <li>The C6 dissolved this ceiling, which is why it became the dev target.</li>
  </ul></div>
  <div class="card"><h3>Measurement debt</h3><ul>
    <li><b>#143</b> footprint baselines stale since 18 July.</li>
    <li><b>#177</b> the size ratchet does not measure two of the newest source files.</li>
    <li><b>#31</b> the footprint gate <i>skips silently</i> without the ARM toolchain.</li>
  </ul></div>
</div>
<p class="note">These share a shape: <b>a limit that is only visible when something is already against it</b>.
None is urgent today; all get expensive the moment a new feature needs room.</p>
""", eyebrow="capacity")

# ==========================================================================
#  SECTION 4 — SHAPE OF THE BACKLOG
# ==========================================================================
era_order = ["VM &amp; milestones", "Boards &amp; bring-up", "porta rewrite", "Current work"]
slide("backlog", "Where the 136 issues actually come from", f"""
<p class="lede">By the era that opened them &mdash; which is objective, since issue numbers only go up.</p>
{bar(by_era, era_order, "var(--porta)")}
<p class="note">Roughly <b>{by_era['VM &amp; milestones'] + by_era['Boards &amp; bring-up']} of {TOTAL}</b>
were opened before the porta rewrite even started. They are not necessarily wrong &mdash; but they were
written against a system that has since been replaced, and almost none has been re-read since.</p>
""", eyebrow="by era")

slide("backlog", "What you can safely leave alone", """
<p class="lede">Deliberately not a chart. Keyword-grouping 136 titles gave answers that swung wildly
with the order of the rules &mdash; a number that unstable is the kind of instrument this project has
learned not to trust. So: a judgement, stated as one.</p>
<div class="tree">
<div class="trow"><code>per-device notes</code><span><b>Leave.</b> MagTag panels, S2 WiFi, nRF52840 pins, DAC quirks. They go quiet the moment you stop using that device, and several devices are already off the bench.</span></div>
<div class="trow"><code>upstream patches</code><span><b>Leave.</b> Zephyr contributions. Real work, zero urgency, and one already merged.</span></div>
<div class="trow"><code>pre-rewrite issues</code><span><b>Leave until touched.</b> Anything under #110 was written against a system that has since been replaced. Re-read one only when you are already in that code.</span></div>
<div class="trow"><code>someday</code><span><b>Leave.</b> This band already did the triage &mdash; it is the only place &ldquo;not now&rdquo; was written down on purpose.</span></div>
</div>
<div class="tree" style="margin-top:20px">
<div class="trow"><code>store hazards</code><span><b>Carry.</b> #180 and #206 arm themselves permanently and fire on a future reboot. They get harder to remember, not easier.</span></div>
<div class="trow"><code>actuator safety</code><span><b>Carry.</b> #201, #202. Physical, and the only item with a deadline set by something other than you.</span></div>
</div>
""", eyebrow="the honest cut")

slide("backlog", "What actually got finished recently", """
<div class="wins">
  <div class="win"><b>The Thread radio blocker</b><span>Four Zephyr 802.15.4 driver bugs, found and fixed.
  One is merged upstream. This was the thing standing in front of everything else.</span></div>
  <div class="win"><b>The malloc-halt class (#190, #205)</b><span>Symbols now carry their bytes inline.
  The node&rsquo;s C heap went from leaking a block per decoded symbol to <b>0 bytes in 0 allocations</b>,
  confirmed on hardware three separate ways.</span></div>
  <div class="win"><b>Allocation attribution (#203 stage 1)</b><span>Every allocation is now attributable
  to the nsl method that caused it, by name, on both heaps.</span></div>
  <div class="win"><b>The profiler&rsquo;s own cost (#203, #166)</b><span>Was a projection of ~0.02%, which
  turned out to be wrong by two orders. Now measured: <b>+1.9% of local CPU, +0.13% of elapsed</b>,
  and the activation hook alone at <b>323 ns</b>.</span></div>
  <div class="win"><b>The check-in decomposition (#107, #161)</b><span>A check-in is six spans, and the
  answer was 70% VM / 30% radio &mdash; the opposite of the expected culprit.</span></div>
</div>
""", eyebrow="the last two weeks")

slide("backlog", "Three honest options for what&rsquo;s next", """
<div class="opts">
  <div class="opt"><span class="ok">safety</span><h3>Close the actuator hazard</h3>
  <p>#201 then #202. One meter reading, then a hardware change. Small, bounded, and it is the only open
  item that can affect something physical at 11pm.</p></div>
  <div class="opt"><span class="warn">trust</span><h3>Make the instruments honest</h3>
  <p>#199&rsquo;s observability half, #180 and #206. The pattern that has cost the most time. Nothing new
  gets built on a base that reports itself healthy while dying.</p></div>
  <div class="opt"><span class="porta">reach</span><h3>Push the story outward</h3>
  <p>#198 gatekeeper (agent-facing queries), #181 (move the bench to the gateway host), #79 (is there a
  route to market at all). Everything here is optional and none of it is blocked.</p></div>
</div>
<p class="note">These are genuinely alternatives, not a sequence. The first is hours; the second is days;
the third is open-ended.</p>
""", eyebrow="pick one")

# ==========================================================================
#  SECTION 5 — THE TABLE
# ==========================================================================
slide("table", "Every open issue, by priority", f"""
<p class="lede">{TOTAL} open. The bands below are the labels as they stand &mdash; remember slide 10:
three of the six P1s predate the current system.</p>
{bar(by_band, ["P1","P2","P3","unlabelled","someday"], "var(--ink-2)")}
<p class="note">The unlabelled majority is not a triage failure so much as a record of a project that
moved faster than its own labels. Treat number order as rough age order.</p>
""", eyebrow="reference")

p1 = pick("P1"); p2 = pick("P2"); p3 = pick("P3")
un = pick("unlabelled"); sd = pick("someday")

slide("table", f"P1 &mdash; {len(p1)} issues", table(p1) +
      '<p class="note"><b>#199</b> and <b>#201</b> are live. <b>#120</b> is an umbrella whose work landed. '
      '<b>#12</b>, <b>#22</b>, <b>#31</b> are from the VM era and unverified against today&rsquo;s tree.</p>',
      eyebrow="highest label")
slide("table", f"P2 &mdash; {len(p2)} issues", table(p2), eyebrow="reference")
for k, part in enumerate(chunk(p3, 11)):
    slide("table", f"P3 &mdash; {len(p3)} issues" + (f" ({k+1}/{len(chunk(p3,11))})" if len(p3) > 11 else ""),
          table(part), eyebrow="reference")
for k, part in enumerate(chunk(un, 22)):
    slide("table", f"Unlabelled &mdash; {len(un)} issues ({k+1}/{len(chunk(un,22))})",
          table(part), eyebrow="reference", cls="dense")
slide("table", f"someday &mdash; {len(sd)} issues", table(sd) +
      '<p class="note">Explicitly parked. This band is doing its job &mdash; it is the only part of the '
      'tracker where &ldquo;not now&rdquo; was written down on purpose.</p>', eyebrow="parked", cls="dense")

slide("table", "Where the truth lives", """
<p class="lede">When this deck goes stale &mdash; and it will, quickly &mdash; these are the sources.</p>
<div class="tree">
<div class="trow"><code>gh issue list</code><span>The queue. Close-out comments carry the verification detail and the deviations. Read them before re-litigating anything.</span></div>
<div class="trow"><code>docs/next-steps-overview.md</code><span>Direction: bench work, what gates a release, candidate targets.</span></div>
<div class="trow"><code>review/program-sequence.md</code><span>Your own ladder. The roadmap of record.</span></div>
<div class="trow"><code>review/tests/CLAIMS.md</code><span>Every claim the system makes, with the test that enforces it.</span></div>
<div class="trow"><code>docs/results/</code><span>Measurements, dated. Nothing here is a projection.</span></div>
<div class="trow"><code>docs/design/</code><span>Decision records &mdash; the <i>why</i>, kept even after the decision is superseded.</span></div>
</div>
<p class="note">One rule that keeps this navigable: <b>a rung is not closed until its durable knowledge has
left the plan and landed in a mainline document.</b> The dated design stays behind as history.</p>
""", eyebrow="after this deck")

# ==========================================================================
#  render
# ==========================================================================
SECTIONS = [("shape", "The shape"), ("halves", "The halves"),
            ("broken", "What&rsquo;s broken"), ("backlog", "The backlog"), ("table", "Reference")]

nav = "".join(
    f'<a class="rl" href="#s{next(k for k,s in enumerate(SL) if s[0]==sid)+1}" data-sec="{sid}">'
    f'<span class="rt"></span><span class="rn">{name}</span></a>'
    for sid, name in SECTIONS)

body = []
for n, (sec, title, content, eyebrow, cls) in enumerate(SL, 1):
    eb = f'<p class="eyebrow">{eyebrow}</p>' if eyebrow else ""
    body.append(f"""<section class="slide {cls}" id="s{n}" data-sec="{sec}">
  <div class="inner">{eb}<h2>{title}</h2>{content}</div>
  <span class="pn">{n:02d}<i>/{len(SL):02d}</i></span>
</section>""")

CSS = """
:root{
  --ground:#E7EAF0; --surface:#FFFFFF; --surface2:#F2F4F8;
  --ink:#141821; --ink2:#525C6C; --ink3:#7E8798;
  --rule:#CDD3DE; --rule2:#E1E6EE;
  --porta:#33528A; --nodus:#9C5D26; --accent:var(--porta);
  --ok:#24704E; --warn:#8A6410; --bad:#9E2F27; --grey:#77808F;
  --okbg:#E4F0EA; --warnbg:#F6EEDA; --badbg:#F6E4E2; --greybg:#EAEDF2;
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme="light"]){
    --ground:#0E1118; --surface:#161A23; --surface2:#1C222E;
    --ink:#E3E7EF; --ink2:#98A2B3; --ink3:#7A8494;
    --rule:#2A313E; --rule2:#222836;
    --porta:#8CAAE2; --nodus:#DF9F63; --accent:var(--porta);
    --ok:#5CBD91; --warn:#D6A944; --bad:#EB7A70; --grey:#8B94A3;
    --okbg:#14261E; --warnbg:#2A2312; --badbg:#2B1816; --greybg:#1D222C;
  }
}
:root[data-theme="dark"]{
  --ground:#0E1118; --surface:#161A23; --surface2:#1C222E;
  --ink:#E3E7EF; --ink2:#98A2B3; --ink3:#7A8494;
  --rule:#2A313E; --rule2:#222836;
  --porta:#8CAAE2; --nodus:#DF9F63; --accent:var(--porta);
  --ok:#5CBD91; --warn:#D6A944; --bad:#EB7A70; --grey:#8B94A3;
  --okbg:#14261E; --warnbg:#2A2312; --badbg:#2B1816; --greybg:#1D222C;
}
*{box-sizing:border-box}
body{
  margin:0; background:var(--ground); color:var(--ink);
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  font-size:16px; line-height:1.5; -webkit-font-smoothing:antialiased;
  scroll-snap-type:y proximity; scroll-behavior:smooth;
}
@media (prefers-reduced-motion:reduce){body{scroll-behavior:auto}}
code,.num,.eyebrow,.pn,.bv,.sstate,.tag{font-family:ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,monospace}

/* ---- slide frame ---- */
.slide{
  min-height:100svh; scroll-snap-align:start; position:relative;
  display:flex; align-items:center; padding:56px 40px 56px 190px;
  border-bottom:1px solid var(--rule2);
}
.inner{width:100%; max-width:1000px; margin:0 auto}
.pn{position:absolute; right:26px; bottom:22px; font-size:12px; color:var(--ink3); letter-spacing:.04em}
.pn i{font-style:normal; opacity:.55}
.eyebrow{
  margin:0 0 14px; font-size:11px; letter-spacing:.16em; text-transform:uppercase;
  color:var(--accent); font-weight:700;
}
h2{
  margin:0 0 22px; font-size:clamp(28px,3.4vw,44px); line-height:1.08;
  letter-spacing:-.021em; font-weight:800; text-wrap:balance;
}
h3{margin:0 0 8px; font-size:15px; letter-spacing:-.005em; font-weight:750}
p{margin:0 0 14px}
.lede{font-size:clamp(17px,1.5vw,20px); color:var(--ink2); max-width:62ch; line-height:1.45}
.note{
  margin-top:24px; padding-left:16px; border-left:2px solid var(--rule);
  font-size:14px; color:var(--ink2); max-width:74ch;
}
.note b{color:var(--ink)}
code{
  font-size:.88em; background:var(--surface2); border:1px solid var(--rule2);
  padding:.08em .34em; border-radius:3px; color:var(--ink);
}

/* ---- title slide ---- */
.slide.title h2{font-size:clamp(56px,9vw,120px); letter-spacing:-.045em; margin-bottom:8px}
.stale{margin:0 0 22px; padding:12px 16px; font-size:13.5px; color:var(--ink);
  background:var(--warnbg); border-left:3px solid var(--warn); border-radius:0 4px 4px 0; max-width:74ch}
.titlefacts{display:flex; flex-wrap:wrap; gap:28px 44px; margin:30px 0 0}
.titlefacts div{display:flex; flex-direction:column; gap:2px}
.titlefacts b{font-size:19px; letter-spacing:-.01em}
.titlefacts span{font-size:13px; color:var(--ink2)}

/* ---- rail ---- */
.rail{position:fixed; left:0; top:0; height:100svh; width:170px; padding:34px 0 34px 30px;
  display:flex; flex-direction:column; justify-content:center; gap:2px; z-index:5}
.rl{display:flex; align-items:center; gap:10px; text-decoration:none; padding:7px 0}
.rt{width:20px; height:2px; background:var(--rule); flex:none; transition:width .2s,background .2s}
.rn{font-size:12px; color:var(--ink3); letter-spacing:.01em}
.rl[aria-current="true"] .rt{width:34px; background:var(--accent)}
.rl[aria-current="true"] .rn{color:var(--ink); font-weight:650}
.rl:focus-visible{outline:2px solid var(--accent); outline-offset:3px; border-radius:3px}
@media (max-width:900px){
  .rail{display:none}
  .slide{padding:44px 22px 52px}
}

/* ---- cards ---- */
.two{display:grid; grid-template-columns:1fr 1fr; gap:18px; margin-top:6px}
@media (max-width:760px){.two{grid-template-columns:1fr}}
.card{background:var(--surface); border:1px solid var(--rule); border-radius:5px; padding:18px 20px}
.card p{margin:0; font-size:14px; color:var(--ink2)}
.card ul{margin:0; padding-left:17px; font-size:14px; color:var(--ink2)}
.card li{margin:0 0 7px}
.card li b{color:var(--ink)}
.card.ok h3{color:var(--ok)} .card.bad h3{color:var(--bad)}

/* ---- status board ---- */
.statusgrid{display:flex; flex-direction:column; border-top:1px solid var(--rule2)}
.srow{display:grid; grid-template-columns:14px 250px 74px 1fr; gap:14px; align-items:center;
  padding:9px 4px; border-bottom:1px solid var(--rule2); font-size:14px}
.dot{width:9px; height:9px; border-radius:50%}
.dot.green{background:var(--ok)} .dot.amber{background:var(--warn)}
.dot.red{background:var(--bad)} .dot.grey{background:var(--grey)}
.sname{font-weight:640}
.sstate{font-size:10.5px; letter-spacing:.08em; text-transform:uppercase; text-align:center;
  padding:2px 0; border-radius:3px}
.sstate.green{color:var(--ok); background:var(--okbg)}
.sstate.amber{color:var(--warn); background:var(--warnbg)}
.sstate.red{color:var(--bad); background:var(--badbg)}
.sstate.grey{color:var(--grey); background:var(--greybg)}
.snote{color:var(--ink2); font-size:13.5px}
@media (max-width:760px){.srow{grid-template-columns:12px 1fr; }.sstate,.snote{grid-column:2}}

/* ---- checks ---- */
.checks{display:flex; flex-direction:column; gap:9px; margin-top:4px}
.chk{display:grid; grid-template-columns:96px auto 1fr; gap:14px; align-items:baseline;
  padding:10px 14px; background:var(--surface); border:1px solid var(--rule); border-radius:4px; font-size:14px}
.chk b{font-weight:700}
.chk code{background:none; border:none; padding:0; color:var(--ok); font-size:12.5px}
.chk.run code{color:var(--warn)}
.chk span{color:var(--ink2); font-size:13px}
@media (max-width:760px){.chk{grid-template-columns:1fr}}

/* ---- tree / rows ---- */
.tree{display:flex; flex-direction:column; border-top:1px solid var(--rule2)}
.trow{display:grid; grid-template-columns:210px 1fr; gap:18px; padding:9px 4px;
  border-bottom:1px solid var(--rule2); font-size:14px}
.trow code{background:none; border:none; padding:0; color:var(--accent); font-weight:600}
.trow span{color:var(--ink2)}
@media (max-width:760px){.trow{grid-template-columns:1fr; gap:2px}}

/* ---- incident ---- */
.incident{display:grid; grid-template-columns:120px 1fr; gap:20px; padding:16px 18px; margin-bottom:18px;
  background:var(--badbg); border-left:3px solid var(--bad); border-radius:0 4px 4px 0}
.when{font-family:ui-monospace,monospace; font-size:13px; font-weight:700; color:var(--bad)}
.what{font-size:14.5px; color:var(--ink)}
@media (max-width:760px){.incident{grid-template-columns:1fr; gap:6px}}

/* ---- pattern rows ---- */
.pattern{display:flex; flex-direction:column; border-top:1px solid var(--rule2)}
.prow{display:grid; grid-template-columns:210px 1fr; gap:18px; padding:10px 4px;
  border-bottom:1px solid var(--rule2); font-size:14px}
.prow code{background:var(--surface2); color:var(--bad); font-size:12.5px}
.prow span{color:var(--ink2)}
@media (max-width:760px){.prow{grid-template-columns:1fr; gap:3px}}

/* ---- bench ---- */
.bench{display:grid; grid-template-columns:repeat(3,1fr); gap:16px}
@media (max-width:860px){.bench{grid-template-columns:1fr}}
.bcard{background:var(--surface); border:1px solid var(--rule); border-radius:5px; padding:16px 18px;
  border-top:3px solid var(--rule)}
.bcard.live{border-top-color:var(--ok)} .bcard.off{border-top-color:var(--grey); opacity:.72}
.bcard h3{margin-bottom:2px; font-size:16px}
.role{font-size:12px; color:var(--ink3); margin:0 0 10px}
.bcard ul{margin:0; padding-left:16px; font-size:13.5px; color:var(--ink2)}
.bcard li{margin-bottom:5px}

/* ---- bars ---- */
.bars{display:flex; flex-direction:column; gap:8px; margin:18px 0 4px}
.bar{display:grid; grid-template-columns:190px 1fr 44px; gap:14px; align-items:center; font-size:14px}
.bl{color:var(--ink2)}
.bt{height:16px; background:var(--surface2); border-radius:2px; overflow:hidden}
.bt i{display:block; height:100%; border-radius:2px}
.bv{text-align:right; font-size:13px; font-weight:700; font-variant-numeric:tabular-nums}
@media (max-width:760px){.bar{grid-template-columns:120px 1fr 36px; gap:8px; font-size:13px}}

/* ---- wins / options ---- */
.wins{display:flex; flex-direction:column; gap:10px}
.win{display:grid; grid-template-columns:250px 1fr; gap:18px; padding:11px 0;
  border-bottom:1px solid var(--rule2); font-size:14px}
.win b{font-weight:700}
.win span{color:var(--ink2)}
@media (max-width:760px){.win{grid-template-columns:1fr; gap:3px}}
.opts{display:grid; grid-template-columns:repeat(3,1fr); gap:16px}
@media (max-width:860px){.opts{grid-template-columns:1fr}}
.opt{background:var(--surface); border:1px solid var(--rule); border-radius:5px; padding:16px 18px}
.opt span{display:inline-block; font-size:10.5px; letter-spacing:.1em; text-transform:uppercase;
  font-weight:700; padding:2px 7px; border-radius:3px; margin-bottom:9px;
  font-family:ui-monospace,monospace}
.opt span.ok{color:var(--bad); background:var(--badbg)}
.opt span.warn{color:var(--warn); background:var(--warnbg)}
.opt span.porta{color:var(--porta); background:var(--greybg)}
.opt p{margin:0; font-size:13.5px; color:var(--ink2)}

/* ---- issue tables ---- */
.tw{overflow-x:auto; max-height:70svh; overflow-y:auto; border:1px solid var(--rule);
  border-radius:4px; background:var(--surface)}
table.issues{border-collapse:collapse; width:100%; font-size:13.5px}
.issues td{padding:6px 12px; border-bottom:1px solid var(--rule2); vertical-align:top}
.issues tr:last-child td{border-bottom:none}
.num{width:62px; color:var(--accent); font-weight:700; font-variant-numeric:tabular-nums; white-space:nowrap}
.tag{font-size:10px; letter-spacing:.06em; text-transform:uppercase; color:var(--ink3);
  border:1px solid var(--rule); border-radius:3px; padding:0 4px; white-space:nowrap}
.slide.dense .tw{max-height:74svh}
.slide.dense table.issues{font-size:12.5px}
.slide.dense .issues td{padding:4.5px 10px}
"""

JS = """
(function(){
  var slides=[].slice.call(document.querySelectorAll('.slide'));
  var links=[].slice.call(document.querySelectorAll('.rl'));
  function mark(sec){links.forEach(function(l){l.setAttribute('aria-current',String(l.dataset.sec===sec));});}
  if('IntersectionObserver' in window){
    var io=new IntersectionObserver(function(es){
      es.forEach(function(e){ if(e.isIntersecting) mark(e.target.dataset.sec); });
    },{rootMargin:'-45% 0px -45% 0px'});
    slides.forEach(function(s){io.observe(s);});
  }
  mark('shape');
  function go(d){
    var y=window.scrollY, best=null;
    for(var i=0;i<slides.length;i++){
      var t=slides[i].offsetTop;
      if(d>0 && t>y+8){best=t;break;}
      if(d<0 && t<y-8){best=t;}
    }
    if(best!==null) window.scrollTo({top:best,behavior:'smooth'});
  }
  addEventListener('keydown',function(e){
    if(e.metaKey||e.ctrlKey||e.altKey) return;
    if(e.target.closest('.tw')) return;
    if(e.key==='ArrowDown'||e.key==='PageDown'||e.key===' '){e.preventDefault();go(1);}
    else if(e.key==='ArrowUp'||e.key==='PageUp'){e.preventDefault();go(-1);}
    else if(e.key==='Home'){e.preventDefault();window.scrollTo({top:0});}
    else if(e.key==='End'){e.preventDefault();window.scrollTo({top:document.body.scrollHeight});}
  });
})();
"""

OUT.write_text(f"""<title>rete &mdash; where we are</title>
<style>{CSS}</style>
<nav class="rail" aria-label="sections">{nav}</nav>
<main>
{chr(10).join(body)}
</main>
<script>{JS}</script>
""", encoding="utf-8")
print(f"wrote {OUT}  slides={len(SL)}  issues={TOTAL}")
print("bands:", dict(by_band))
print("eras:", dict(by_era))
