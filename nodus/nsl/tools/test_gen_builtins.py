import subprocess, sys, importlib.util
from pathlib import Path

TOOLS = Path(__file__).resolve().parent


def load_registry():
    spec = importlib.util.spec_from_file_location("builtins_registry", TOOLS / "builtins_registry.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_generator_produces_stable_indices():
    subprocess.run([sys.executable, str(TOOLS / "gen_builtins.py")], check=True)
    reg = load_registry()
    assert reg.REGISTRY["IntegerVtable"] == 3
    assert reg.REGISTRY["FloatVtable"] == 4
    assert reg.REGISTRY["nil"] == 0
    # rete#228: this used to read `assert reg.VERSION == 2` and `len(...) == 23`.
    # Both were SNAPSHOTS, not invariants: VERSION is 5 and the registry holds 19, so
    # the test had been red since whichever legitimate change moved them — unnoticed,
    # because nothing ran this file. Re-pinning today's numbers would just re-arm the
    # same staleness on the next honest bump, so assert the PROPERTIES instead.
    #
    # The one that matters is the lockstep gen_builtins.py's own comment asks for and
    # nothing enforced: VERSION must equal BUILTIN_REGISTRY_VERSION in oimg_loader.c.
    # When they diverge, oimg_loader.c:131 rejects EVERY image at load with a version
    # mismatch — so a hand-maintained "keep in lockstep" comment was the only thing
    # standing between a one-line edit and a VM that loads nothing.
    import re
    c_src = (TOOLS.parent.parent / "oevm" / "src" / "oimg_loader.c").read_text()
    m = re.search(r"BUILTIN_REGISTRY_VERSION\s*=\s*(\d+)", c_src)
    assert m, "oimg_loader.c no longer declares BUILTIN_REGISTRY_VERSION"
    assert reg.VERSION == int(m.group(1)), (
        f"registry version drift: builtins_registry.py says {reg.VERSION}, "
        f"oimg_loader.c says {m.group(1)} — every image would be refused at load")

    # "Stable indices" means dense and unique, which is the property the name claims
    # and a bare length never checked.
    idx = sorted(reg.REGISTRY.values())
    assert idx == list(range(len(idx))), f"registry indices are not dense 0..n-1: {idx}"
