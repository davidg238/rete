/* zephyr/blob.S — the Model B program, copied verbatim into flash .rodata.
   Deliberately NOT `xxd -i` into a C array: a byte array re-enters the
   assembler through the front door, which is the exact cost Model B exists to
   escape (946 KB of C -> 941.7 MB / 71M lines of Xtensa asm -> ~46 min in `as`).
   .incbin assembles the same bytes in 0.00 s. OEVM_BLOB_FILE is -D'd by CMake.

   .balign 4 is load-bearing, not cosmetic: oimg_load_blob() computes
   ALIGN_UP(OIMG_PREFIX_FIXED, sizeof(oop)) and read_word()s the header refs off
   `base`, so the blob's first byte must be oop-aligned (sizeof(oop) == 4 here,
   per the banked layout's "oop_size": 4). */
    .section .rodata
    .balign 4
    .global oevm_blob_start
oevm_blob_start:
    .incbin OEVM_BLOB_FILE
    .global oevm_blob_end
oevm_blob_end:
