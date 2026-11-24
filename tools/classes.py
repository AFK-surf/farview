# Failure classes for a site: python3 tools/classes.py <site> [rows]
import json, sys, collections
site = sys.argv[1]; rows = int(sys.argv[2]) if len(sys.argv) > 2 else 8
c = json.load(open(f"tests/sites/{site}/rects.json"))
m = {}
for l in open(f"/tmp/fv-{site}.txt"):
    k, *v = l.split(); m[int(k)] = [float(x) for x in v]
ctr = collections.Counter(); ex = {}
for i, r in enumerate(c):
    if r[3] == 0 and r[4] == 0: continue
    a = m.get(i)
    if a is None:
        key = (r[0], r[5], 'none')
    else:
        if all(abs(x - y) <= 1 for x, y in zip(a, r[1:5])): continue
        d = [round(x - y, 1) for x, y in zip(a, r[1:5])]
        key = (r[5], ''.join(n for n, v in zip('xywh', d) if abs(v) > 1))
    ctr[key] += 1
    ex.setdefault(key, (i, r[0] + r[8], r[1:5], a))
for k, v in ctr.most_common(rows): print(k, v, ex[k])
