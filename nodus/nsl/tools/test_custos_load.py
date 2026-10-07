#!/usr/bin/env python3
"""custos's load page against a real VM, driven as an agent drives it (rete#367).

custos serves /load on --host boards, which run the host VM once on each uploaded
image and reads the two boot lines a board prints (peer design D14): the firmware's
build-id, then the program's sha256 once the loader has accepted it. POST /load
answers that proof; the gate asserts the sha256 is the file's own. An image whose
registry_version the VM refuses answers 502 with the loader's reason, and bytes that
are not an .oimg answer 400 before the VM runs.

It serves two boards (peer design D15): `dev`, with a map, and `spare`, without one.
GET /boards names both and what each serves; each load goes to the board its URL names;
and with two boards there is no board-less shorthand.

Needs Go, and exits 77 without it, which the suite reports as a named SKIP (as
test_custos_debug.py does). Exits non-zero on failure.
"""
import hashlib, json, os, shutil, struct, sys, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_custos_debug import OIMG, VM, build, go_live, serve  # noqa: E402


def load_args():
    return ["--host", f"dev={VM}", "--map", f"dev={OIMG}.map.json", "--host", f"spare={VM}"]


def post(base, body, name="app.oimg"):
    req = urllib.request.Request(f"{base}/load?name={name}", data=body, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def run_http():
    with serve(load_args(), ready="/boards") as root:
        with urllib.request.urlopen(f"{root}/boards", timeout=10) as r:
            boards = {b["Name"]: b for b in json.load(r)}
        assert boards["dev"]["Verbs"] == ["debug", "profile", "load"], boards
        assert boards["spare"]["Verbs"] == ["load"] and boards["spare"]["Target"] == "host", boards
        try:
            urllib.request.urlopen(f"{root}/load/last", timeout=10)
            raise AssertionError("two boards, yet the board-less /load answered")
        except urllib.error.HTTPError as e:
            assert e.code == 404, e.code
        line = _run_http(f"{root}/board/spare")
        # The other board saw none of it.
        try:
            urllib.request.urlopen(f"{root}/board/dev/load/last", timeout=10)
            raise AssertionError("a load on spare is dev's last load")
        except urllib.error.HTTPError as e:
            assert e.code == 404, e.code
        return "boards dev and spare, no shorthand; on spare: " + line


def _run_http(base):
    image = open(OIMG, "rb").read()
    sha = hashlib.sha256(image).hexdigest()
    code, l = post(base, image, os.path.basename(OIMG))
    assert code == 200, (code, l)
    r = l["Result"]
    assert r["SHA256"] == sha and r["BuildID"] == "host" and r["Offset"] == 0, r
    assert r["Image"] == os.path.basename(OIMG) and r["Size"] == len(image), r
    with urllib.request.urlopen(f"{base}/load/last", timeout=10) as resp:
        assert json.load(resp)["Result"]["SHA256"] == sha
    # The VM's own loader refuses a registry it was not built for: custos passes
    # it (the prefix is well formed), so the refusal is the VM's, in its words.
    bad = bytearray(image)
    reg = struct.unpack_from("<I", bad, 8)[0]
    struct.pack_into("<I", bad, 8, reg + 1)
    code, body = post(base, bytes(bad))
    assert code == 502 and f"registry_version {reg + 1}" in body, (code, body)
    code, body = post(base, b"this is not an image, but it is long enough")
    assert code == 400 and "not an .oimg" in body, (code, body)
    return (f"http: {os.path.basename(OIMG)} loaded, sha256 {sha[:12]} from the VM's boot; "
            f"registry_version {reg + 1} refused 502 by the VM; a non-image 400")


def run_tui():
    """The TUI's load tab, no terminal, against board dev of a live custos."""
    sha = hashlib.sha256(open(OIMG, "rb").read()).hexdigest()
    with serve(load_args(), ready="/boards") as root:
        line = go_live("TestLiveLoad", {"CUSTOS_TUI_LOAD_URL": f"{root}/board/dev", "CUSTOS_TUI_LOAD_IMAGE": OIMG,
                                        "CUSTOS_TUI_LOAD_SHA": sha})
    return "tui: " + " ".join(line)


def main():
    if shutil.which("go") is None:
        print("custos-load: SKIP (go is not on PATH)")
        sys.exit(77)
    build()
    http = run_http()
    tui = run_tui()
    print("test_custos_load: PASSED (Path B, /load on two --host boards)")
    print(http)
    print(tui)


if __name__ == "__main__":
    main()
