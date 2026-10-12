#!/usr/bin/env python3
"""patch_method.py — one method of a running image, recompiled from edited source
(rete#313).

    python3 tools/patch_method.py <running.oimg.map.json> <old.nsl> <new.nsl> <Vtable>>selector>

A method in an .oimg names everything it uses by the image's own offsets, and the
VM matches a selector by identity, so a method from a second image is useless to
the first. This compiles both sources with the layout the running image's map
carries (protocol/IMAGE-ARTIFACTS.md §3 `image.layout`): the old one must reproduce
the running image byte for byte (the map's sha256), which is what makes the old
compile's offsets the running image's. The changed method is taken from the new compile and each constant is
re-encoded through the old offsets. A constant the running image does not hold (a
new symbol, string, class or block) refuses the patch by name: interning one into
a running image is not this spike.

The output, on stdout, is the VM's patch text, one line: the debugger sends it as
`dbg:patch <text>` (supervisor.nsl doPatch:; vm.c primitive_patch), on the desktop
and over a board's cable alike. A debug line is capped by the VM's read buffer
(platform.c PRIM_pf_readLine), so a patch that would not fit is refused here rather
than arriving cut. Exits 1 with the reason on a refusal.
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


# The VM reads a debug line into a 512-byte buffer (platform.c PRIM_pf_readLine,
# `char buf[512]`), newline stripped; `dbg:patch ` and the text must fit in it.
LINE_BYTES = 511


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


def build(image_map, old_src, new_src, qual):
    if ">>" not in qual:
        raise Refused(f"name the method as <Vtable>>selector, not {qual!r}")
    vt_name, selector = qual.split(">>", 1)
    layout = image_map["image"]["layout"]
    oop = layout["oop_size"]
    old_ser, old_graph, old_blob, _ = serialize_oimg(compile_source(old_src, oop), layout)
    have = image_map["image"]["sha256"]
    got = hashlib.sha256(old_blob).hexdigest()
    if got != have:
        raise Refused(f"{old_src} compiles to sha256 {got[:12]}, not the running image's {have[:12]}")
    old_m = method_of(old_graph, vt_name, selector)
    new_m = method_of(compile_source(new_src, oop).solidify, vt_name, selector)
    consts = [encode(old_ser, c) for c in new_m["constants"]]
    if new_m["num_arguments"] != old_m["num_arguments"]:
        raise Refused("the arguments changed")
    words = [
        "oevm-patch 1",
        f"vtable {old_ser._vtable_ref(vt_name):x}",
        f"selector {old_ser._symbol_ref(selector):x}",
        f"registers {new_m['num_registers']}",
        f"arguments {new_m['num_arguments']}",
        f"instructions {len(new_m['instructions'])}",
        " ".join(f"{w & 0xFFFFFFFF:x}" for w in new_m["instructions"]),
        f"constants {len(consts)}",
        " ".join(f"{w:x}" for w in consts),
    ]
    text = " ".join(words)
    if len("dbg:patch " + text) > LINE_BYTES:
        raise Refused(f"the patch is {len(text)} B; a debug line holds {LINE_BYTES - len('dbg:patch ')}")
    return text + "\n", old_m, new_m


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    map_path, old_src, new_src, qual = argv
    try:
        text, old_m, new_m = build(json.load(open(map_path)), old_src, new_src, qual)
    except Refused as e:
        print(f"patch_method: refused: {e}")
        return 1
    sys.stdout.write(text)
    print(f"patch_method: {qual}, {len(new_m['instructions'])} instructions "
          f"(was {len(old_m['instructions'])}), {new_m['num_registers']} registers "
          f"(was {old_m['num_registers']})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
