#ifndef OEVM_PRIM_IDS_H
#define OEVM_PRIM_IDS_H
/* Platform-capability primitive IDs shared between dispatch.c and platform.c.
   Values mirror the anonymous enum in dispatch.c (std-lib step 3c-3b task 1).
   dispatch.c defines them authoritatively; these macros make them visible to
   platform.c (and future capability modules) without duplicating the enum.
   IMPORTANT: these values must stay in sync with the enum in dispatch.c.
   Task 9a: the 5 retired NLR/fiber prim enum slots (executeCleanup,
   resumeWith:, fiberCreate:, fiberSwitch:value:, currentFiber) were removed,
   shifting the platform prim IDs down by 5 (61->56 .. 71->66).
   Milestone B Task 1: 3 new Integer bitop prim enum slots (PRIM_bitAnd,
   PRIM_bitOr, PRIM_shr) were inserted before this group, shifting the
   platform prim IDs up by 3 (56->59 .. 66->69).
   Milestone B Task 3: 2 new RawBytes platform prims (PRIM_pf_basicNewBytes,
   PRIM_pf_freezeBytes) were appended AFTER this entire group (after the last
   existing slot, PRIM_pf_readLine) — no pre-existing id shifts; new ids are
   70/71. (at:/at:put:/size on a RawBytes receiver reuse the existing generic
   PRIM_at/PRIM_at_put/PRIM_size ids, gated by receiver vtable in dispatch.c;
   they need no prim_ids.h mirror since platform.c never sees them.)
   Milestone B Task 4: 1 new platform prim, PRIM_pf_valueRank, appended after
   PRIM_pf_freezeBytes — no pre-existing id shifts; new id is 72. The six
   predicate selectors (isValue/isSymbol/isList/isMap/isRecord/isBytes) get
   enum slots in dispatch.c's PRIM enum too (positions 1:1 with PRIM_NAMES)
   but are serviced entirely by a dispatch.c fast-path switch keyed on builtin
   vtables/SMI/nil/Boolean-singleton identity — platform.c never sees them, so
   they need no mirror here. */
#define PRIM_ix_array            59
#define PRIM_ix_basicAt          60
#define PRIM_ix_basicAt_put      61
#define PRIM_ix_basicSize        62
#define PRIM_pf_identicalTo      63
#define PRIM_pf_hashcode         64
#define PRIM_pf_print            65
#define PRIM_pf_printNewline     66
#define PRIM_pf_basicNewIndexable 67
#define PRIM_pf_string           68
#define PRIM_pf_readLine         69
#define PRIM_pf_basicNewBytes    70
#define PRIM_pf_freezeBytes      71
#define PRIM_pf_valueRank        72
/* Milestone C Task 1: channel-space C substrate prims (src/space.c). The
   task brief specified ids 73-76 for these, computed from this file's last
   MIRRORED id above (72, PRIM_pf_valueRank) without accounting for the 6
   value-algebra predicate enum slots (isValue/isSymbol/isList/isMap/
   isRecord/isBytes) that Milestone B Task 4 already appended at ids 73-78 in
   dispatch.c's PRIM enum — unmirrored here because platform.c never sees
   them (serviced entirely by dispatch.c's own fast-path switch; see that
   task's comment there). True append-only numbering — the binding
   constraint is that no EXISTING id may shift — is therefore 79-82, not
   73-76; flagged as a concern in the Task 1 report
   (.superpowers/sdd/mc-task-1-report.md). */
#define PRIM_pf_spacePost        79
#define PRIM_pf_spaceRegister    80
#define PRIM_pf_spaceTake        81
#define PRIM_pf_spaceDequeue     82
/* Milestone D Task 1: the setjmp crash-boundary primitive, appended after the
   entire space-C-substrate group — no pre-existing id shifts. 83 was arrived
   at by COUNTING PRIM_NAMES (82 entries before this one), not by 82+1
   arithmetic, per the lesson flagged in Milestone C Task 1's report above
   (6 unmirrored value-algebra predicate slots hide in the enum between ids
   72 and 79; they don't affect this count since this entry is appended after
   PRIM_pf_spaceDequeue, whose mirrored id (82) already accounts for them). */
#define PRIM_pf_protectedFire    83
/* Milestone D Task 4: the restart-sweep verb, appended after protectedFire —
   no pre-existing id shifts. 84 was arrived at by COUNTING PRIM_NAMES (84
   entries total after the append), not by 83+1 arithmetic, per the standing
   lesson above. */
