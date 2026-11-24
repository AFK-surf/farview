# The first boxes that differ from Chrome's after a given element: python3 tools/first.py <site> [from] [count]
import json, sys
site = sys.argv[1]; a = int(sys.argv[2]) if len(sys.argv) > 2 else 0; n = int(sys.argv[3]) if len(sys.argv) > 3 else 12
c = json.load(open(f"tests/sites/{site}/rects.json"))
m = {}
for l in open(f"/tmp/fv-{site}.txt"):
    k, x, y, w, h = l.split(); m[int(k)] = tuple(map(float, (x, y, w, h)))
for i, r in enumerate(c):
    if i < a or (r[3] == 0 and r[4] == 0): continue
    mm = m.get(i)
    if not mm or any(abs(p - q) > 1 for p, q in zip(mm, r[1:5])):
        print(i, r[0], r[8][:40], r[5], r[1:5], [round(v, 1) for v in mm] if mm else None, repr(r[12][:30]) if len(r) > 12 else '')
        n -= 1
        if n == 0: break
