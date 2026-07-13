#!/usr/bin/env python3
"""soak_replay — replay a real udp-echo6 journal through soak_web's health metric.

The dashboard's THIN/OK verdict is the whole point of the instrument, so it must
not be retuned by taste. Feed it real history and look at what it actually says.

    ssh david@gw85224-01 'journalctl -u udp-echo6 --since -12h --no-pager' > j.log
    ./soak_replay.py j.log              # verdict per node, at the log's last echo
    ./soak_replay.py j.log --sweep      # how the verdict moves with WINDOW_S

The --sweep mode is how WINDOW_S=3600 was chosen (2026-07-12): a 15-30 min window
lands BETWEEN bursts and calls a genuinely lossy node clean, while >=90 min lets a
long-dead outage re-brand a node that has since recovered. Both failures are
invisible unless you replay.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import soak_web as sw


def load(path):
    for line in open(path):
        sw.parse(line, sw.datetime.now().year)
    if not sw.nodes:
        sys.exit(f"no echo lines parsed from {path} — wrong journal format?")
    # evaluate as if "now" were the last echo in the log, not wall-clock
    return max(n["last"] for n in sw.nodes.values())


def verdict(n, now):
    misses, seen = sw.recent(n, now)
    rate = misses / max(1, misses + seen)
    silent = (now - n["last"]).total_seconds()
    state = ("DEAD" if silent > 4 * sw.CADENCE_S else
             "THIN" if rate > 0.02 else "OK")
    return state, misses, seen, rate


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    now = load(path)
    live = sorted((k, v) for k, v in sw.nodes.items()
                  if (now - v["last"]).total_seconds() < 60)

    if "--sweep" not in sys.argv:
        print(f"now = {now:%H:%M:%S} (last echo in log)\n")
        for (addr, port), n in sorted(sw.nodes.items(),
                                      key=lambda kv: kv[1]["last"], reverse=True):
            state, misses, seen, rate = verdict(n, now)
            kind = "gate" if port == "54360" else "soak"
            loss = "—" if state == "DEAD" else f"{misses} missed / {seen} seen ({rate:.1%})"
            print(f"…{addr[-9:]} {kind}: {state:4s}  {loss}")
        return

    print(f"{'window':>7} {'slots':>6} {'2% bar':>8}   verdicts (live nodes only)")
    for win in (900, 1800, 2700, 3600, 5400, 7200):
        sw.WINDOW_S = win
        out = []
        for (addr, _), n in live:
            state, misses, seen, rate = verdict(n, now)
            out.append(f"{addr[-9:]}={rate:5.1%} {state:4s}")
        slots = win // sw.CADENCE_S
        print(f"{win // 60:>5}m {slots:>6} {slots * .02:>7.1f}   {' | '.join(out)}")


if __name__ == "__main__":
    main()
