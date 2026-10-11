#!/usr/bin/env python3
"""patch_ocdo.py IN.oimg OUT.oimg [--check] — rewrite Spike296Node_BlockCost>>ocDoLowered: in an image with
`oc do: [:e | e]` lowered at the call site: the kernel's OrderedCollection>>do: loop inlined into the caller
and the block's body inlined into the loop, so an element costs bytecodes and no block activation. The
ceiling measurement for a guarded `do:` lowering (rete#37); see README.md beside this script.

What the lowering a compiler would emit looks like, per `oc do:` call:
  - a class guard, priced here by a stand-in of the same length (two loads and a never-taken branch):
    the ISA has no class-test opcode yet, and the guard runs once per call, not per element;
  - `count` and `contents` read once from the receiver's slots (the kernel's do: re-reads `contents`
    per element; the lowering hoists it, which is what inlining makes legal for the loop body here);
  - per element: LE (k <= count), JMP_IF_FALSE, AT (contents basicAt: k), the body, ADD, JMP.
LE and AT write their result over the window's first register, so the two copies before each are the
ISA's, not slack a peephole could take.

--check makes the body sum the elements and the method answer the sum instead of the time, for the host:
100000 iterations over 1..10 is 5,500,000 exactly when every element is visited once.

Reads the sibling .oimg.map.json for the layout and the method's offset. Jump offsets are relative to the
instruction after the jump (vm.c OP_JMP / OP_JMP_IF_FALSE)."""
import json, re, struct, sys
src, dst = sys.argv[1], sys.argv[2]
check = '--check' in sys.argv[3:]
NAME = 'Spike296Node_BlockCost>>ocDoLowered:'
blob = bytearray(open(src, 'rb').read())
m = json.load(open(src + '.map.json'))
lay = m['image']['layout']['structs']['CompiledMethod']['fields']
oopsz = m['image']['layout']['oop_size']
OP = {}
for l in open(__file__.rsplit('/docs/', 1)[0] + '/nodus/nsl/tools/ns_compile_oevm.py'):
    r = re.match(r'(OP_[A-Z_]+)\s*=\s*(0x[0-9a-fA-F]+|\d+)', l)
    if r: OP[r.group(1)] = int(r.group(2), 0)
fmt = '<Q' if oopsz == 8 else '<I'
off = next(int(o) for o, e in m['methods'].items() if e['name'] == NAME)
ins = struct.unpack_from(fmt, blob, off + lay['instructions']['offset'])[0] >> 2
cnt_off = off + lay['instruction_count']['offset']
reg_off = off + lay['num_registers']['offset']
old_n = struct.unpack_from('<I', blob, cnt_off)[0]

def w(i):
    return struct.unpack_from('<I', blob, ins + 4 * i)[0]

# The compiler's own prologue and epilogue are kept; check they are the ones this script was written
# against (consts 0 = 0, 1 = #millis, 2 = 1, 3 = 100000, 4 = #<=; r1 oc, r3 t0, r7 i, r8 the limit).
want = [('OP_LD_CONST', 6, 0, 0), ('OP_SEND', 0, 1, 6), ('OP_LD_REG', 3, 6, 0), ('OP_LD_CONST', 6, 2, 0),
        ('OP_LD_REG', 7, 6, 0), ('OP_LD_CONST', 8, 3, 0), ('OP_LD_REG', 9, 7, 0), ('OP_LD_REG', 10, 8, 0),
        ('OP_LE', 1, 4, 9)]
for i, (op, d, s1, s2) in enumerate(want):
    assert w(i) == OP[op] | d << 8 | s1 << 16 | s2 << 24, ('prologue moved', i, hex(w(i)))