#define PRIM_pf_spaceDropReactionsOwnedBy 84
/* Milestone D Task 6: the host virtual-clock timer seam, appended after
   spaceDropReactionsOwnedBy — no pre-existing id shifts. 85-89 were arrived
   at by COUNTING PRIM_NAMES (89 entries total after the append), not by
   84+1 arithmetic, per the standing lesson above. */
#define PRIM_pf_timerAfter        85
#define PRIM_pf_timerEvery        86
#define PRIM_pf_timerCancel       87
#define PRIM_pf_monotonicMillis   88
#define PRIM_pf_advanceMillis     89
/* Milestone E Task 1: the terminate-only raise verb (`aRecord raise`), appended
   after the entire timer group — no pre-existing id shifts. 90 was arrived at by
   COUNTING PRIM_NAMES (89 entries before this one), not by 89+1 arithmetic, per
   the standing lesson above. Serviced entirely in dispatch.c's prim switch
   (vm_raise); platform.c never sees it, but it is mirrored here for symmetry. */
#define PRIM_raise                90
/* Milestone E Task 1: the tagged-raise verb (`[text] error: [tag]`), appended
   after PRIM_raise — no pre-existing id shifts. 91 was arrived at by COUNTING
   PRIM_NAMES (90 entries before this one), not by 90+1 arithmetic, per the
   standing lesson above. Serviced entirely in dispatch.c's prim switch (builds
   the crash Record via the captured kernel instance, then vm_raise). */
#define PRIM_errorTag             91
/* Zephyr rung-2 sub-project 1 Task 3: host GPIO prims + pumpEvents, appended
   after the entire Milestone E group (on:do:/ensure: have no mirrors here,
   being serviced entirely inside dispatch.c's own prim switch) — no
   pre-existing id shifts. 94-96 were arrived at by COUNTING PRIM_NAMES (93
   entries before this append), not by 91+1 arithmetic, per the standing
   lesson above. */
#define PRIM_pf_gpioPin      94
#define PRIM_pf_gpioFire     95
#define PRIM_pf_pumpEvents   96
/* Rung 3 Task 2: CBOR integer leaf (`cborEncodeInt:`), appended after the entire
   Zephyr rung-2 GPIO/pumpEvents group — no pre-existing id shifts. 97 was arrived
   at by COUNTING PRIM_NAMES (96 entries before this append), not by 96+1
   arithmetic, per the standing lesson above. Serviced in platform.c's
   platform_dispatch switch (used by the Task 5 encode walk). */
#define PRIM_pf_cborEncodeInt 97
/* Rung 3 Task 3: CBOR float leaf (`cborEncodeFloat:`), appended after
   cborEncodeInt: — no pre-existing id shifts. 98 was arrived at by COUNTING
   PRIM_NAMES (97 entries before this append), not by 97+1 arithmetic, per the
   standing lesson above. Serviced in platform.c's platform_dispatch switch
   (used by the Task 5 encode walk). Encodes a Float as CBOR major 7, the
   shortest of f16/f32/f64 that round-trips the double exactly. */
#define PRIM_pf_cborEncodeFloat 98
/* Rung 3 Task 4: the four CBOR DECODE leaves, appended after cborEncodeFloat: —
   no pre-existing id shifts. 99-102 were arrived at by COUNTING PRIM_NAMES (98
   entries before this append), not by 98+1 arithmetic, per the standing lesson
   above. All four are serviced in platform.c's platform_dispatch switch (used
   by the Task 6 decode walk). */
#define PRIM_pf_cborReadHead     99
#define PRIM_pf_cborIntFromMajor 100
#define PRIM_pf_cborReadFloat    101
#define PRIM_pf_utf8Valid        102
/* Rung 3 Task 6: String->Symbol leaf (`asSymbol:`), appended after utf8Valid: —
   no pre-existing id shifts. 103 was arrived at by COUNTING PRIM_NAMES (102
   entries before this append), not by 102+1 arithmetic, per the standing lesson
   above. Platform-dispatched (mirrored here as 103). Used by the Task 6 decode
   walk to rebuild Symbols for CBOR tag 39 (and, transitively, Record tags). */
#define PRIM_pf_asSymbol         103
/* Rung 4 (on-target measurement) Task 1: heap-telemetry leaf (`heapStats`),
   appended after asSymbol: — no pre-existing id shifts. 104 was arrived at by
   COUNTING PRIM_NAMES (103 entries before this append), not by 103+1 arithmetic,
   per the standing lesson above. Platform-dispatched (mirrored here as 104).
   Returns a 5-slot Array [heapSize, liveBytes, highWater, largestFreeBlock,
   gcCycles] snapshotting src/gc.c oevm_heap_stats(); the nsl HeapStats class
   names the fields. */
