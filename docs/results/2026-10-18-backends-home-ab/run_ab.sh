#!/usr/bin/env bash
# run_ab.sh IMG OUT TAG... — every TAG's AWFY and STIC on c6spare, two passes, TAGs interleaved; custos lends the cable.
set -u
IMG=$1 OUT=$2; shift 2; TAGS=$*
E=$HOME/.espressif/python_env/idf5.4_py3.12_env/bin
D=$(cd "$(dirname "$0")" && pwd)
source ~/workspace/rete/nodus/oevm/tools/bench-identity.sh
PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_d2e21074c7beed11a5935a4371c9e8b5-if00-port0
mkdir -p "$OUT"
bench::lend c6spare 5400 || { echo "custos would not lend c6spare"; exit 1; }
trap 'bench::return c6spare; echo "[ab] cable returned"' EXIT
sleep 2
for pass in 1 2; do for t in $TAGS; do for p in awfy performance; do
  ESPTOOL=$E/esptool.py C6_PORT=$PORT C6_TIMEOUT=900 $E/python $D/c6run.py "$OUT/$t-$p-$pass.log" done \
    0x0 "$IMG/c6-$t-$p.bin" 0x220000 "$IMG/c6-$t-$p.oimg"
  echo "[ab] pass $pass $t $p rc $?"
done; done; done
