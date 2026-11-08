# apple-pvg plugin

Apple ParavirtualizedGraphics (PVG) GPU for `qdev-host`: the macOS guest's stock
`AppleParavirtGPU` binds it as on Apple's hypervisor. Out-of-process port of the
`apple-gfx-ml` device of the apple-virgl QEMU fork (`hw/display/apple-gfx-ml.c`
at `c43589029b`), without the fork's env-gated guest-scheduler diagnostics
(they read vCPU state). Rendering, FIFOs and the guest protocol are in QMetal
(`libmlapi.so`, unchanged); the plugin is the device glue. Needs a qdp v2 host.

Guest view: PCI `106b:eeee` rev 0 (or `eee1`), no subsystem IDs, class 03.80,
PCIe config space with one capability, MSI (1 vector, 64-bit, no mask), no
INTx; BAR0 = 16 KiB MMIO, 32-bit, non-prefetchable, every access trapped (the
two read-and-clear status registers must stay trapped). The option ROM is the
client's: `-device vfio-user-pci,socket=...,romfile=ApplePVG-stock-eeee.rom`
(the ROM's PCIR IDs must match the personality; cold-plug for the EFI GOP).

Plugin args (`--plugin-args`; the fork's property names are accepted):

| Key | Default | Meaning |
|---|---|---|
| `xres=`, `yres=` | 1920, 1080 | primary mode (mode 0; modes 1, 2 are 1440x1080 and 1280x1024) |
| `vram_size_mb=` (`vram=`) | 512 | internal VRAM for QMetal (not a BAR); production uses 256 |
| `debug=` | 0 | QMetal debug level |
| `vsync=`, `direct_scanout=` | on, off | passed to QMetal |
| `spirv_cache_dir=` | none | SPIR-V / VkPipelineCache directory |
| `device-id=` (`x-pci-device-id=`), `vendor-id=` (`x-pci-vendor-id=`) | 0xeeee, 0x106b | personality, e.g. `device-id=0xeee1` |
| `log=` | `stderr` | device log sink: `stderr` keeps the fork's `[apple-gfx-ml]` lines unchanged; `host` routes them through `qdp_host_ops.log` |
| `exit-on-disconnect=` | on | when the client goes away: run the quit stop phase, then exit the process (GPU state refers to the lost guest's memory) |

Build (QMetal headers and the libmlapi to link against):

    make QMETAL_INCLUDE=<qmetal>/include QMETAL_LIBDIR=<dir of libmlapi.so> plugins/apple-pvg/apple-pvg.so

The plugin gets `QMETAL_LIBDIR` as rpath (`QMETAL_RPATH=` to override or drop).
`libmlapi.so` itself needs `libair2spirv.so.1`: start qdev-host with the
package's `LD_LIBRARY_PATH` (`PKG/runtime/lib:PKG/lib`), its Vulkan ICD
variables, the `QMU_DIAG_*` variables and, for production speed, jemalloc
(`LD_PRELOAD=libjemalloc.so.2`), as the fork's QEMU had them.

Guest memory: QEMU must give fd-backed RAM (`-object memory-backend-memfd,
share=on` as machine memory, seal=on default; no vIOMMU). The plugin maps each
DMA region itself from a dup of its fd, so a mapping QMetal holds stays valid
after the client unmaps it (the fork's MemoryRegion reference). Targets that
are not fd-backed RAM fail and are logged as `[apple-gfx-ml][memory][ERROR]`;
there is no message-DMA fallback. udmabuf exports (`/dev/udmabuf`) are built
over the same memfds.

Threads: MMIO runs on the host's vfio-user thread; `agfx-main` runs what the
fork ran in QEMU main-loop BHs (display completions, frame chain, mode, cursor),
in order; `agfx-render` x2 encode owned display frames; `agfx-present` is the
100 ms EFI present timer; `agfx-log` writes the device log. quiesce() holds IRQs
and lookups until resume(); a held IRQ is delivered by resume().

Display: QMetal reads each presented frame into one of three buffers of a memfd
(`x8r8g8b8`, stride = 4 x width); `frame_ready()` announces it
(`QDP_F_PLUGIN_DAMAGE`, whole-frame damage). A buffer is not rewritten while it
is the front one or the one the host fetched last with `get_scanout()`. The
cursor (`get_cursor()`) is the fork's ARGB conversion of the guest glyph.

Self-test without a VM (real libmlapi on the GPU; lavapipe cannot open QMetal):

    make QMETAL_INCLUDE=... QMETAL_LIBDIR=... plugins/apple-pvg/apple-pvg.so plugins/apple-pvg/selftest
    LD_LIBRARY_PATH=PKG/runtime/lib:PKG/lib VK_ICD_FILENAMES=PKG/runtime/amd/radv-icd.json \
        plugins/apple-pvg/selftest plugins/apple-pvg/apple-pvg.so
    ... plugins/apple-pvg/selftest plugins/apple-pvg/apple-pvg.so --personality 0xeee1

It loads the plugin on a libvfio-user context with a host-services double,
checks config space, drives BAR0 (init registers, FIFO setup, a root-FIFO NOP
whose stamp QMetal writes back to guest memory with an MSI), non-RAM refusals,
quiesce/resume, scanout, reset, the quit stop phase and the lost-connection exit.