#define PRIM_pf_heapStats 104
/* Rung 9 (cells — tuvm#32 option A) Task 2: the cell seam, appended after
   heapStats — no pre-existing id shifts. 105/106/107 were arrived at by COUNTING
   PRIM_NAMES (104 entries before this append), not by 104+1 arithmetic, per the
   standing lesson above.
     spaceKind:      -> SMI 0 unset / 1 stream / 2 cell. Does NOT intern: asking
                        about a name must not create it (space_take's rule).
     spacePeek:      -> the cell's value, non-destructively. nil if not a cell.
     spacePut:value: -> writes the value slot, THEN space_post_id's it so
                        `whenever:` reactions fire through the existing reactor.
   `kind == CELL` is itself the has-a-value flag (only put sets it, and put always
   stores), so "wrote nil" and "never written" are distinguishable — which is why
   Channel>>peek:ifAbsent: is correct without waiting on tuvm#21. */
#define PRIM_pf_spaceKind 105
#define PRIM_pf_spacePeek 106
#define PRIM_pf_spacePut  107
/* tuvm#20 (G2 overflow policy), appended after spacePut: — no pre-existing id
   shifts; 108 arrived at by COUNTING PRIM_NAMES (107 entries before this
   append), not by 107+1 arithmetic, per the standing lesson above.
     spaceOverruns: -> SMI count of posts shed by the full event ring on that
                       exact channel; '' (empty path) -> the node total. Does
                       NOT intern (space_take's rule: asking about a name must
                       not create it). Cumulative, never reset — the pull side
                       of G2's "never silent"; the sys/metrics push side is
                       tuvm#24. */
#define PRIM_pf_spaceOverruns 108
/* tuvm#24: sys-telemetry leaf (`sysStats`), appended after spaceOverruns: — no
   pre-existing id shifts. 109 arrived at by COUNTING PRIM_NAMES (108 entries
   before this append), not by 108+1 arithmetic, per the standing lesson above.
     sysStats -> 4-slot Array of SMIs [handlesLive, handlesHighWater,
                 queueHighWater, totalOverruns] (heapStats' shape, prim 104).
                 The push-side sys/metrics record is composed in nsl
                 (kernel metrics) and published by the APP onto a cell — the
                 kernel never writes the space (capability rule). */
#define PRIM_pf_sysStats 109
/* webserver-channel-device spike, Task 3: the http driver's platform prims,
   appended after sysStats — no pre-existing id shifts. 110-112 were arrived at
   by COUNTING PRIM_NAMES (109 entries before this append), not by 109+1
   arithmetic, per the standing lesson above.
     httpListen: port                     -> stub, answers self (real in Task 6).
     httpRespondId: id status: n body: s  -> frames + emits the reply via the
                                              http_backend seam (http_respond).
     httpInject: method path: p           -> test-seam request injection
                                              (http_accept_request with fd=-1,
                                              framed reply goes to stdout). */
#define PRIM_pf_httpListen  110
#define PRIM_pf_httpRespond 111
#define PRIM_pf_httpInject  112
/* streaming-render (2026-07-18): chunked egress prims — appended after
   httpInject, no pre-existing id shifts.
     httpBeginId: id status: st  -> opens a chunked response (http_begin).
     httpChunkId: id data: s     -> writes one chunk (http_chunk).
     httpEndId: id               -> closes the chunked response (http_end). */
#define PRIM_pf_httpBegin  113
#define PRIM_pf_httpChunk  114
#define PRIM_pf_httpEnd    115
/* AMG8833 thermal rung (2026-07-19): the sensor seam.
     sensorFrame                                 -> 64-slot Array of centi-degC
                                                    SMIs, or nil if not ready.
     sensorArmEventPost: msg on: path threshold: -> arm the driver threshold
                                                    trigger; a crossing posts msg
                                                    on the interned channel path
                                                    via pulse_post_isr. Same
                                                    String-path + handle_new shape
                                                    as gpioPin:edge:post:on:, and
                                                    the same answer as
                                                    rtcAlarmAt:post:on: — the
                                                    platform on success, NIL when
                                                    the backend cannot arm (rete#184:
                                                    it used to answer the platform
                                                    either way, reporting "armed"
                                                    for a trigger that was not). */
#define PRIM_pf_sensorFrame     116
#define PRIM_pf_sensorArmEvent  117
/* Rung 3 Task 4: the out-of-range sentinel returned by cborIntFromMajor:arg:
   for the one unrepresentable CBOR integer (major 1, arg 2^64-1 => value -2^64,
   just past the ratified +/-(2^64-1) range). In this VM nil IS the C NULL
   pointer, and the primitive-success path returns *out verbatim, so a leaf that
   sets `*out = CBOR_OOR_SENTINEL; return 1;` yields nil to the caller. A validly
   decoded integer is never NULL, so nil is an unambiguous OOR signal. The Task 6
   nsl decode walk checks `r == nil` (i.e. r == CBOR_OOR_SENTINEL) and fails. */
#define CBOR_OOR_SENTINEL NULL

/* tuvm#61 job latency: the span seam — spanBegin:/spanEnd:/spanStats:/spanRecord:us:.
   Appended after sensorArmEvent; no pre-existing id shifts. 118-121 were arrived at
   by COUNTING PRIM_NAMES (117 entries before this append), not by 117+1 arithmetic,
   per the standing rule above. The slots these address are NAMELESS in C — the
   name<->slot table lives in nsl's Spans class, the same division timer_backend.h
   states for the timer seam. */
#define PRIM_pf_spanBegin  118
#define PRIM_pf_spanEnd    119
#define PRIM_pf_spanStats  120
#define PRIM_pf_spanRecord 121

/* tuvm#72 diag span capture: spanReset — a bounded capture window needs the
   accumulator zeroed at the start of it. Wraps span_reset_all(), which already
   existed for tests (src/span.c). Appended after spanRecord; no pre-existing id
   shifts. 122 was arrived at by COUNTING PRIM_NAMES (121 entries before this
   append), not by 121+1 arithmetic, per the standing rule above. Nullary and
   all-slots on purpose: a per-slot reset would let a caller clear half a window
   and publish the result as if it were whole. */
#define PRIM_pf_spanReset  122

/* tuvm#82 (MagTag LEDs): the OUTBOUND half of the gpio seam. gpioSet:to: drives a
   DECLARED pin to a LOGICAL level; gpioLevel: reads one back — and reads a button's
   held state, which is what lets chording cost zero mailbox traffic instead of the
   doubled event rate release events pay. Appended after spanReset; no pre-existing
   id shifts. 123/124 were arrived at by COUNTING PRIM_NAMES (122 entries before
   this append), not by 122+1 arithmetic, per the standing rule above.
   Both answer nil for a pin the board never declared. The raise is written in nsl
   (GpioPulse in kernel.nsl), not here — the same division Vector>>at: uses, and the
   only one available, since build_and_raise is static to dispatch.c. */
#define PRIM_pf_gpioSet   123
#define PRIM_pf_gpioLevel 124

/* tuvm#88 (MagTag e-paper): the display seam. clear/at:col:put: touch only the
   C-side character framebuffer and are cheap; refresh/refreshRow: drive the panel
   and BLOCK the VM for the paint — safe because the pulse mailbox sheds and
   coalesces rather than aborting (src/backend/zephyr/pulse_zephyr.c, tuvm#34), so
   a button pressed mid-paint is delivered once afterwards.
   Appended after gpioLevel; no pre-existing id shifts. 125-128 were arrived at by
   COUNTING PRIM_NAMES (124 entries before this append), not by 124+1 arithmetic,
   per the standing rule above.
   ANSWER CONVENTION, an extension of the gpio pair's: nil still means "this board
   has no such thing" (no display bound at all) and is the structural error a wrong
   literal produces. An operational outcome the driver knows about answers an SMI
   instead — 0 ok, 1 out of bounds, 2 driver fault — because one nil cannot carry
   three different tags. The raises are written in nsl (Display in kernel.nsl), not
   here, the same division Vector>>at: uses over basicAt:. */
#define PRIM_pf_displayClear      125
#define PRIM_pf_displayAt         126
#define PRIM_pf_displayRefresh    127
#define PRIM_pf_displayRefreshRow 128

/* tuvm#94 stage 1 (MagTag bell): `bellStrike: n` strikes n bells in the
   ship's-bell phrasing. ONE prim, not a family — the phrasing (pairs, lone final
   stroke) and the envelope are hardware-shaped decisions that belong beside the
   cosine generator, and exposing them separately would let nsl compose a
   striking the hardware cannot actually voice.
   IT BLOCKS, and for a long time: ~500 ms a strike plus the gaps, so eight bells
   is ~4 s. That is the same bargain the display's refresh prims took (tuvm#88) —
   safe because the pulse mailbox sheds and coalesces rather than aborting
   (src/backend/zephyr/pulse_zephyr.c, tuvm#34), so a button pressed mid-strike is
   delivered once afterwards instead of being lost.
   Appended after displayRefreshRow; no pre-existing id shifts. 129 was arrived at
   by COUNTING PRIM_NAMES (128 entries before this append), not by 128+1
   arithmetic, per the standing rule above.
   ANSWER CONVENTION, the display seam's: nil means "this board has no bell"
   (structural — no DAC, no speaker-enable pin, or the bring-up failed), and an
   operational outcome answers an SMI — 0 ok, 2 driver fault. There is no
   bounds code because a count outside 1-8 is checked in nsl, where the raise
   lives (Bell in kernel.nsl). */
