#!/usr/bin/env bash
# Copyright (c) 2026 Ekorau LLC
#
# coap_interop.sh — libcoap's coap-client against `porta serve --coap` (rete#505,
# design §8.3). A check-in over IPv6 and a blob fetch over IPv4, against a porta on
# a temporary store. Exit 0 and "COAP INTEROP OK", or exit 1 and "FAIL: …".
# There is no skip exit.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PORTA_DIR=$(cd "$HERE/.." && pwd)
UDP_PORT=37969
COAP_PORT=37683

fail() { echo "FAIL: $*"; exit 1; }

# A missing client FAILS rather than skips. The Go tests in internal/coapsrv
# use go-coap's own client, so they share the library with the server; this is
# the one check that does not share a cause with what it checks (design §8.3).
# libcoap3-bin installs the client under its TLS flavour's name on Debian/Ubuntu.
CLIENT=""
for c in coap-client coap-client-notls; do
	if command -v "$c" >/dev/null 2>&1; then CLIENT=$c; break; fi
done
[ -n "$CLIENT" ] || fail "no coap-client (nor coap-client-notls) — sudo apt install libcoap3-bin"

W=$(mktemp -d)
PID=""
cleanup() {
	if [ -n "$PID" ]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
	rm -rf "$W"
}
trap cleanup EXIT

(cd "$PORTA_DIR" && go build -o "$W/porta" ./cmd/porta) || fail "go build porta"

# The gate names its own port; there is no default to rely on.
"$W/porta" serve --db "$W/p.db" --udp "127.0.0.1:$UDP_PORT" --http-port 0 \
	--coap ":$COAP_PORT" >"$W/serve.log" 2>&1 &
PID=$!
for _ in $(seq 50); do
	grep -q "serving CoAP" "$W/serve.log" && break
	kill -0 "$PID" 2>/dev/null || { cat "$W/serve.log"; fail "porta serve exited"; }
	sleep 0.1
done
grep -q "serving CoAP" "$W/serve.log" || { cat "$W/serve.log"; fail "no 'serving CoAP' within 5 s"; }

"$W/porta" --db "$W/p.db" key gen >/dev/null || fail "porta key gen"
head -c 1000 /dev/urandom >"$W/img"
"$W/porta" --db "$W/p.db" blob put --node 0123456789ab "$W/img" >"$W/put.out" || fail "porta blob put"
HASH=$(awk '$1 == "hash" { print $2 }' "$W/put.out")
[ ${#HASH} -eq 64 ] || { cat "$W/put.out"; fail "no hash in blob put output"; }

# Check-in over IPv6: the reply must be the batch carrying the node's
# goal/apps/main, which only HandleFrom could have produced — so it holds the
# blob's 32 hash bytes. -t 60 is application/cbor.
xxd -r -p "$HERE/fixtures/pull.hex" >"$W/pull.cbor"
"$CLIENT" -B 10 -m post -t 60 -f "$W/pull.cbor" -o "$W/reply.cbor" \
	"coap://[::1]:$COAP_PORT/space" >"$W/post.log" 2>&1 || { cat "$W/post.log"; fail "coap-client POST /space"; }
python3 -I -c '
import sys
reply = open(sys.argv[1], "rb").read()
sys.exit(0 if bytes.fromhex(sys.argv[2]) in reply else 1)
' "$W/reply.cbor" "$HASH" || { xxd "$W/reply.cbor" | head; fail "check-in reply does not carry goal/apps/main's hash"; }

# The blob over IPv4, by Block2 at 256 B.
"$CLIENT" -B 10 -m get -b 256 -o "$W/got" \
	"coap://127.0.0.1:$COAP_PORT/blob/$HASH" >"$W/get.log" 2>&1 || { cat "$W/get.log"; fail "coap-client GET /blob"; }
GOT=$(sha256sum "$W/got" | awk '{ print $1 }')
[ "$GOT" = "$HASH" ] || fail "blob sha256 $GOT != $HASH ($(stat -c %s "$W/got") bytes)"

echo "COAP INTEROP OK"
