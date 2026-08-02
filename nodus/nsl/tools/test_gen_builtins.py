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
    assert reg.VERSION == 2
    assert len(reg.REGISTRY) == 23