#define PRIM_pf_bellStrike        129

/* tuvm#94 stage 4 (NTP catchup): `ntpSyncTz: offsetSeconds` runs sntp_simple,
   applies the offset, and answers SECONDS SINCE LOCAL MIDNIGHT (0..86399, SMI-safe
   — Unix epoch would overflow a 2^30 SMI). offsetSeconds is passed from nsl because
   the timezone is a product fact, not a hardware one. BLOCKS up to the SNTP timeout,
   so it is only ever called between strikes.
   ANSWER CONVENTION: nil = couldn't sync (structural — no WiFi, no route, timeout);
   an SMI = seconds-since-midnight. There is NO fault code, because there is nothing
   a node would do differently: a miss is a nil and the clock free-runs on its own
   crystal, which is the whole point of the rung (the network improves the time, it
   does not gate the bell).
   Counted (128 display + bellStrike = 129 entries before this), so this is the
   130th PRIM_NAMES entry, per the standing rule above. */
#define PRIM_pf_ntpSync           130

/* tuvm#94 stage 5 (deep-sleep clock): the RTC-retention seam. State that must
   outlive the dead VM across a deep sleep lives in a fixed 10-field SMI struct
   (.rtc.data on device, a file on host). rtcValid guards the magic word; rtcGet:/
   rtcPut:value: are indexed SMI accessors (design §3 table). Appended after
   ntpSync; no pre-existing id shifts. Counted (not computed): these are the
   131st/132nd/133rd PRIM_NAMES entries. */
#define PRIM_pf_rtcValid          131
#define PRIM_pf_rtcGet            132
#define PRIM_pf_rtcPut            133

/* tuvm#94 stage 5: deepSleep: seconds. On device it arms an esp_sleep timer wakeup
   and starts deep sleep (never returns — the VM ceases to exist until the next boot).
   On host it logs and exit(0)s so a run terminates. Counted (not computed) to be the
   134th PRIM_NAMES entry. */
#define PRIM_pf_deepSleep         134

/* tuvm#100: the WALL-CLOCK seam — an external RTC (DS3231 over I2C) as the
   timebase. `wallClock` answers SECONDS SINCE LOCAL MIDNIGHT (0..86399).
   NOT to be confused with PRIM_pf_rtcValid/rtcGet/rtcPut above: those are the
   deep-sleep RETENTION store (RAM that survives a sleep), which is a different
   device and a different question. The name collision is why this seam is
   `wallClock` and not `rtc*`.
   ANSWER CONVENTION, ntpSync's exactly: nil = no RTC on this board, or it has
   never been set (structural, and a node treats both the same way — free-run);
   an SMI = seconds since local midnight. Answering in the same shape as
   ntpSyncTz: is deliberate: schedule arithmetic must not be able to tell which
   timebase produced the number, so the two are interchangeable.
   Epoch seconds are NOT an option — 2^30 SMI, same reason ntpSync gives.
   There is no tz argument: NTP hands out UTC and the product applies its own
   offset, whereas an RTC holds the local time that was written to it, so a
   second offset here would double-correct.
   Counted (not computed): 134 PRIM_NAMES entries precede this, so this is the
   135th, per the standing rule above. */
#define PRIM_pf_wallClock         135

/* tuvm#100: `wallClockSetDate: yyyymmdd time: secondsOfDay` sets the RTC.
   The date is packed DECIMAL (20260731), not three arguments: Zephyr's
   rtc_set_time takes a whole struct rtc_time so the seam cannot be time-of-day
   only, but a six-keyword prim would exceed anything else in this VM. 20260731
   is SMI-safe with room to spare (2.0e7 against 2^30). The time half stays in
   `wallClock`'s own unit so a round trip is an identity, not a conversion.
   ONE call rather than a date write then a time write: two steps leave a window
   where the part holds a date from one instant and a time from another, and the
   second write can land after midnight has moved the day on.
   ANSWER CONVENTION: nil = no RTC on this board, or the write failed; SMI 0 = set.
   Counted (not computed): the 136th PRIM_NAMES entry. */
#define PRIM_pf_wallClockSet      136

/* tuvm#100 (2026-08-01): `ntpDate` answers the packed DECIMAL date (20260801) of
   the last successful ntpSyncTz:, in that call's timezone. It exists because
   ntpSyncTz: answers seconds-of-day while wallClockSetDate:time: requires a date,
   so without this the correction loop can measure the RTC's error and not fix it.
   IT DOES NOT PERFORM A REQUEST — it reads the response ntpSyncTz: already
   received. A second request would mean another 4-second blocking call and more
   radio traffic for a value already in hand.
   ORDERING CONTRACT: call ntpSyncTz: first and only read this if it answered
   non-nil. A node that ignores that gets the previous sync's date, or nil.
   ANSWER CONVENTION: nil = no sync has ever succeeded in this boot (structural,
   same as ntpSyncTz:'s own nil); otherwise the SMI yyyymmdd.
   Counted (not computed): the 137th PRIM_NAMES entry. */
#define PRIM_pf_ntpDate           137

/* porta space-sync, plan 2 (design D5): the space-sync seam — the piece the
   node's PortaBridge job replicates its space with.
     spaceSeq   -> SMI, the space's ONE monotonic counter (0 before any write).
     spaceStamp -> SMI, that counter BUMPED. The bridge stamps a CAPTURED stream
                   entry with it. A stream's value is killed on delivery, so it
                   is captured in a `whenever:` reaction — and reading the
                   counter instead of bumping it would let two captures with no
                   intervening write share one seq, which the gateway drops as a
                   duplicate. That is silent LOSS, not a repeated delivery.
     spaceChangedSince: cursor under: prefix max: n
                -> a raw indexable of up to n 3-slot indexables
                   {pathString, seqSMI, value}, ASCENDING by seq; an empty
                   indexable (not nil) when nothing changed, so a caller loops
                   over basicSize without a special case.
                   CELLS ONLY: a stream's last_seq advances on every post but its
                   value is not retained, so a stream row would carry a
                   stale-or-nil value under a fresh seq.
                   The prefix is SEGMENT-ALIGNED ('obs' never matches 'obscure')
                   and '' is the root; confinement is resolved in nsl before it
                   arrives here, exactly as checkPath: confines the write verbs.
                   Does NOT intern (space_take's rule).
   Appended after ntpDate; no pre-existing id shifts. 138-140 were arrived at by
   COUNTING PRIM_NAMES (137 entries before this append), not by 137+1 arithmetic,
   per the standing lesson above. */
#define PRIM_pf_spaceSeq          138
#define PRIM_pf_spaceStamp        139
#define PRIM_pf_spaceChangedSince 140

/* shipsBellsV3 (#102): `rtcAlarmAt: secondsOfDay post: msg on: 'path'` — the RTC
   says WHEN. Arms the DS3231's Alarm1 (the one with a seconds field) for a
   time-of-day; when it fires, the driver's workqueue callback posts `msg` on the
   interned channel through the ordinary pulse mailbox. Mirrors
   sensorArmEventPost:on:threshold: exactly: the path is interned and the message
   oop pinned via an M6 handle HERE, in thread context, because the delivery side
   must never touch the VM.
   FULL match mask (hour+minute+second), i.e. once per day, and the node re-arms
   after every strike — a repeating mask would have to encode "every half hour",
   which the part cannot express, and re-arming is needed regardless.
   Answers nil when the alarm cannot be armed: no RTC, or an image built without
   CONFIG_RTC_ALARM. A node that reads nil degrades to its own watchdog rather
   than falling silent, which is the whole point of having one.
   Appended after spaceChangedSince:under:max:; no pre-existing id shifts.
   Counted (not computed): the 141st PRIM_NAMES entry. */
#define PRIM_pf_rtcAlarmAt        141

/* #165 Task 4: per-method profile window control (`profileStart`/`profileStop`).
   heapStats (104) is NOT the last entry — rtcAlarmAt:post:on: (141, above) is;
   these two are appended after it, no pre-existing id shifts. The two ids were
   arrived at by COUNTING PRIM_NAMES (141 entries before this append), not by
   141+1 arithmetic, per the standing lesson above. Platform-dispatched. Both
   are no-ops in a build without OEVM_METHOD_PROFILE so an app runs unchanged
   on a production image. */
#define PRIM_pf_profileStart      142
#define PRIM_pf_profileStop       143

