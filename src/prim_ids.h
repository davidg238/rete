#ifndef OEVM_PRIM_IDS_H
#define OEVM_PRIM_IDS_H
/* Platform-capability primitive IDs shared between dispatch.c and platform.c.
   Values mirror the anonymous enum in dispatch.c (std-lib step 3c-3b task 1).
   dispatch.c defines them authoritatively; these macros make them visible to
   platform.c (and future capability modules) without duplicating the enum.
   IMPORTANT: these values must stay in sync with the enum in dispatch.c.
   Updated for R7: PRIM_currentFiber inserted after PRIM_fiberSwitch (id 18),
   shifting all platform prim IDs by 1 (60->61 .. 70->71). */
#define PRIM_ix_array            61
#define PRIM_ix_basicAt          62
#define PRIM_ix_basicAt_put      63
#define PRIM_ix_basicSize        64
#define PRIM_pf_identicalTo      65
#define PRIM_pf_hashcode         66
#define PRIM_pf_print            67
#define PRIM_pf_printNewline     68
#define PRIM_pf_basicNewIndexable 69
#define PRIM_pf_string           70
#define PRIM_pf_readLine         71
#endif /* OEVM_PRIM_IDS_H */
