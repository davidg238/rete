#!/usr/bin/env bash
set -e
cd "$(dirname "$0")/.."
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dump_layout
/tmp/dump_layout > /tmp/layout.json
python3 - <<'PY'
import json
d = json.load(open("/tmp/layout.json"))
assert d["oop_size"] in (4, 8), d["oop_size"]
sym = d["structs"]["Symbol"]
assert sym["fields"]["name"]["is_ptr"] is True
assert sym["fields"]["base.vtable"]["is_ref"] is True
vt = d["structs"]["Vtable"]
assert vt["fields"]["delegate"]["is_ref"] is True
assert vt["fields"]["bindings"]["is_ptr"] is True
print("layout OK")
PY
