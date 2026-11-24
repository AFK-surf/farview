#!/usr/bin/env python3
# pngink.py a.png [b.png...]: each picture's share of pixels unlike its own background (its most common colour), on a
# grid of at most 160 × 100 samples, as a percentage: how much a page drew, to compare a client's picture with the
# host's of the same page (a blank or half-drawn one shows as much less).
import struct, sys, zlib

def pixels(path):
    d = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t = d[i + 4:i + 8]
        if t == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", d[i + 8:i + 18])
        elif t == b"IDAT":
            idat += d[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    bpp = {6: 4, 2: 3, 0: 1, 4: 2}[ct]
    stride = w * bpp
    rows, prev, j = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[j]
        line = bytearray(raw[j + 1:j + 1 + stride])
        j += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows

def ink(path):
    w, h, bpp, rows = pixels(path)
    sx, sy = max(1, w // 160), max(1, h // 100)
    samples = [tuple(rows[y][x * bpp:x * bpp + 3]) for y in range(0, h, sy) for x in range(0, w, sx)]
    counts = {}
    for s in samples:
        counts[s] = counts.get(s, 0) + 1
    bg = max(counts, key=counts.get)
    unlike = sum(1 for s in samples if max(abs(s[k] - bg[k]) for k in range(3)) > 24)
    return 100.0 * unlike / len(samples)

for p in sys.argv[1:]:
    try:
        print("%.1f" % ink(p))
    except Exception as e:
        print("-1")
