#ifndef OEVM_PRIM_IDS_H
#define OEVM_PRIM_IDS_H
/* Platform-capability primitive IDs shared between dispatch.c and platform.c.
   Values mirror the anonymous enum in dispatch.c (std-lib step 3c-3b task 1).
   dispatch.c defines them authoritatively; these macros make them visible to
   platform.c (and future capability modules) without duplicating the enum.
   IMPORTANT: these values must stay in sync with the enum in dispatch.c.
   Task 9a: the 5 retired NLR/fiber prim enum slots (executeCleanup,
   resumeWith:, fiberCreate:, fiberSwitch:value:, currentFiber) were removed,
   shifting the platform prim IDs down by 5 (61->56 .. 71->66). */
#define PRIM_ix_array            56
#define PRIM_ix_basicAt          57
#define PRIM_ix_basicAt_put      58
#define PRIM_ix_basicSize        59
#define PRIM_pf_identicalTo      60
#define PRIM_pf_hashcode         61
#define PRIM_pf_print            62
#define PRIM_pf_printNewline     63
#define PRIM_pf_basicNewIndexable 64
#define PRIM_pf_string           65
#define PRIM_pf_readLine         66
#endif /* OEVM_PRIM_IDS_H */
