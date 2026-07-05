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
#endif /* OEVM_PRIM_IDS_H */
