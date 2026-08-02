#!/usr/bin/env python3
# Simulate loader.c map_rom_segments' segment walk over a real zephyr.bin,
# with the OLD IS_RTC (RTC_DRAM only) vs the FIXED IS_RTC (RTC_DRAM|RTC_IRAM|RTC_DATA).
# Proves on the host that the fix lets the walk reach the hidden IMAP/DMAP headers
# and produce the correct IROM/DROM flash offsets (tuvm#94 stage 5 POWEROFF crash).
import struct, sys

BIN = sys.argv[1]
FLASH_BASE = 0x1000            # boot_partition offset: zephyr.bin is flashed here
CHECKSUM_ALIGN = 16

R = dict(DRAM=(0x3FFB0000, 0x40000000), IRAM=(0x40020000, 0x40070000),
         IROM=(0x40080000, 0x40800000), DROM=(0x3F000000, 0x3FF80000),
         RTC_DRAM=(0x3FF9E000, 0x3FFA0000), RTC_IRAM=(0x40070000, 0x40072000),
         RTC_DATA=(0x50000000, 0x50002000))

def klass(addr, fixed):
    if addr == 0: return 'PADD'
    for name, (lo, hi) in R.items():
        if lo <= addr < hi:
            if name in ('RTC_IRAM', 'RTC_DATA') and not fixed: return 'LAST'
            return name
    return 'LAST'

data = open(BIN, 'rb').read()
magic, seg_count = data[0], data[1]
assert magic == 0xE9, hex(magic)
print(f"header: segment_count={seg_count}")

for fixed in (False, True):
    print(f"\n--- {'FIXED' if fixed else 'OLD'} walk:")
    off = 24  # sizeof(esp_image_header_t)
    ram_segments = 0; checksum = False; segments = 0
    irom_off = drom_off = None
    while segments < 16:
        segments += 1
        load_addr, dlen = struct.unpack_from('<II', data, off)
        k = klass(load_addr, fixed)
        if k == 'LAST':
            print(f"  seg{segments}: load 0x{load_addr:08x} len 0x{dlen:x} -> IS_LAST, break")
            break
        print(f"  seg{segments}: load 0x{load_addr:08x} len 0x{dlen:<6x} {k}"
              f"  (flash lma 0x{FLASH_BASE + off + 8:x})")
        if k == 'IROM': irom_off = FLASH_BASE + off + 8
        if k == 'DROM': drom_off = FLASH_BASE + off + 8
        if k in ('DRAM', 'IRAM', 'RTC_DRAM', 'RTC_IRAM', 'RTC_DATA'):
            ram_segments += 1
        off += 8 + dlen
        if ram_segments == seg_count and not checksum:
            off += (CHECKSUM_ALIGN - 1) - (off % CHECKSUM_ALIGN) + 1
            checksum = True
    print(f"  walk saw {segments - 1} segments; "
          f"IROM fix-up={'0x%x' % irom_off if irom_off else 'NEVER (stale static used)'} "
          f"DROM fix-up={'0x%x' % drom_off if drom_off else 'NEVER (stale static used)'}")