/* #74/#165 D3: `buildId`, the string baked into the image at build time (git
   describe + timestamp) — so a profile reader can pick the .oimg.methods.json
   sidecar that actually matches what is running, rather than choosing by hand
   and getting a mismatch that resolves nothing and looks identical to a quiet
   program. Appended after profileStop; no pre-existing id shifts. 144 was
   arrived at by COUNTING PRIM_NAMES (143 entries before this append), not by
   143+1 arithmetic, per the standing lesson above. Platform-dispatched.
   Unconditional: unlike profileStart/profileStop, this is NOT gated on
   OEVM_METHOD_PROFILE — it is not part of the profiler, and a build without
   the profiler still needs to name itself for the sidecar to be pickable. */
#define PRIM_pf_buildId           144

/* rete#25: the image-push seam reaches nsl. Six prims, one per ota.h function,
   each case in platform.c a thin translation and nothing more — the policy
   (which hash is wanted, when to confirm) lives in nsl, and the bytes live in C
   because a staged image must never enter the VM heap (design G3).

   Appended after buildId; no pre-existing id shifts. 145-150 were arrived at by
   COUNTING PRIM_NAMES (144 entries before this append), not by 144+1
   arithmetic, per the standing lesson above. Platform-dispatched.

   UNCONDITIONAL, like buildId and unlike profileStart/profileStop: there is no
   build flag to gate them on, because the gate is the RUNNER'S ENVIRONMENT and
   it is read at run time. With OEVM_IMAGE_HASH unset, otaRunningHash answers
   nil and otaIsRunning:/otaTrial/otaStage:... answer false; otaConfirm and
   otaActivate:sig: degrade to a console line. A node launched bare — which is
   every host test in this tree — therefore behaves exactly as it did before
   these six existed.

   BYTES CROSS THE SEAM AS LEAVES, NOT AS nsl VALUES: 146/148/150 take the
   RawBytes leaf (the `struct String` layout: length + inline bytes) and 145
   answers one. kernel.nsl's Platform accessors do the wrap/unwrap
   (`Bytes overRaw:` / `rawLeaf`), the same division of labour Socket>>sendTo:
   already uses — C never learns what an nsl Bytes object is.

   146 compares with memcmp in ota.c. That is deliberate and load-bearing: which
   image is installed must not be decided by nsl's `Bytes =`, which routes
   through the value algebra and could be shadowed by an app. */
#define PRIM_pf_otaRunningHash    145
#define PRIM_pf_otaIsRunning      146
#define PRIM_pf_otaTrial          147
#define PRIM_pf_otaStage          148
#define PRIM_pf_otaConfirm        149
#define PRIM_pf_otaActivate       150
/* rete#162/#117: the node's per-boot identity. Appended after the entire OTA
   group; no pre-existing id shifts. 151 was arrived at by COUNTING PRIM_NAMES
   (150 entries before this append), not by 150+1 arithmetic, per the standing
   lesson above. Platform-dispatched.

   WHY THIS IS A PRIM AND NOT A SOURCE CONSTANT. `bootId` has ridden the pull and
   push envelopes since the space-sync rung and has always answered 1, which made
   RECORDS §3's `(bootId, monoMs)` session key degenerate and — the reason this
   landed — left porta unable to tell a rebooted node from a node whose ack it
   merely missed. That ambiguity cost c6-bell a night of telemetry (#162): both
   ends reported success for ten hours while nothing was delivered.

   PERSISTED, NOT RANDOM. The counter must be MONOTONIC, because RECORDS §3
   claims `(bootId, monoMs)` totally ORDERS every observation a node ever makes —
   a random per-boot nonce would distinguish boots without ordering them, and
   would have forced that line to be weakened. Every Thread board already mounts
   NVS (`CONFIG_NVS=y` + `CONFIG_SETTINGS_NVS=y` in prj_*_thread.conf) because
   OpenThread persists its dataset there, so this costs no new subsystem — only a
   key. That is also why it is not gated on a build flag: the storage is present
   on every node that can reach porta at all.

   ANSWERS AN SMI. Kept below 2^30 so it stays a SmallInteger on a 32-bit target
   (oevm.h: SMI is intptr_t >> 1, i.e. 31-bit on the C6) — a LargeInteger here
   would allocate on the check-in path and cost 9 CBOR bytes instead of 5. */
#define PRIM_pf_bootId            151
/* rete#190 sub-project A: `platform cheapStats` — the C-allocation seam's
   counters (src/gc.c oevm_cheap_stats), the C-heap twin of heapStats (104),
   which reports the GC ARENA. Both exist because conflating them is what let
   a per-decoded-Symbol leak run for a week against a gauge that could not
   see it. Appended after bootId; no pre-existing id shifts. 152 was arrived
   at by COUNTING PRIM_NAMES (151 entries before this append), not by 151+1
   arithmetic, per the standing lesson above. Platform-dispatched (mirrored
   here as 152). Returns a 5-slot Array [outBytes, outHigh, allocs, frees,
   fails] snapshotting src/gc.c oevm_cheap_stats(); the nsl CHeapStats class
   names the fields. */
