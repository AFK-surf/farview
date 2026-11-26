#!/usr/bin/env python3
"""Write the icon pack package deps/icon_pack (gitignored) that pion/packages/shell_ui draws its icons from.

    (the empty pack, invar's own icons, is tools/icon_pack_empty/, copied by tools/deps_icons.sh)
    python3 tools/gen_icon_pack.py OUT_DIR OUTLINED_DIR FILLED_DIR  # Central Icons, from the unpacked npm packages

Only the names in tools/icon_map.txt are taken from the Central Icons packages: their license (Iconists) allows the
icons in an end product but not handing the set on, so the generated source must never be committed to this public
repository. tools/deps_icons.sh calls this; see its header.
"""
import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOLERANCE = 0.025  # largest control-point distance from the chord, in 24-unit grid units (as invar's gen_icons.py)


def read_map():
    rows = []
    for line in (ROOT / "tools/icon_map.txt").read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if line:
            name, comp, style = line
            if style not in ("outlined", "filled"):
                raise SystemExit(f"icon_map.txt: {name}: style must be outlined or filled")
            rows.append((name, comp, style))
    per_style = {}
    for _, comp, style in rows:
        per_style.setdefault(style, set()).add(comp)
    for style, comps in per_style.items():
        if len(comps) > 300:
            raise SystemExit(f"icon_map.txt: {len(comps)} {style} icons, the license allows at most 300 per style")
    return rows


# ---- SVG geometry -------------------------------------------------------------------------------------------------

def dist_to_segment(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    dx, dy = bx - ax, by - ay
    n = dx * dx + dy * dy
    if n == 0:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / n))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def split_cubic(a, b, c, d):
    def mid(p, q):
        return ((p[0] + q[0]) / 2, (p[1] + q[1]) / 2)
    ab, bc, cd = mid(a, b), mid(b, c), mid(c, d)
    abc, bcd = mid(ab, bc), mid(bc, cd)
    m = mid(abc, bcd)
    return (a, ab, abc, m), (m, bcd, cd, d)


class Outlines:
    def __init__(self):
        self.done = []
        self.points = []

    def move(self, p):
        self.end(False)
        self.points = [p]

    def line(self, p):
        p = (round(p[0], 4), round(p[1], 4))
        if not self.points or p != self.points[-1]:
            self.points.append(p)

    def cubic(self, a, b, c, d):
        if max(dist_to_segment(b, a, d), dist_to_segment(c, a, d)) <= TOLERANCE:
            self.line(d)
        else:
            h1, h2 = split_cubic(a, b, c, d)
            self.cubic(*h1)
            self.cubic(*h2)

    def end(self, closed):
        pts = self.points
        if closed and len(pts) > 1 and pts[-1] == pts[0]:
            pts = pts[:-1]
        if len(pts) >= (3 if closed else 2):
            self.done.append((pts, closed))
        self.points = []


def arc_to_cubics(p0, rx, ry, phi, large, sweep, p1):
    """SVG elliptical arc (endpoint form) as cubic Béziers (SVG 1.1 appendix F.6)."""
    if p0 == p1:
        return []
    if rx == 0 or ry == 0:
        return [(p0, p0, p1, p1)]
    rx, ry = abs(rx), abs(ry)
    cphi, sphi = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1p, y1p = cphi * dx + sphi * dy, -sphi * dx + cphi * dy
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        coef = -coef
    cxp, cyp = coef * rx * y1p / ry, -coef * ry * x1p / rx
    cx = cphi * cxp - sphi * cyp + (p0[0] + p1[0]) / 2
    cy = sphi * cxp + cphi * cyp + (p0[1] + p1[1]) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(1, int(math.ceil(abs(dt) / (math.pi / 2) - 1e-9)))
    step = dt / n
    k = 4 / 3 * math.tan(step / 4)
    out = []

    def pt(t):
        x, y = rx * math.cos(t), ry * math.sin(t)
        return (cphi * x - sphi * y + cx, sphi * x + cphi * y + cy)

    def deriv(t):
        x, y = -rx * math.sin(t), ry * math.cos(t)
        return (cphi * x - sphi * y, sphi * x + cphi * y)

    t = t1
    start = p0
    for i in range(n):
        t2 = t + step
        end = p1 if i == n - 1 else pt(t2)
        d1, d2 = deriv(t), deriv(t2)
        out.append((start, (start[0] + k * d1[0], start[1] + k * d1[1]), (end[0] - k * d2[0], end[1] - k * d2[1]), end))
        start, t = end, t2
    return out


TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def parse_path(d, pen):
    toks = TOKEN.findall(d)
    i = 0
    cmd = None
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    last_ctrl = None
    last_cmd = None

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def flag():
        # Arc flags may be packed without separators ("a1 1 0 011 1"): take one digit.
        nonlocal i
        t = toks[i]
        if len(t) > 1 and t[0] in "01":
            toks[i] = t[1:]
            return t[0] == "1"
        i += 1
        return float(t) != 0

    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                pen.end(True)
                cur = start
                last_ctrl, last_cmd = None, "Z"
                continue
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = cur if rel else (0.0, 0.0)
        if c == "M":
            p = (ox + num(), oy + num())
            pen.move(p)
            pen.line(p)
            cur = start = p
            cmd = "l" if rel else "L"
            last_ctrl = None
        elif c == "L":
            p = (ox + num(), oy + num())
            pen.line(p)
            cur, last_ctrl = p, None
        elif c == "H":
            p = ((cur[0] if rel else 0.0) + num(), cur[1])
            pen.line(p)
            cur, last_ctrl = p, None
        elif c == "V":
            p = (cur[0], (cur[1] if rel else 0.0) + num())
            pen.line(p)
            cur, last_ctrl = p, None
        elif c == "C":
            b = (ox + num(), oy + num())
            cc = (ox + num(), oy + num())
            p = (ox + num(), oy + num())
            pen.cubic(cur, b, cc, p)
            cur, last_ctrl = p, cc
        elif c == "S":
            b = (2 * cur[0] - last_ctrl[0], 2 * cur[1] - last_ctrl[1]) if last_cmd in ("C", "S") and last_ctrl else cur
            cc = (ox + num(), oy + num())
            p = (ox + num(), oy + num())
            pen.cubic(cur, b, cc, p)
            cur, last_ctrl = p, cc
        elif c in ("Q", "T"):
            if c == "Q":
                q = (ox + num(), oy + num())
            else:
                q = (2 * cur[0] - last_ctrl[0], 2 * cur[1] - last_ctrl[1]) if last_cmd in ("Q", "T") and last_ctrl else cur
            p = (ox + num(), oy + num())
            b = (cur[0] + 2 / 3 * (q[0] - cur[0]), cur[1] + 2 / 3 * (q[1] - cur[1]))
            cc = (p[0] + 2 / 3 * (q[0] - p[0]), p[1] + 2 / 3 * (q[1] - p[1]))
            pen.cubic(cur, b, cc, p)
            cur, last_ctrl = p, q
        elif c == "A":
            rx, ry, phi = num(), num(), num()
            large, sweep = flag(), flag()
            p = (ox + num(), oy + num())
            for seg in arc_to_cubics(cur, rx, ry, phi, large, sweep, p):
                pen.cubic(*seg)
            cur, last_ctrl = p, None
        else:
            raise ValueError(f"unsupported path command {cmd}")
        last_cmd = c
    pen.end(False)
    return pen.done


def circle_path(cx, cy, r):
    return f"M{cx - r} {cy}A{r} {r} 0 1 0 {cx + r} {cy}A{r} {r} 0 1 0 {cx - r} {cy}Z"


# ---- Central Icons React modules ------------------------------------------------------------------------------------

ELEMENT = re.compile(r'createElement\("([a-z]+)",\{([^{}]*)\}')
ATTR = re.compile(r'(\w+):(?:"([^"]*)"|([-\d.]+))')


def read_layers(module):
    """The drawn elements of one icon's index.mjs: those after the svg wrapper (which carries maskId)."""
    src = module.read_text()
    at = src.rfind('maskId:"')
    if at < 0:
        raise ValueError("no icon body")
    layers = []
    for tag, body in ELEMENT.findall(src[at:]):
        attrs = {k: (v if v else n) for k, v, n in ATTR.findall(body)}
        if tag == "path":
            d = attrs["d"]
        elif tag == "circle":
            d = circle_path(float(attrs["cx"]), float(attrs["cy"]), float(attrs["r"]))
        elif tag in ("g", "title"):
            continue
        else:
            raise ValueError(f"unsupported element {tag}")
        fill = attrs.get("fill", "none") not in ("none", "")
        stroke = attrs.get("stroke", "none") not in ("none", "")
        for k in ("opacity", "fillOpacity", "strokeOpacity", "transform"):
            if k in attrs:
                raise ValueError(f"unsupported attribute {k}")
        sw = float(attrs.get("strokeWidth", "2"))
        if not fill and not stroke:
            continue
        outlines = parse_path(d, Outlines())
        if fill:
            outlines = [(pts, True) for pts, _ in outlines if len(pts) >= 3]
        if outlines:
            layers.append((outlines, fill, stroke, attrs.get("fillRule") == "evenodd", sw / 2.0))
    if not layers:
        raise ValueError("empty icon")
    return layers


