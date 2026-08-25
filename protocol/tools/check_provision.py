# Copyright (c) 2026 Ekorau LLC
"""Provision-time pin re-validation (#243 design §6.3): a build's archived
manifest pin must equal the hash of the manifest porta currently serves.

  check_provision.py <build-id> --url http://<porta>:6970/manifest

Exit 0 = the pin matches. Exit 1 with named reasons otherwise — a MISSING
archive entry refuses the same way, because a build whose provenance the
archive cannot answer for must not be provisioned on faith.

Gatekeeper's own plan absorbs this check when that process exists; until then
this is the flash flow's pre-step.
"""
import argparse
import hashlib
import os
import sys
import urllib.request
from pathlib import Path


def check(build_id, url):
    findings = []
    root = Path(os.environ.get("RETE_ELF_ROOT",
                               Path.home() / ".local" / "state" / "rete" / "elf"))
    entry = root / build_id / "manifest.sha256"
    if not entry.is_file():
        return [f"no archive entry for {build_id} at {entry} — provenance unanswerable, refuse"]
    pinned = entry.read_text().strip()
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            served = hashlib.sha256(resp.read()).hexdigest()
    except OSError as e:
        return [f"cannot fetch served manifest from {url}: {e}"]
    if pinned != served:
        findings.append(
            f"pin mismatch: build {build_id} pinned {pinned[:12]}…, porta serves "
            f"{served[:12]}… — rebuild against the current manifest before provisioning")
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build_id")
    ap.add_argument("--url", required=True)
    args = ap.parse_args()
    findings = check(args.build_id, args.url)
    for f in findings:
        print(f"check_provision: {f}", file=sys.stderr)
    if not findings:
        print(f"check_provision: {args.build_id} pin matches the served manifest")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
