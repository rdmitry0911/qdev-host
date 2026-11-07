#!/usr/bin/env python3
# Runs inside the guest: animates on the fbdev of the given PCI device for a
# number of frames as fast as it can (a moving bar + frame counter band), then
# prints achieved fps. Used to load the host's damage detection and KMS sink.
import fcntl, os, struct, sys, time
pci = sys.argv[1] if len(sys.argv) > 1 else "0000:01:00.0"
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 600
full = len(sys.argv) > 3 and sys.argv[3] == "full"     # full-frame updates instead of a moving bar
fb = None
for f in os.listdir("/sys/class/graphics"):
    d = f"/sys/class/graphics/{f}/device"
    if f.startswith("fb") and os.path.exists(d) and os.path.basename(os.readlink(d)) == pci: fb = f
if not fb: print("no fbdev for", pci); sys.exit(1)
dev = os.open("/dev/" + fb, os.O_RDWR)
vinfo = bytearray(160); fcntl.ioctl(dev, 0x4600, vinfo)
w, h, xv, yv, xo, yo, bpp = struct.unpack_from("<7I", vinfo, 0)
struct.pack_into("<7I", vinfo, 0, w, h, w, h, 0, 0, 32); struct.pack_into("<I", vinfo, 27 * 4, 128)
fcntl.ioctl(dev, 0x4601, vinfo); fcntl.ioctl(dev, 0x4600, vinfo)
w, h, xv, yv, xo, yo, bpp = struct.unpack_from("<7I", vinfo, 0)
print(f"{fb}: {w}x{h} bpp {bpp}, {frames} frames, {'full' if full else 'bar'} updates")
import mmap
m = mmap.mmap(dev, w * h * 4, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)
# background once
bg = bytes(bytearray([64, 64, 64, 0]) * w)
for y in range(h): m[y * w * 4:(y + 1) * w * 4] = bg
bar_w = 64; bar_h = h // 4
colors = [bytes([255, 0, 0, 0]), bytes([0, 255, 0, 0]), bytes([0, 0, 255, 0]), bytes([0, 255, 255, 0])]
t0 = time.time(); last = t0
for i in range(frames):
    c = colors[i % 4]
    if full:
        row = c * w
        for y in range(h): m[y * w * 4:(y + 1) * w * 4] = row
    else:
        x = (i * 16) % (w - bar_w)
        row = c * bar_w
        for y in range(h // 2, h // 2 + bar_h): m[(y * w + x) * 4:(y * w + x + bar_w) * 4] = row
        # erase the previous bar
        px = ((i - 1) * 16) % (w - bar_w) if i else x
        if px != x:
            row = bytes([64, 64, 64, 0]) * bar_w
            for y in range(h // 2, h // 2 + bar_h): m[(y * w + px) * 4:(y * w + px + bar_w) * 4] = row
    # frame counter band (top-left): width proportional to i
    band = c * ((i % 256) + 1)
    m[0:len(band)] = band
t1 = time.time()
print(f"guest: {frames} frames in {t1 - t0:.2f} s = {frames / (t1 - t0):.1f} fps")