def pion_shapes(layers):
    lines = []
    for outlines, fill, stroke, even_odd, rel in layers:
        lines.append("      paint::shape_of([")
        for points, closed in outlines:
            values = [repr(float(v)) for pair in points for v in pair]
            lines.append("        (scaled([")
            for i in range(0, len(values), 10):
                lines.append("          " + ", ".join(values[i:i + 10]) + ",")
            lines.append(f"        ], scale), {str(closed).lower()}),")
        params = []
        if fill:
            params.append("fill=ink")
            if even_odd:
                params.append("even_odd=true")
        if stroke:
            params.extend(("stroke=ink", "width=width" if rel == 1.0 else f"width=width * {rel!r}"))
        lines.append("      ], " + ", ".join(params) + "),")
    return lines


HEADER = """\
// Generated by tools/gen_icon_pack.py. Do not edit and never commit (deps/ is gitignored).
"""

COMMON = """\
// Each generated contour is a fresh array owned by its shape.
fn scaled(points : Array[Double], scale : Double) -> Array[Double] {
  if scale != 1.0 {
    for i in 0..<points.length() {
      points[i] = points[i] * scale
    }
  }
  points
}
"""


def generate(out, outlined, filled):
    src = out / "src"
    src.mkdir(parents=True, exist_ok=True)
    (out / "pion.toml").write_text(
        '[package]\nname = "icon_pack"\nversion = "0.1.0"\n'
        + ('license = "MIT"\n' if outlined is None else 'license = "LicenseRef-Iconists"\n')
        + '\n[dependencies]\npaint = { path = "../invar/packages/paint" }\n'
    )
    lines = [HEADER]
    if outlined is None:
        lines += [
            "// The empty pack: shell_ui draws invar's own open icons (packages/icons).",
            'import "paint"',
            "",
            'pub fn pack_name() -> String {\n  "invar"\n}',
            "",
            "/// Grid units of stroke per unit of invar's weight width (unused: the empty pack draws nothing).",
            "pub fn pack_stroke_ratio() -> Double {\n  1.0\n}",
            "",
            "pub fn pack_names() -> Array[String] {\n  []\n}",
            "",
            "pub fn pack_shapes(name : String, scale : Double, ink : Int64, width : Double) -> Option[Array[paint::Shape]] {",
            "  let _ = (name, scale, ink, width)",
            "  None",
            "}",
            "",
        ]
    else:
        rows = read_map()
        lines += [
            "// Central Icons by Iconists (https://iconists.co/central), round-outlined / round-filled, radius 2, stroke 2:",
            "// licensed for use in Farview's own builds; not to be redistributed as icons. Names: tools/icon_map.txt.",
            'import "paint"',
            "",
            COMMON,
            'pub fn pack_name() -> String {\n  "central"\n}',
            "",
            "/// Central draws a 2-unit stroke where invar's Regular weight draws 1.75.",
            "pub fn pack_stroke_ratio() -> Double {\n  2.0 / 1.75\n}",
            "",
            "pub fn pack_names() -> Array[String] {",
            "  [",
            *[f"    {json.dumps(n)}," for n, _, _ in rows],
            "  ]",
            "}",
            "",
            "pub fn pack_shapes(name : String, scale : Double, ink : Int64, width : Double) -> Option[Array[paint::Shape]] {",
            "  match name {",
        ]
        for name, comp, style in rows:
            module = (outlined if style == "outlined" else filled) / comp / "index.mjs"
            try:
                layers = read_layers(module)
            except (ValueError, KeyError, OSError) as error:
                raise SystemExit(f"{name} ({comp}, {style}): {error}")
            lines.append(f'    "{name}" => Some([')
            lines += pion_shapes(layers)
            lines.append("    ])")
        lines += ["    _ => None", "  }", "}", ""]
    (src / "pack.pion").write_text("\n".join(lines))


if __name__ == "__main__":
    if len(sys.argv) == 2:
        generate(Path(sys.argv[1]), None, None)
    elif len(sys.argv) == 4:
        generate(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
    else:
        raise SystemExit(__doc__)
