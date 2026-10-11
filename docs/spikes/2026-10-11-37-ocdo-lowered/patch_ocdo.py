#!/usr/bin/env python3
"""patch_ocdo.py IN.oimg OUT.oimg [--check] — rewrite two methods of Spike296Node_BlockCost in an image with
`oc do: [:e | e]` lowered at the call site: the kernel's OrderedCollection>>do: loop inlined into the caller
and the block's body inlined into the loop, so an element costs bytecodes and no block activation. The
ceiling measurement for a guarded `do:` lowering (rete#37); see README.md beside this script.

  ocDoLowered:  today's ISA — per element LE (k <= count), JMP_IF_FALSE, AT (contents basicAt: k), the body,
                ADD, JMP: 10 bytecodes, four of them window copies (LE and AT write over their window's
                first register).
  ocDoFused:    the spike's OP_ITER_AT (vm.c, oevm.h) — per element ITER_AT, the body, JMP: 3 bytecodes.

Both, per `oc do:` call:
  - a class guard, priced by a stand-in of the same length (two loads and a never-taken branch): the ISA has
    no class-test opcode yet, and the guard runs once per call, not per element;
  - `count` and `contents` read once from the receiver's slots. The kernel's do: reads `count` once too but
    re-reads `contents` per element, so a body that grows the collection would see the new array there and
    the old one here: a real lowering must keep the re-read or prove the body cannot grow the receiver.

--check makes the body sum the elements and the method answer the sum instead of the time, for the host:
100000 iterations over 1..10 is 5,500,000 exactly when every element is visited once.

Reads the sibling .oimg.map.json for the layout and the methods' offsets. Jump offsets are relative to the
instruction after the jump (vm.c OP_JMP / OP_JMP_IF_FALSE / OP_ITER_AT)."""
import json, re, struct, sys
src, dst = sys.argv[1], sys.argv[2]
check = '--check' in sys.argv[3:]
blob = bytearray(open(src, 'rb').read())
m = json.load(open(src + '.map.json'))
lay = m['image']['layout']['structs']['CompiledMethod']['fields']
oopsz = m['image']['layout']['oop_size']
OP = {}
for l in open(__file__.rsplit('/docs/', 1)[0] + '/nodus/nsl/tools/ns_compile_oevm.py'):
    r = re.match(r'(OP_[A-Z_]+)\s*=\s*(0x[0-9a-fA-F]+|\d+)', l)
    if r: OP[r.group(1)] = int(r.group(2), 0)
fmt = '<Q' if oopsz == 8 else '<I'

ACC = 22                                         # registers past the compiler's 17; the frame grows to 23

# The compiler's own prologue and epilogue are kept; check they are the ones this script was written
# against (consts 0 = 0, 1 = #millis, 2 = 1, 3 = 100000, 4 = #<=; r1 oc, r3 t0, r7 i, r8 the limit).
PROLOGUE = [('OP_LD_CONST', 6, 0, 0), ('OP_SEND', 0, 1, 6), ('OP_LD_REG', 3, 6, 0), ('OP_LD_CONST', 6, 2, 0),
            ('OP_LD_REG', 7, 6, 0), ('OP_LD_CONST', 8, 3, 0), ('OP_LD_REG', 9, 7, 0), ('OP_LD_REG', 10, 8, 0),
            ('OP_LE', 1, 4, 9)]

def head():
    return [
     ('OP_LD_CONST', 6, 0, 0),         # r6 = 0
     ('OP_SEND',     0, 1, 6),         # 0 millis
     ('OP_LD_CONST', ACC, 0, 0) if check else ('OP_LD_REG', 3, 6, 0),   # t0 (check: acc = 0)
     ('OP_LD_CONST', 6, 2, 0),
     ('OP_LD_REG',   7, 6, 0),         # i = 1
     ('OP_LD_CONST', 8, 3, 0),         # limit 100000
     ('L', 'outer'),
     ('OP_LD_REG',   9, 7, 0),         # outer: window
     ('OP_LD_REG',  10, 8, 0),
     ('OP_LE',       1, 4, 9),         # i <= 100000
     ('JIF',         9, 'exit'),
     ('OP_LD_REG',  13, 1, 0),         # guard stand-in: oc's class, the constant ...
     ('OP_LD_REG',  14, 1, 0),
     ('JIF',        13, None),         # ... and the branch to the send, never taken
    ]

