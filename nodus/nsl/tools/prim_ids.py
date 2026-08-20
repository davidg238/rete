"""rete#226 — the primitive selector ids, read from the ONE copy of the list.

`nodus/tuvm/src/prim_names.def` is the single source: dispatch.c builds PRIM_NAMES from it
(index i -> id i+1) and the image builders bake each Symbol's id from it. Before #226 the
list lived only in C, so baking ids at image build would have meant a second copy in
Python — and the C side already carries a compile-time parity guard against the enum
precisely because a list that drifts misroutes every primitive after the insertion point,
silently and only on the side that drifted.

Reading the .def instead of restating it means a prim appended in C is picked up by both
builders with no second edit, and a .def that cannot be found is an ERROR rather than an
empty map: baking zeros would leave every image with no primitives intercepted at all.
"""
import re
from pathlib import Path

_DEF = Path(__file__).resolve().parents[2] / "tuvm" / "src" / "prim_names.def"


def load_prim_ids(path=None):
    """Answer {selector_name: prim_id}, ids 1-based in file order (PRIM_NONE == 0)."""
    p = Path(path) if path else _DEF
    if not p.exists():
        raise FileNotFoundError(
            f"rete#226: {p} is missing. It is the single source for primitive selector "
            f"ids; without it an image would be baked with every selector marked 'not a "
            f"primitive' and would run with no primitives intercepted at all.")
    names = []
    for line in p.read_text().splitlines():
        m = re.match(r'\s*PRIM_NAME\("((?:[^"\\]|\\.)*)"\)\s*$', line)
        if m:
            names.append(m.group(1))
    if not names:
        raise ValueError(f"rete#226: {p} parsed to zero names — the format changed?")
    if len(names) != len(set(names)):
        dupes = sorted({n for n in names if names.count(n) > 1})
        raise ValueError(
            f"rete#226: duplicate selector(s) in {p}: {dupes}. The list is positional — a "
            f"duplicate means two ids for one name and a silently wrong dispatch.")
    return {name: i + 1 for i, name in enumerate(names)}


PRIM_IDS = load_prim_ids()
