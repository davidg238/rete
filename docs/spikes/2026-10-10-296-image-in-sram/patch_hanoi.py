#!/usr/bin/env python3
"""patch_hanoi.py IN.oimg OUT.oimg — rewrite Spike296Node>>hanoiMove:from:to:temp: in an image with the
bytecode a copy-propagating compiler would emit (17 instructions, 11 executed per activation on average,
against the compiler's 27 / 16). Same semantics: the two recursive sends, `^ n`. The ceiling measurement
for a peephole pass; see docs/results/2026-10-10-296-image-in-sram.md §9. Reads the sibling .oimg.map.json
for the layout and the method's offset. Jump offsets are relative to the instruction after the jump."""
import json, re, struct, sys
src, dst = sys.argv[1], sys.argv[2]
blob = bytearray(open(src, 'rb').read())
m = json.load(open(src + '.map.json'))
lay = m['image']['layout']['structs']['CompiledMethod']['fields']
oopsz = m['image']['layout']['oop_size']
names = {}
for l in open(__file__.rsplit('/docs/', 1)[0] + '/nodus/nsl/tools/ns_compile_oevm.py'):
    r = re.match(r'(OP_[A-Z_]+)\s*=\s*(0x[0-9a-fA-F]+|\d+)', l)
    if r: names[r.group(1)] = int(r.group(2), 0)
fmt = '<Q' if oopsz == 8 else '<I'
off = next(int(o) for o, e in m['methods'].items() if e['name'] == 'Spike296Node>>hanoiMove:from:to:temp:')
ins = struct.unpack_from(fmt, blob, off + lay['instructions']['offset'])[0] >> 2
cnt_off = off + lay['instruction_count']['offset']
old_n = struct.unpack_from('<I', blob, cnt_off)[0]
new = [
 ('OP_LD_CONST', 9, 0, 0),     # r9 = 1 (const 0), once
 ('OP_LD_REG',   8, 1, 0),     # r8 = n   (the compare's window: r8 recv, r9 arg)
 ('OP_GT',       1, 1, 8),     # r8 = n > 1
 ('OP_JMP_IF_FALSE', 8, 12, 0),# leaf -> 16
 ('OP_LD_REG',  10, 0, 0),     # window 1: r10 = self
 ('OP_SUB',     11, 1, 9),     #   r11 = n - 1   (operands direct, no copies)
 ('OP_LD_REG',  12, 2, 0),     #   src
 ('OP_LD_REG',  13, 4, 0),     #   tmp
 ('OP_LD_REG',  14, 3, 0),     #   dst
 ('OP_SEND',     4, 2, 10),    # hanoiMove: n-1 from: src to: tmp temp: dst
 ('OP_LD_REG',  10, 0, 0),     # window 2
 ('OP_SUB',     11, 1, 9),
 ('OP_LD_REG',  12, 4, 0),
 ('OP_LD_REG',  13, 3, 0),
 ('OP_LD_REG',  14, 2, 0),
 ('OP_SEND',     4, 2, 10),
 ('OP_RET',      1, 0, 0),     # ^ n, from its own register
]
assert len(new) <= old_n, (len(new), old_n)
for i, (op, d, s1, s2) in enumerate(new):
    struct.pack_into('<I', blob, ins + 4 * i, names[op] | (d << 8) | (s1 << 16) | (s2 << 24))
struct.pack_into('<I', blob, cnt_off, len(new))
open(dst, 'wb').write(blob)
print("patched %s: %d -> %d instructions at blob offset %d" % (src.rsplit('/', 1)[-1], old_n, len(new), ins))
