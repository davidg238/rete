# ed25519 verify, vendored (rete#233 S2, 2026-10-10)

The firmware verifies a pushed program's signature with **MCUboot's own ed25519**, copied
unchanged from the Zephyr workspace's `bootloader/mcuboot` (the `main` the workspace
pins, 2026-10) so that the verify seat on the board is the one MCUboot would use for the
firmware itself (code-push design D2, D3, §10 T4):

| File | From | Licence |
|---|---|---|
| `curve25519.c`, `curve25519.h`, `curve25519_tables.h` | `ext/fiat/src/` (fiat-crypto / BoringSSL) | MIT (header in each file) |
| `sha512.c`, `tinycrypt/sha512.h` | `ext/tinycrypt-sha512/lib/` | BSD-3 (header in each file) |
| `utils.c`, `tinycrypt/constants.h`, `tinycrypt/utils.h` | `ext/tinycrypt/lib/` | BSD-3 |

One edit: `curve25519.c`'s `#include <bootutil/bootutil_public.h>` is removed (it pulled
MCUboot's config headers and nothing in the file used it); the `MCUBOOT_USE_MBED_TLS`
branch is simply never defined, so the tinycrypt SHA-512 path is compiled.

The one entry point the runner uses is `ED25519_verify(message, len, signature[64],
public_key[32])`, over the 32 raw hash bytes porta signs (`blob.SignHash`).
