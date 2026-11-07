#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG without external libraries."""
import struct, sys, zlib
def main(src, dst):
    data = open(src, "rb").read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split()); maxv = int(parts[2]); px = parts[3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png); print(f"{dst}: {w}x{h}")
if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
