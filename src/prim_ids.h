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
#endif /* OEVM_PRIM_IDS_H */
