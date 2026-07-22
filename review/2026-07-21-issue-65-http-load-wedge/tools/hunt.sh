#!/usr/bin/env bash
# #65 wedge hunt. Declares a wedge ONLY on sustained, multi-probe failure of BOTH
# ICMP and HTTP -- a single failed curl right after a burst is a transient blip,
# not the fatal mode (learned the hard way: a one-shot probe gave a false positive).
URL=http://192.168.0.240:18080/thermal
IP=192.168.0.240
H=/home/david/workspace/tuvm/zephyr/tools/latency_harness.py
for r in $(seq 1 25); do
  out=$(timeout 200 python3 $H $URL -n 300 --delay 0 --timeout 2 2>&1 | awk '/^n /{print $2"ok/"$4"fail"}')
  bad=0
  for p in 1 2 3 4 5; do
    sleep 5
    ping -c 1 -W 2 $IP >/dev/null 2>&1 && pk=up || pk=DOWN
    hc=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 $URL)
    [ "$pk" = "DOWN" ] && [ "$hc" != "200" ] && bad=$((bad+1))
  done
  echo "$(date +%H:%M:%S) round $r: $out | sustained-fail probes: $bad/5"
  if [ "$bad" -ge 5 ]; then echo ">>> WEDGE CONFIRMED (5/5 probes failed both ICMP and HTTP) at $(date +%H:%M:%S)"; break; fi
done
echo "hunt finished $(date +%H:%M:%S)"
