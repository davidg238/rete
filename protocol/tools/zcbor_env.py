# Copyright (c) 2026 Ekorau LLC
"""Bootstrap a cached venv holding zcbor 0.9.1 and validate CBOR against
MANIFEST.cddl (#243).

Pins per the 2026-08-24 probe: cbor2<6 (cbor2 6.x removed
CBORDecodeValueError and breaks zcbor 0.9.1's import), and CBOR is piped
via stdin (0.9.1's CLI crashes opening a CBOR file: open(path, "rb",
encoding=...)). The verdict is always zcbor's own exit code.
"""
import subprocess
import sys
from pathlib import Path

VENV = Path.home() / ".cache" / "rete-manifest-venv"


def ensure():
    """Answer the venv's python, creating and provisioning the venv if
    absent. Raises on pip failure (typically: no network on first run)."""
    py = VENV / "bin" / "python"
    if py.exists():
        return py
    subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)
    subprocess.run([str(VENV / "bin" / "pip"), "-q", "install",
                    "zcbor==0.9.1", "cbor2<6", "regex"], check=True)
    return py


def validate(cddl_path, rule, cbor_bytes):
    """zcbor-validate one encoded value against one rule. Raises
    subprocess.CalledProcessError (with stderr attached) on invalid."""
    ensure()
    subprocess.run(
        [str(VENV / "bin" / "zcbor"), "validate", "-c", str(cddl_path),
         "-i", "-", "--input-as", "cbor", "-t", rule],
        input=cbor_bytes, check=True, capture_output=True)
