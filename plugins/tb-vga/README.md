# tb-vga plugin

Hot-pluggable stdvga/Bochs-compatible display device for `qdev-host`
(out-of-process port of the qemu-thunderbolt fork's `thunderbolt-vga`).

Plugin args (`--plugin-args`):

| Key | Default | Meaning |
|---|---|---|
| `edid=FILE` | none | EDID blob served at BAR2 offset 0 (e.g. `qemu-edid -x 1280 -y 800 -o edid.bin`) |
| `vram=MiB` | 16 | VRAM size, power of two, 4–256. The Linux VRAM helper wants 2 × frame size: 16 for 1280x800, 128 for 3840x2400 |

Guest view: PCI `1234:1111` rev 2 (subsystem `1af4:1100`), class VGA, PCIe
endpoint; BAR0 = VRAM (64-bit prefetchable, mmap-ed by QEMU, no exits),
BAR2 = 4 KiB registers (EDID @0x000, VGA ports @0x400, DISPI @0x500, QEXT @0x600).
Linux binds `bochs-drm`; the preferred EDID mode becomes the fbdev mode.

The scanout exported to the host is the DISPI mode (32 bpp): width/height from
XRES/YRES, stride from VIRT_WIDTH, offset from X/Y_OFFSET, pixman `x8r8g8b8`.

The VRAM memfd is created with `MFD_ALLOW_SEALING` and sealed with `F_SEAL_SHRINK`
so the host can wrap it into a dma-buf (udmabuf) for zero-copy KMS scan-out.
