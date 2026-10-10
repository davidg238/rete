/* Copyright (c) 2026 Ekorau LLC */
/* rete#233 T10 — ota.c under CONFIG_OEVM_OTA_INCHANNEL: the fetch runs on the caller's
 * thread and ota_stage answers 1 or -1 on its first call, never 0. Same scripted peer,
 * same resume arithmetic, same digests as ota_seam_test.c — this file IS that test,
 * compiled with the define (nodus/nsl/tools/run_tests_oevm.py, C_UNIT_TESTS), plus the
 * one assertion the define adds: stage() never loops. The stub ota_thread_start below
 * still links and is never reached. */
#ifndef CONFIG_OEVM_OTA_INCHANNEL
#error "ota_inchannel_test is the seam test compiled with -DCONFIG_OEVM_OTA_INCHANNEL=1"
#endif
#include "ota_seam_test.c"
