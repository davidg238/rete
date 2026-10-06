/* Copyright (c) 2026 Ekorau LLC */
/* rete#310 — the zephyr ota backend answers OEVM_IMAGE_HASH once main.c has
 * noted the hash of the program it loaded, and nothing before that.
 *
 * Link set (run_tests_oevm.py C_UNIT_TESTS): src/backend/zephyr/ota_zephyr.c
 * ONLY. That file includes no zephyr header on purpose, so it builds here. */
#include "../src/ota_backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t h[32];
    for (int i = 0; i < 32; i++) h[i] = (uint8_t)(i * 9 + 1);

    assert(ota_env("OEVM_IMAGE_HASH") == NULL);      /* nothing noted: nil, as before */
    ota_note_running_hash(h);
    const char *hex = ota_env("OEVM_IMAGE_HASH");
    assert(hex != NULL);
    assert(strcmp(hex, "010a131c252e374049525b646d767f88919aa3acb5bec7d0d9e2ebf4fd060f18") == 0);
    assert(ota_env("OEVM_OTA_STAGE") == NULL);       /* every other name still answers nil */
    printf("ota_zephyr: ok\n");
    return 0;
}
