"""Source-map (.stmap) serializer for ns-oevm.

Emits the same JSON contract st-berry produces (st_serializer.serialize_source_map),
so the jast-gw debug viewer consumes ns-oevm and st-berry maps identically:

    { "source": "<basename.ns>",
      "functions": { "<Vtable>>selector>": [[offset, line], ...] } }
"""
import json


def build_stmap(source_filename, source_maps):
    """Serialize collected per-method line maps to a .stmap JSON string.

    source_maps: dict {qualified_name: [(bytecode_offset, line), ...]}.
    Returns a JSON string; tuples become two-element [offset, line] arrays.
    """
    functions = {
        name: [[offset, line] for (offset, line) in pairs]
        for name, pairs in source_maps.items()
    }
    return json.dumps(
        {"source": source_filename, "functions": functions},
        indent=2,
    )
