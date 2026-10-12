import re, sys
def load(p):
    rows, cur, gcs = [], [], 0
    for ln in open(p):
        m = re.match(r'CENSUS (\d+) (\d+)', ln)
        if m: cur.append((int(m[1]), int(m[2]))); continue
        m = re.match(r'(\w+): (\d+)$', ln.strip())
        if m:
            rows.append((m[1], len(cur), max([c[0] for c in cur], default=0), max([c[1] for c in cur], default=0))); cur = []
    return rows
a, b = load(sys.argv[1]), load(sys.argv[2])
print('| row | GCs master / hdr | peak live objects | peak live bytes master | hdr | saved | ÷ |')
print('|---|--:|--:|--:|--:|--:|--:|')
for (n, ga, oa, ba), (_, gb, ob, bb) in zip(a, b):
    r = f'{bb/ba:.3f}' if ba else '–'
    print(f'| {n} | {ga} / {gb} | {oa} / {ob} | {ba} | {bb} | {ba-bb} | {r} |')
