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
                                                    as gpioPin:edge:post:on:. */
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
#endif /* OEVM_PRIM_IDS_H */
