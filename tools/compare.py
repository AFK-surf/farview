# Compares our boxes with Chrome's: python3 tools/compare.py <site> <ours> [how many roots of failure to list].
# Right: within 1 px on x, y, w and h. Sizes alone: within 1 px on w and h. A failure inside a failed box is not listed.
import json, sys
site, ours = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0
chrome = json.load(open(f"tests/sites/{site}/rects.json"))
mine = {}
for line in open(ours):
    k, x, y, w, h = line.split()
    mine[int(k)] = tuple(map(float, (x, y, w, h)))
tot = ok = size_ok = 0
shown = []
lines = []
for i, r in enumerate(chrome):
    tag, x, y, w, h = r[:5]
    if w == 0 and h == 0:
        continue
    tot += 1
    m = mine.get(i)
    if m and abs(m[2] - w) <= 1 and abs(m[3] - h) <= 1:
        size_ok += 1
    if m and all(abs(a - b) <= 1 for a, b in zip(m, (x, y, w, h))):
        ok += 1
        continue
    if tag in ("html", "body"):
        continue
    inside = any(a[0] <= x + .5 and a[1] <= y + .5 and x + w <= a[0] + a[2] + .5 and y + h <= a[1] + a[3] + .5 for a in shown)
    if not inside and len(lines) < limit:
        shown.append((x, y, w, h))
        mm = tuple(round(v, 1) for v in m) if m else None
        lines.append(f"  {i} {tag}{r[8] if len(r)>8 else ''} {r[5]} {r[9:11] if len(r)>10 else ''} chrome {(x, y, w, h)} ours {mm} {r[12] if len(r)>12 else ''!r}")
print(f"{site}: {ok}/{tot} = {ok*100/max(tot,1):.1f}% boxes, sizes {size_ok*100/max(tot,1):.1f}%")
print("\n".join(lines))
