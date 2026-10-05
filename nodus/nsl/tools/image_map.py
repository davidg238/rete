"""The image map (`<name>.oimg.map.json`): every method in a .oimg, by blob offset.

The contract is protocol/IMAGE-ARTIFACTS.md §3 (rete#337); this module is its writer and
the fixture in protocol/fixtures/image/ is what both sides check against. It replaces
the two sidecars that came before it (`.stmap`, `.oimg.methods.json`), which described
the same methods under two names that did not agree.
"""
import hashlib
import json
import os


def build_image_map(blob, oop_size, entry, files, quals, source_maps, vtable_files,
                    block_vtables):
    """The map for one image, as a JSON string.

    blob          the .oimg bytes (the map carries their hash and size)
    files         source paths, the app's first; the map keeps base names
    quals         {offset: "<Vtable>>selector"} from the serializer
    source_maps   {"<name>": [(bytecode_offset, line), ...]} from the compiler
    vtable_files  {vtable: source path} for every vtable the compiler declared
    block_vtables {"Block_<home>_blockN": "<home>_blockN"}: a block is named without
                  the vtable's `Block_` prefix (§3)
    """
    index = {os.path.normpath(f): i for i, f in enumerate(files)}
    methods = {}
    for off in sorted(quals):
        vt, sel = quals[off].split(">>", 1)
        name = f"{block_vtables.get(vt, vt)}>>{sel}"
        methods[str(off)] = {
            "name": name,
            "file": index[os.path.normpath(vtable_files[vt])],
            "lines": [[o, l] for (o, l) in source_maps.get(name, [])],
        }
    # One method per line: the fixture's diff then reads method by method.
    head = {"image": {"sha256": hashlib.sha256(blob).hexdigest(), "size": len(blob),
                      "oop_size": oop_size},
            "entry": entry,
            "files": [os.path.basename(f) for f in files]}
    out = ["{"] + [f" {json.dumps(k)}: {json.dumps(v)}," for k, v in head.items()]
    out.append(' "methods": {')
    rows = [f"  {json.dumps(k)}: {json.dumps(v)}" for k, v in methods.items()]
    out.append(",\n".join(rows))
    out.append(" }\n}\n")
    return "\n".join(out)
