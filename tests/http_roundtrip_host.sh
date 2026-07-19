#!/usr/bin/env bash
# Task 6 host integration: the webserver-channel-device spike over REAL posix
# sockets. Bakes HttpServerNode.oimg, builds the image VM (http_poll runs inside
# pumpEvents on the single reactor thread — no accept thread), runs it listening
# on 8080, and drives it with curl:
#   GET /thermal -> 200 with an SVG heat-grid (body contains rgb()),
#   GET /nope    -> 404,
#   an 8-way concurrent burst (informational: with a 4-deep in-flight table some
#   requests may shed 503 under load — the deterministic shed proof is the C unit
#   test test_backpressure_shed + nsl-tests/14_http/03_backpressure).
# Correctness of the round-trip over sockets is the deliverable.
set -euo pipefail
cd "$(dirname "$0")/.."
NSL="$HOME/workspace/nsl-tuvm/tools/ns_compile_oevm.py"
TARGET="${OEVM_TARGET:-posix}"
PORT="${OEVM_HTTP_PORT:-18080}"   # 8080 is commonly taken (e.g. Jetty); node hardcodes 18080

echo "[1/5] host-ABI layout descriptor"
gcc -std=c99 -Isrc tools/dump_layout.c -o /tmp/dl
/tmp/dl > /tmp/layout.json

echo "[2/5] compile HttpServerNode.nsl -> .oimg"
python3 "$NSL" zephyr/HttpServerNode.nsl /tmp/http_server.oimg --format=oimg --layout=/tmp/layout.json

echo "[3/5] build image VM (http_poll, no accept thread)"
gcc -Wall -std=c99 -Isrc \
    src/vm.c src/gc.c src/dispatch.c src/platform.c src/space.c src/handles.c \
    "src/backend/$TARGET/platform_$TARGET.c" src/serial.c \
    "src/backend/$TARGET/serial_$TARGET.c" src/udp.c "src/backend/$TARGET/udp_$TARGET.c" \
    src/http.c src/http_core.c "src/backend/$TARGET/http_$TARGET.c" \
    "src/backend/$TARGET/pulse_$TARGET.c" "src/backend/$TARGET/timer_$TARGET.c" \
    src/mirror.c \
    src/oimg_loader.c src/kernel_boot.c oevm_image_main.c -o /tmp/oevm_http -lm

echo "[4/5] launch server on :$PORT"
/tmp/oevm_http /tmp/http_server.oimg &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
# wait for the listener to come up
for _ in $(seq 1 50); do
  if curl -s -o /dev/null "http://127.0.0.1:$PORT/hello" 2>/dev/null; then break; fi
  sleep 0.1
done

echo "[5/5] drive with curl"
code=$(curl -s -o /tmp/http_body -w '%{http_code}' "http://127.0.0.1:$PORT/thermal")
echo "  GET /thermal -> $code"
test "$code" = "200"
grep -q "rgb(" /tmp/http_body || { echo "FAIL: /thermal body has no rgb()"; exit 1; }
echo "  /thermal body contains rgb() — heat-grid rendered"

nope=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/nope")
echo "  GET /nope -> $nope"
test "$nope" = "404"

echo "  concurrent burst (8-way, informational):"
for _ in $(seq 1 8); do curl -s -o /dev/null -w '%{http_code}\n' "http://127.0.0.1:$PORT/thermal" & done | sort | uniq -c | sed 's/^/    /'

echo "http_roundtrip OK"