ONE, CNT, CON, K, ACC = 16, 12, 15, 17, 22       # registers past the compiler's 17
LE_W, AT_W = 18, 20                              # windows: 18,19 and 20,21
new = [
 ('OP_LD_CONST', 6, 0, 0),         #  0  r6 = 0
 ('OP_SEND',     0, 1, 6),         #  1  0 millis
 ('OP_LD_CONST', ACC, 0, 0) if check else ('OP_LD_REG', 3, 6, 0),   # 2  t0 (check: acc = 0)
 ('OP_LD_CONST', 6, 2, 0),         #  3
 ('OP_LD_REG',   7, 6, 0),         #  4  i = 1
 ('OP_LD_CONST', 8, 3, 0),         #  5  limit 100000
 ('OP_LD_REG',   9, 7, 0),         #  6  outer: window
 ('OP_LD_REG',  10, 8, 0),         #  7
 ('OP_LE',       1, 4, 9),         #  8  i <= 100000
 ('JIF',         9, 'exit'),       #  9
 ('OP_LD_REG',  13, 1, 0),         # 10  guard stand-in: oc's class, the constant ...
 ('OP_LD_REG',  14, 1, 0),         # 11
 ('JIF',        13, 0),            # 12  ... and the branch to the send, never taken
 ('OP_LD_SLOT', CNT, 2, 1),        # 13  count    (OrderedCollection slot 2, as kernel do: reads it)
 ('OP_LD_SLOT', CON, 1, 1),        # 14  contents (slot 1)
 ('OP_LD_CONST', ONE, 2, 0),       # 15
 ('OP_LD_REG',   K, ONE, 0),       # 16  k = 1
 ('L', 'inner'),
 ('OP_LD_REG',  LE_W, K, 0),       # 17
 ('OP_LD_REG',  LE_W + 1, CNT, 0), # 18
 ('OP_LE',       1, 4, LE_W),      # 19  k <= count
 ('JIF',        LE_W, 'next'),     # 20
 ('OP_LD_REG',  AT_W, CON, 0),     # 21
 ('OP_LD_REG',  AT_W + 1, K, 0),   # 22
 ('OP_AT',       1, 4, AT_W),      # 23  e = contents basicAt: k (in bounds: the fallback's const is unused)
 ('OP_ADD', ACC, ACC, AT_W) if check else ('OP_LD_REG', 13, AT_W, 0),  # 24  the body: e
 ('OP_ADD',      K, K, ONE),       # 25
 ('JMP', 'inner'),                 # 26
 ('L', 'next'),
 ('OP_LD_CONST', 9, 2, 0),         # 27  outer i + 1
 ('OP_ADD',      7, 7, 9),         # 28
 ('JMP', 6),                       # 29
 ('L', 'exit'),
 ('OP_LD_CONST', 7, 0, 0),         # 30
 ('OP_SEND',     0, 1, 7),         # 31  0 millis
 ('OP_LD_REG', 7, 7, 0) if check else ('OP_SUB', 7, 7, 3),  # 32  t1 - t0 (check: no t0 to subtract)
 ('OP_RET', ACC if check else 7, 0, 0),  # 33
]
labels, code = {}, []
for t in new:
    if t[0] == 'L': labels[t[1]] = len(code)
    else: code.append(t)
assert len(code) <= old_n, (len(code), old_n)
for i, t in enumerate(code):
    if t[0] == 'JMP':
        word = OP['OP_JMP'] | (((labels[t[1]] if isinstance(t[1], str) else t[1]) - (i + 1)) & 0xFFFF) << 8
    elif t[0] == 'JIF':
        o = 0 if t[2] == 0 else labels[t[2]] - (i + 1)
        word = OP['OP_JMP_IF_FALSE'] | t[1] << 8 | (o & 0xFFFF) << 16
    else:
        op, d, s1, s2 = t
        word = OP[op] | d << 8 | s1 << 16 | s2 << 24
    struct.pack_into('<I', blob, ins + 4 * i, word)
struct.pack_into('<I', blob, cnt_off, len(code))
old_r = struct.unpack_from('<I', blob, reg_off)[0]
struct.pack_into('<I', blob, reg_off, max(old_r, ACC + 1))
open(dst, 'wb').write(blob)
print("patched %s%s: %d -> %d instructions, %d -> %d registers, at blob offset %d"
      % (src.rsplit('/', 1)[-1], ' (check)' if check else '', old_n, len(code), old_r, ACC + 1, ins))
