# Lists Chrome's and our boxes for a range: python3 tools/near.py <site> <from> <count>
import json, sys
site, a, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
c = json.load(open(f"tests/sites/{site}/rects.json"))
m = {}
for l in open(f"/tmp/fv-{site}.txt"):
    k, *v = l.split(); m[int(k)] = [round(float(x), 1) for x in v]
for i in range(a, min(a + n, len(c))):
    r = c[i]
    print(i, r[0] + r[8], r[5], r[9], r[10], r[1:5], m.get(i), (r[7] if i % 1 == 0 else ""), repr(r[12][:18]))
