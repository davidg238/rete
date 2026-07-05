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
   platform prim IDs up by 3 (56->59 .. 66->69). */
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
#endif /* OEVM_PRIM_IDS_H */
