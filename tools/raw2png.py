# RGBA to PNG: python3 tools/raw2png.py in.raw out.png [width height] (1280x800 by default)
import sys, zlib, struct
w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (1280, 800)
d = open(sys.argv[1], "rb").read()
rows = b"".join(b"\0" + d[y * w * 4:(y + 1) * w * 4] for y in range(h))
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))
