#!/usr/bin/env python3
# Runs inside the guest: animates on the fbdev of the given PCI device for a
# number of frames as fast as it can (a moving bar + frame counter band), then
# prints achieved fps. Used to load the host's damage detection and KMS sink.
import fcntl, mmap, os, struct, sys, time
pci = sys.argv[1] if len(sys.argv) > 1 else "0000:01:00.0"
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 600
full = len(sys.argv) > 3 and sys.argv[3] == "full"     # full-frame updates instead of a moving bar
fps = float(sys.argv[4]) if len(sys.argv) > 4 else 0    # pace frames (0 = as fast as possible)
raw = len(sys.argv) > 5 and sys.argv[5] == "raw"        # write straight into BAR0 (bypasses fbdev's deferred I/O)
W = H = 0
if raw:
    # Device-level path: detach the kernel driver (a busy BAR cannot be mmap-ed with
    # CONFIG_IO_STRICT_DEVMEM), program the DISPI registers through BAR2 and write
    # frames straight into VRAM through BAR0. Bypasses fbdev's deferred I/O.
    import struct
    dev_dir = f"/sys/bus/pci/devices/{pci}"
    if os.path.exists(dev_dir + "/driver"):
        open(dev_dir + "/driver/unbind", "w").write(pci)
    cfg = os.open(dev_dir + "/config", os.O_RDWR)
    cmd = struct.unpack("<H", os.pread(cfg, 2, 4))[0]
    os.pwrite(cfg, struct.pack("<H", cmd | 0x2), 4)          # memory decoding on
    r2 = os.open(dev_dir + "/resource2", os.O_RDWR | os.O_SYNC)
    regs = mmap.mmap(r2, 4096, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)
    def dispi_w(i, v): regs[0x500 + i * 2:0x500 + i * 2 + 2] = struct.pack("<H", v)
    def dispi_r(i): return struct.unpack("<H", regs[0x500 + i * 2:0x500 + i * 2 + 2])[0]
    W, H = dispi_r(1), dispi_r(2)
    if not (W and H and dispi_r(4) & 1):
        W, H = int(os.environ.get("W", 3840)), int(os.environ.get("H", 2400))
        dispi_w(4, 0); dispi_w(1, W); dispi_w(2, H); dispi_w(3, 32); dispi_w(6, W); dispi_w(7, H); dispi_w(8, 0); dispi_w(9, 0); dispi_w(4, 0x41)
    w, h, bpp = W, H, 32
    r0 = os.open(dev_dir + "/resource0", os.O_RDWR | os.O_SYNC)
    m = mmap.mmap(r0, w * h * 4, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)
    print(f"raw BAR0: {w}x{h} bpp {bpp}, {frames} frames, {'full' if full else 'bar'} updates, pace {fps or 'max'} fps")
else:
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
    print(f"{fb}: {w}x{h} bpp {bpp}, {frames} frames, {'full' if full else 'bar'} updates, pace {fps or 'max'} fps, fbdev")
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
    if fps:
        nxt = t0 + (i + 1) / fps
        d = nxt - time.time()
        if d > 0: time.sleep(d)
t1 = time.time()
print(f"guest: {frames} frames in {t1 - t0:.2f} s = {frames / (t1 - t0):.1f} fps")
