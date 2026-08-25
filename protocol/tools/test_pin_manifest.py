# Copyright (c) 2026 Ekorau LLC
"""pin_manifest / check_provision contract tests (#243 D5, §6)."""
import hashlib
import http.server
import threading

import pytest

from pathlib import Path

import check_provision
import pin_manifest

REPO_MANIFEST = Path(__file__).resolve().parents[1] / "MANIFEST.cddl"


class _ManifestHandler(http.server.BaseHTTPRequestHandler):
    body = b""
    lie = False

    def do_GET(self):
        digest = hashlib.sha256(self.body).hexdigest()
        if self.lie:
            digest = "0" * 64
        self.send_response(200)
        self.send_header("X-Manifest-SHA256", digest)
        self.end_headers()
        self.wfile.write(self.body)

    def log_message(self, *a):
        pass


@pytest.fixture
def served():
    _ManifestHandler.body = REPO_MANIFEST.read_bytes()
    _ManifestHandler.lie = False
    srv = http.server.HTTPServer(("127.0.0.1", 0), _ManifestHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{srv.server_port}/manifest"
    srv.shutdown()


def test_pin_from_porta(tmp_path, served):
    res = pin_manifest.pin(out_dir=tmp_path, url=served)
    assert res.source == "porta"
    assert (tmp_path / "MANIFEST.pinned.cddl").read_bytes() == REPO_MANIFEST.read_bytes()
    lines = (tmp_path / "oevm_manifest_pin.txt").read_text().splitlines()
    assert lines[0] == hashlib.sha256(REPO_MANIFEST.read_bytes()).hexdigest()
    assert lines[1] == "source=porta"


def test_lying_endpoint_refused(tmp_path, served):
    _ManifestHandler.lie = True
    with pytest.raises(pin_manifest.PinError):
        pin_manifest.pin(out_dir=tmp_path, url=served)


def test_unreachable_falls_back_to_repo(tmp_path, capsys):
    res = pin_manifest.pin(out_dir=tmp_path, url="http://127.0.0.1:1/manifest")
    assert res.source == "repo"
    assert "falling back" in capsys.readouterr().err
    lines = (tmp_path / "oevm_manifest_pin.txt").read_text().splitlines()
    assert lines[1] == "source=repo"


def test_no_url_is_repo_without_noise(tmp_path, capsys):
    res = pin_manifest.pin(out_dir=tmp_path, url=None)
    assert res.source == "repo"
    assert "falling back" not in capsys.readouterr().err


def test_emit_c_carries_first_eight_hash_bytes(tmp_path):
    res = pin_manifest.pin(out_dir=tmp_path, url=None, emit_c=tmp_path / "pin.c")
    want = bytes.fromhex(res.sha256)[:8]
    text = (tmp_path / "pin.c").read_text()
    for b in want:
        assert f"0x{b:02x}" in text
    assert "oevm_manifest_pin_len = 8" in text
    assert 'oevm_manifest_src[] = "repo"' in text


def test_dirty_manifest_refused(tmp_path):
    bad = tmp_path / "bad.cddl"
    bad.write_text('sym-orphan = #6.39("orphan")\n')
    with pytest.raises(pin_manifest.PinError):
        pin_manifest.pin(out_dir=tmp_path, url=None, repo_file=bad)


def test_check_provision(tmp_path, served, monkeypatch):
    digest = hashlib.sha256(REPO_MANIFEST.read_bytes()).hexdigest()
    arch = tmp_path / "elf" / "build-1"
    arch.mkdir(parents=True)
    (arch / "manifest.sha256").write_text(digest + "\n")
    monkeypatch.setenv("RETE_ELF_ROOT", str(tmp_path / "elf"))
    assert check_provision.check("build-1", served) == []
    (arch / "manifest.sha256").write_text("f" * 64 + "\n")
    assert any("pin" in f for f in check_provision.check("build-1", served))
    assert any("no archive entry" in f for f in check_provision.check("build-2", served))