def tail():
    return [
     ('L', 'next'),
     ('OP_LD_CONST', 9, 2, 0),         # outer i + 1
     ('OP_ADD',      7, 7, 9),
     ('JMP', 'outer'),
     ('L', 'exit'),
     ('OP_LD_CONST', 7, 0, 0),
     ('OP_SEND',     0, 1, 7),         # 0 millis
     ('OP_LD_REG', 7, 7, 0) if check else ('OP_SUB', 7, 7, 3),   # t1 - t0 (check: no t0 to subtract)
     ('OP_RET', ACC if check else 7, 0, 0),
    ]

def body(e):
    return ('OP_ADD', ACC, ACC, e) if check else ('OP_LD_REG', 13, e, 0)   # the body: e

def lowered():
    ONE, CNT, CON, K = 16, 12, 15, 17
    LE_W, AT_W = 18, 20                                  # windows: 18,19 and 20,21
    return head() + [
     ('OP_LD_SLOT', CNT, 2, 1),        # count    (OrderedCollection slot 2, as kernel do: reads it)
     ('OP_LD_SLOT', CON, 1, 1),        # contents (slot 1)
     ('OP_LD_CONST', ONE, 2, 0),
     ('OP_LD_REG',   K, ONE, 0),       # k = 1
     ('L', 'inner'),
     ('OP_LD_REG',  LE_W, K, 0),
     ('OP_LD_REG',  LE_W + 1, CNT, 0),
     ('OP_LE',       1, 4, LE_W),      # k <= count
     ('JIF',        LE_W, 'next'),
     ('OP_LD_REG',  AT_W, CON, 0),
     ('OP_LD_REG',  AT_W + 1, K, 0),
     ('OP_AT',       1, 4, AT_W),      # e = contents basicAt: k (in bounds: the fallback's const is unused)
     body(AT_W),
     ('OP_ADD',      K, K, ONE),
     ('JMP', 'inner'),
    ] + tail()

def fused():
    B = 17                                               # R(B) k, R(B+1) count, R(B+2) contents, R(B+3) e
    return head() + [
     ('OP_LD_SLOT', B + 1, 2, 1),      # count
     ('OP_LD_SLOT', B + 2, 1, 1),      # contents
     ('OP_LD_CONST', B, 2, 0),         # k = 1
     ('L', 'inner'),
     ('ITER', B, 'next'),              # k > count -> next; else e = contents[k], k = k + 1
     body(B + 3),
     ('JMP', 'inner'),
    ] + tail()

def patch(name, prog):
    off = next(int(o) for o, e in m['methods'].items() if e['name'] == name)
    ins = struct.unpack_from(fmt, blob, off + lay['instructions']['offset'])[0] >> 2
    cnt_off = off + lay['instruction_count']['offset']
    reg_off = off + lay['num_registers']['offset']
    old_n = struct.unpack_from('<I', blob, cnt_off)[0]
    for i, (op, d, s1, s2) in enumerate(PROLOGUE):
        word = struct.unpack_from('<I', blob, ins + 4 * i)[0]
        assert word == OP[op] | d << 8 | s1 << 16 | s2 << 24, ('prologue moved', name, i, hex(word))
    labels, code = {}, []
    for t in prog:
        if t[0] == 'L': labels[t[1]] = len(code)
        else: code.append(t)
    assert len(code) <= old_n, (name, len(code), old_n)
    rel = lambda lab, i: 0 if lab is None else labels[lab] - (i + 1)
    for i, t in enumerate(code):
        if t[0] == 'JMP':
            word = OP['OP_JMP'] | (rel(t[1], i) & 0xFFFF) << 8
        elif t[0] in ('JIF', 'ITER'):
            word = OP['OP_JMP_IF_FALSE' if t[0] == 'JIF' else 'OP_ITER_AT'] | t[1] << 8 | (rel(t[2], i) & 0xFFFF) << 16
        else:
            op, d, s1, s2 = t
            word = OP[op] | d << 8 | s1 << 16 | s2 << 24
        struct.pack_into('<I', blob, ins + 4 * i, word)
    struct.pack_into('<I', blob, cnt_off, len(code))
    old_r = struct.unpack_from('<I', blob, reg_off)[0]
    struct.pack_into('<I', blob, reg_off, max(old_r, ACC + 1))
    print("patched %s%s: %d -> %d instructions, %d -> %d registers, at blob offset %d"
          % (name, ' (check)' if check else '', old_n, len(code), old_r, ACC + 1, ins))

patch('Spike296Node_BlockCost>>ocDoLowered:', lowered())
patch('Spike296Node_BlockCost>>ocDoFused:', fused())
open(dst, 'wb').write(blob)
