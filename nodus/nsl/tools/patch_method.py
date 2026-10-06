#!/usr/bin/env python3
"""patch_method.py — one method of a running image, recompiled from edited source
(rete#313).

    python3 tools/patch_method.py <running.oimg> <old.nsl> <new.nsl> <Vtable>>selector> <out.patch> --layout=<layout.json>

A method in an .oimg names everything it uses by the image's own offsets, and the
VM matches a selector by identity, so a method from a second image is useless to
the first. This compiles both sources: the old one must reproduce the running
image byte for byte, which is what makes the old compile's offsets the running
image's. The changed method is taken from the new compile and each constant is
re-encoded through the old offsets. A constant the running image does not hold (a
new symbol, string, class or block) refuses the patch by name: interning one into
a running image is not this spike.

The output is the VM's patch file (oevm_image_main's `dbg:patch <file>`; vm.c
primitive_patch_from). Exits 1 with the reason on a refusal.
"""
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from ns_compile_oevm import compile_source, serialize_oimg  # noqa: E402


class Refused(Exception):
    pass


def method_of(graph, vt_name, selector):
    vt = graph.vtables.get(vt_name)
    if vt is None:
        raise Refused(f"no class {vt_name}")
    ids = [mid for sel, mid in vt["bindings"] if sel == selector]
    if not ids:
        raise Refused(f"{vt_name} has no method {selector}")
    return next(m for m in graph.methods if m["id"] == ids[0])


def encode(ser, c):
    """c through the running image's offsets, or Refused naming it."""
    if isinstance(c, str) and c.startswith("METHOD:"):
        raise Refused(f"the method makes a block ({c}): a block is a method of its own, not patched yet")
    try:
        return ser._const_ref(c)
    except KeyError as e:
        raise Refused(f"the running image holds no {c!r} ({e.args[0]})")


def build(running, old_src, new_src, qual, layout):
    vt_name, selector = qual.split(">>", 1)
    oop = layout["oop_size"]
    old_ser, old_graph, old_blob, _ = serialize_oimg(compile_source(old_src, oop), layout)
    have = hashlib.sha256(open(running, "rb").read()).hexdigest()
    got = hashlib.sha256(old_blob).hexdigest()
    if got != have:
        raise Refused(f"{old_src} compiles to sha256 {got[:12]}, not the running image's {have[:12]}")
    old_m = method_of(old_graph, vt_name, selector)
    new_m = method_of(compile_source(new_src, oop).solidify, vt_name, selector)
    consts = [encode(old_ser, c) for c in new_m["constants"]]
    if new_m["num_arguments"] != old_m["num_arguments"]:
        raise Refused("the arguments changed")
    lines = [
        "oevm-patch 1",
        f"vtable {old_ser._vtable_ref(vt_name):x}",
        f"selector {old_ser._symbol_ref(selector):x}",
        f"registers {new_m['num_registers']}",
        f"arguments {new_m['num_arguments']}",
        f"access {new_m.get('access_modifier', 0)}",
        f"instructions {len(new_m['instructions'])}",
        " ".join(f"{w & 0xFFFFFFFF:x}" for w in new_m["instructions"]),
        f"constants {len(consts)}",
        " ".join(f"{w:x}" for w in consts),
    ]
    return "\n".join(lines) + "\n", old_m, new_m


def main(argv):
    layout_path = None
    pos = []
    for a in argv:
        if a.startswith("--layout="):
            layout_path = a.split("=", 1)[1]
        else:
            pos.append(a)
    if len(pos) != 5 or not layout_path:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    running, old_src, new_src, qual, out = pos
    try:
        text, old_m, new_m = build(running, old_src, new_src, qual, json.load(open(layout_path)))
    except Refused as e:
        print(f"patch_method: refused: {e}")
        return 1
    with open(out, "w") as f:
        f.write(text)
    print(f"patch written to {out}: {qual}, {len(new_m['instructions'])} instructions "
          f"(was {len(old_m['instructions'])}), {new_m['num_registers']} registers (was {old_m['num_registers']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
