/* rete's declaration of the vendored ed25519's one entry point (rete#460).
   curve25519.c defines ED25519_verify, but the vendored curve25519.h declares no
   prototype for it (MCUboot declares it in its own bootutil sources, which are not
   vendored here). This header is rete's, not vendored, so the vendored files stay as
   copied (README.md). Answers 1 when the signature verifies. */
#ifndef RETE_ED25519_H
#define RETE_ED25519_H
#include <stddef.h>
#include <stdint.h>
int ED25519_verify(const uint8_t *message, size_t message_len,
                   const uint8_t signature[64], const uint8_t public_key[32]);
#endif
