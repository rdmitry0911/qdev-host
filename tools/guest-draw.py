#!/usr/bin/env python3
# Runs inside the guest: find the fbdev of the PCI device at $1 (default 0000:01:00.0),
# set 1280x800x32 via fbdev ioctl if needed, write a gradient + checkerboard pattern.
import fcntl, os, struct, sys
pci = sys.argv[1] if len(sys.argv) > 1 else "0000:01:00.0"
fb = None
for f in os.listdir("/sys/class/graphics"):
    if f.startswith("fb") and os.path.exists(f"/sys/class/graphics/{f}/device"):
        if os.path.basename(os.readlink(f"/sys/class/graphics/{f}/device")) == pci:
            fb = f
if not fb:
    print("no fbdev for", pci); sys.exit(1)
dev = os.open("/dev/" + fb, os.O_RDWR)
FBIOGET_VSCREENINFO = 0x4600; FBIOPUT_VSCREENINFO = 0x4601
vinfo = bytearray(160)
fcntl.ioctl(dev, FBIOGET_VSCREENINFO, vinfo)
xres, yres, xv, yv, xo, yo, bpp = struct.unpack_from("<7I", vinfo, 0)
print(f"{fb}: {xres}x{yres} virtual {xv}x{yv} bpp {bpp}")
# Always commit the mode: FBIOPUT -> fb_set_par -> DRM modeset, which makes the
# driver program the device (DISPI registers) and pin the framebuffer in VRAM.
FB_ACTIVATE_NOW, FB_ACTIVATE_FORCE = 0, 128
struct.pack_into("<7I", vinfo, 0, 1280, 800, 1280, 800, 0, 0, 32)
struct.pack_into("<I", vinfo, 27 * 4, FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE)   # activate field
fcntl.ioctl(dev, FBIOPUT_VSCREENINFO, vinfo)
fcntl.ioctl(dev, FBIOGET_VSCREENINFO, vinfo)
xres, yres, xv, yv, xo, yo, bpp = struct.unpack_from("<7I", vinfo, 0)
print(f"after modeset: {xres}x{yres} virtual {xv}x{yv} bpp {bpp}")
w, h = xres, yres
img = bytearray()
for y in range(h):
    row = bytearray(w * 4)
    for x in range(w):
        i = x * 4
        row[i] = 128 if (x // 80 + y // 80) % 2 else 32   # B
        row[i + 1] = y * 255 // h                           # G
        row[i + 2] = x * 255 // w                           # R
    img += row
os.lseek(dev, 0, 0); n = os.write(dev, img)
import time; time.sleep(0.5)   # let fbdev deferred I/O flush the damage into VRAM
print(f"wrote {n} bytes to /dev/{fb}")