#define PRIM_pf_cheapStats        152
/* rete#215: the retained panic note. Appended after cheapStats — no pre-existing id
   shifts. 153/154 were arrived at by COUNTING PRIM_NAMES (152 string literals before this
   append, cross-checked against the compile-time parity guard), not by 152+1 arithmetic,
   per the standing lesson above.
     panicNote        -> 4-slot Array [reason, bootId, monoMs, detail] or nil. The
                         register triple is FORMATTED TEXT in slot 4, not numbers: mcause
                         does not fit an SMI on a 32-bit target (the same 2^30 limit
                         rtc_target_put's comment records), and RECORDS §2.4 already pins
                         Panic.detail as text and names it the one place truncation is
                         legal.
     panicNoteAck: id -> clears the report AND the loop-breaker count, and deposits this
                         boot's id for the next death to carry. One call because §5.1's
                         report-once clear and §5.2's "reset on a successful check-in" are
                         the same moment. */
#define PRIM_pf_panicNote         153
#define PRIM_pf_panicNoteAck      154
/* rete#186/#118: `nodeId` — WHICH BOARD THIS IS. Appended after panicNoteAck; no
   pre-existing id shifts. 155 was arrived at by COUNTING PRIM_NAMES (154 entries
   before this append, cross-checked against the parity guard), not by 154+1
   arithmetic, per the standing lesson above.
   Platform-dispatched. Sibling of buildId (144) and bootId (151): buildId names
   the IMAGE, bootId names the RUN, this names the BOARD.

   THE ID IS THE BOARD'S FACTORY LINK ADDRESS, read from silicon and formatted as
   lowercase hex by link_addr.h (design 2026-09-11, addendum 2026-10-02). It is not
   a build-time define and not a source constant: `PortaBridgeNodeC6.nsl` once
   answered `'c6-bench'`, so whatever devkit carried the bridge wrote into one
   `c6-bench` space (36,153 events, 19 distinct `obs/build` entries, at least two
   boards) and a space keyed on a slot cannot answer "what did THIS board do".
   The ROLE has its own home (porta's `goal/name`), so the id is not spent on it.

   A BOARD WITH NO FACTORY ADDRESS FAILS the primitive (the send ends in
   doesNotUnderstand:, preceded by one console line saying why): it does not boot
   with a made-up id. Likewise an id too long for PLATFORM_NODE_ID_MAX.

   The HOST has no silicon: platform_backend_node_id answers the OEVM_NODE_ID env
   value, and unset or empty fails like a board with no address. */
#define PRIM_pf_nodeId            155
/* rete#243: `manifestPin` — the truncated (8-byte) SHA-256 of the MANIFEST.cddl
   this image was compiled against, baked by zephyr/cmake/gen_manifest_pin.cmake
   beside the build id. Appended after nodeId; no pre-existing id shifts. 156 was
   arrived at by COUNTING PRIM_NAMES (155 entries before this append, and the
   dispatch.c parity guard was advanced in the same commit), not by 155+1
   arithmetic, per the standing lesson above.
   Platform-dispatched. Fourth sibling of the provenance triple: buildId names
   the IMAGE, bootId the RUN, nodeId the BOARD — this names the CONTRACT the
   image answers to. Answers a RawBytes leaf (the kernel accessor wraps it, on
   otaRunningHash's precedent) or nil on a build with no pin — a host build, or
   any pre-manifest image — which reads as "the node cannot say" at obs/build,
   exactly the absence rule that record already carries. Identifies a manifest
   against a known set; it does not authenticate one (obs/build.image's rule). */
#define PRIM_pf_manifestPin       156
/* rete#273 D1: `appLiveness:` — the app's DECLARED cadence for the observer's §3.2
   (feed age > appCad). Appended after manifestPin AND after dispatch.c's PRIM_intMod
   (157, not platform-dispatched, so not mirrored here); 158 was arrived at by
   COUNTING PRIM_NAMES (157 entries before this append), not by arithmetic.
   Platform-dispatched. Stores a uint32 the observer thread reads through
   platform_app_cad_ms(); 0 = undeclared = §3.2 not judged. Answers the stored
   value, so a caller can see what a clamp did. */
#define PRIM_pf_appLiveness       158
#endif /* OEVM_PRIM_IDS_H */
