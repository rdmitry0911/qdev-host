#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG without external libraries."""
import struct, sys, zlib
def main(src, dst, scale=1):
    data = open(src, "rb").read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split()); maxv = int(parts[2]); px = parts[3]
    if scale > 1:   # nearest-neighbour downscale by an integer factor
        rows = []
        for y in range(0, h, scale):
            row = px[y * w * 3:(y + 1) * w * 3]
            rows.append(b"".join(row[x * 3:x * 3 + 3] for x in range(0, w, scale)))
        w, h = (w + scale - 1) // scale, (h + scale - 1) // scale
        px = b"".join(rows)
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png); print(f"{dst}: {w}x{h}")
if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 1)
