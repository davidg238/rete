#!/usr/bin/env python3
"""custos's build and run verbs against a real VM, driven as an agent drives them
(rete#314, peer design D18).

One --host board. Over HTTP only:
  build  POST /build?for=dev&name=F.nsl with the source: the answer names the image and
         its sha256, and GET /build/last.oimg is that image (and its map, which names
         F.nsl); POSTed to the board's /load, the VM's own boot proves the same sha256.
  run    POST /run with a test's source: its output and exit status 0; a source that
         does not compile answers 422 with the compiler's words; a program that never
         ends is stopped at custos's RunTimeout and says so.

Needs Go, and exits 77 without it, which the suite reports as a named SKIP (as
test_custos_debug.py does). Exits non-zero on failure.
"""
import hashlib, json, os, shutil, sys, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_custos_debug import NSROOT, VM, build, go_live, serve  # noqa: E402

TESTS = os.path.join(NSROOT, "..", "tests")
ADDITION = os.path.join(TESTS, "01_expressions", "01_addition.nsl")
FOREVER = """class Forever usingPlatform: platform = () (
  public main: platform2 args: args = (
    'forever' println.
    [ true ] whileTrue: [ ].
    ^ 0 )
) : (
)
"""


def post(url, body):
    req = urllib.request.Request(url, data=body if isinstance(body, bytes) else body.encode(), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def get(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def run_http():
    with serve(["--host", f"dev={VM}"], ready="/boards") as root:
        src = open(ADDITION).read()
        # build, fetch, load: the VM's boot names the image build answered.
        code, b = post(f"{root}/build?for=dev&name=Sum.nsl", src)
        assert code == 200 and b["Image"] == "Sum.oimg" and b["Board"] == "dev", (code, b)
        image = get(f"{root}/build/last.oimg")
        assert hashlib.sha256(image).hexdigest() == b["SHA256"] and len(image) == b["Size"], b
        m = json.loads(get(f"{root}/build/last.oimg.map.json"))
        assert m["files"][0] == "Sum.nsl" and m["image"]["sha256"] == b["SHA256"], m["image"]
        code, l = post(f"{root}/board/dev/load?name=Sum.oimg", image)
        assert code == 200 and l["Result"]["SHA256"] == b["SHA256"], (code, l)
        # run: the program's own output, and its exit.
        code, r = post(f"{root}/run?name=Sum.nsl", src)
        assert code == 200 and r["Exit"] == 0 and not r["TimedOut"], (code, r)
        assert "7" in r["Output"], r["Output"]
        # A compile error is the compiler's, by name, and nothing runs.
        code, e = post(f"{root}/run?name=Bad.nsl", "(3 + ) println")
        assert code == 422 and e["Error"], (code, e)
        # A bounded run time: a program that never ends is stopped and says so.
        t0 = time.time()
        code, f = post(f"{root}/run?name=Forever.nsl", FOREVER)
        took = time.time() - t0
        assert code == 200 and f["TimedOut"] and f["Exit"] == -1 and "forever" in f["Output"], (code, f)
        return (f"http: built Sum.oimg sha256 {b['SHA256'][:12]}, fetched, loaded (the VM's boot agrees); "
                f"run printed {r['Output'][-1]!r}, exit 0; a compile error 422 "
                f"({e['Error'].splitlines()[-1][:60]}); a forever program stopped after {took:.0f}s")


def run_tui():
    """The TUI's build tab, no terminal: build, load the build, run."""
    with serve(["--host", f"dev={VM}"], ready="/boards") as root:
        line = go_live("TestLiveBuild", {"CUSTOS_TUI_BUILD_URL": f"{root}/board/dev",
                                         "CUSTOS_TUI_BUILD_SOURCE": ADDITION})
    return "tui: " + " ".join(line)


def main():
    if shutil.which("go") is None:
        print("custos-build: SKIP (go is not on PATH)")
        sys.exit(77)
    build()
    http = run_http()
    tui = run_tui()
    print("test_custos_build: PASSED (Path B, /build and /run on a --host board)")
    print(http)
    print(tui)


if __name__ == "__main__":
    main()
