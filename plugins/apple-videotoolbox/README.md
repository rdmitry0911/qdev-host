# apple-videotoolbox — AppleGVA host backend (increments 1-2)

Host side of Apple's paravirtualized **VideoToolbox** codec device
(PCI `106b:1a03`), as a qdev-host plugin. Lane L-M2V03, 2026-11-16.

A multi-increment effort. Increment 1 brought up the host device skeleton +
the VA-API decode backend; increment 2 implemented the virtio split-virtqueue
ring state machine (the E2E connector the guest kext binds). All buildable and
unit-tested without a VM.

## Clean-room

Every file here is original, written from:
- the device/ABI contract in
  `analysis/20261115-APPLEGVA-VIDEO-HOST-CONTRACT.md` (apple-virgl commits
  `47f5ef89`, `18a6501c`; §7 is the F0-GVA verdict);
- the qdev-host plugin ABI (`qdp.h`) and libvfio-user headers;
- the H.264 / HEVC and libva (`<va/va.h>`, `<va/va_drm.h>`) specifications.

It does **not** copy, port or transcribe code from the reims reference
(`research/reims/**`, GPL/LGPL) or its QEMU `apple-videotoolbox-pci.c`. The
reims *docs* were read for the device **contract** (what Apple's host does)
only — the PCI identity, the two-queue transport, the 256-descriptor cap and
the >1 MiB sample limiter are facts about Apple's device, not reims's code.

## F0-GVA / why the guest half is blocked

Per §7 of the contract: on x86_64 Tahoe the VideoToolbox paravirt **userspace
ships** and the IOKit integration hook is Apple-defined, but the **kernel
kext** that binds PCI `0x1a03106b` is arm64e-only. So there is no x86_64
guest consumer yet; lane C must write that kext. Consequently **nothing here
runs end-to-end**, and this increment is unit-tested in isolation, as the lead
scoped it.

## Architecture (the three modules)

| file | role | state |
|---|---|---|
| `vtpv_proto.{h,c}` | codec-session **protocol handler**: parse submission messages (QUERY_CAPS / SESSION_CREATE / DECODE_FRAME / SESSION_END), enforce the transport limits, dispatch to a decoder-backend vtable, emit completions | **complete + unit-tested** (`test_proto.c`, 13 cases) |
| `vtpv_vaapi.{h,c}` + `vtpv_h264.{h,c}` | **VA-API decode backend** over libva on a DRM render node: display open, capability enumeration (→ D-GVA-2), decode config/context/surface pool, **real H.264 decode** (SPS/PPS/slice parse → VA parameter buffers → `vaBeginPicture`/`RenderPicture`/`EndPicture`), surface readback | **complete + tested live** on `/dev/dri/renderD128`: `test_vaapi.c` (lifecycle + caps) and `test_h264.c` (a minimal I\_PCM I-frame decoded and its luma verified byte-exact, 0/101376 mismatches on radeonsi) |
| `vtpv_virtq.{h,c}` | **virtio 1.x split-virtqueue mechanics**: desc table / available ring / used ring walk, chain gather (readable) + scatter (writable), retire, over a caller guest-memory map callback | **complete + unit-tested** (`test_virtq.c`): a synthetic ring drives a full submission -> handler -> completion round-trip, plus malformed-chain refusal |
| `apple-videotoolbox.c` | **qdp plugin**: presents PCI `106b:1a03` as a virtio-1.x device; BAR0 **common-config register file** (feature negotiation, queue select/size/enable, desc/avail/used programming), ISR, notify, MSI-X; tracks guest **DMA regions** (`dma_register`/`unregister`); on a submission-queue notify drains the ring into the protocol handler and emits completions onto the completion queue with the per-queue MSI-X | **complete + loadable** (`test_plugin.c` ABI smoke); E2E (guest kext + real decode) is the separate guest session's |

### The wire (D-GVA-2)

The host→guest **video-capability advertisement** (decision D-GVA-2) is a new
device-feature report, **not** a METAL_FEATURES bit (video is a separate
device). It lives in the completion of `VTPV_MSG_QUERY_CAPS` as a `vtpv_caps`
record: per codec, decode/encode flags, max dimensions, output pixel format.
`vtpv_vaapi` fills it from the live driver (here: H.264 + HEVC decode).

### Surface handoff

`DECODE_FRAME` completes with a `vtpv_decoded_desc` (surface id + dimensions +
`NV12`). That is the hand-off point to the GPU path; QMetal owns the
decoded-surface ⇄ GPU sharing (IOSurface / DMA-BUF) in a later increment. The
backend owns the surface and names it by id; `vtpv_vaapi_read_luma` is the
test's readback of it.

## Scope boundary — what is NOT in increment 1 (tracked)

1. **The virtio ring state machine** — avail/used rings, descriptor-chain walk,
   feature negotiation beyond VERSION_1. The BAR register map and queue
   semantics are laid out in `apple-videotoolbox.c`; walking a live guest's
   rings is guest-blocked (no guest consumer). The message protocol the rings
   carry is already complete and tested.
2. **HEVC per-frame decode** — H.264 decode is real and pixel-verified (above).
   HEVC's session lifecycle + surface hand-off are wired, but the HEVC
   access-unit → VA-buffer parse (`vtpv_h264.c`'s HEVC sibling) is the next
   codec increment.
3. **Encode** (this GPU exposes `VAEntrypointEncSlice` for H.264/HEVC),
   **NVDEC/NVENC** and **VideoToolbox** backends, the **M2 scaler** device
   (`106b:1a0b`), and the **conformance** suite — all later increments.

## Decisions

- **D-GVA-1 (reuse vs build):** the contract recommended adopting reims's
  device+backends. The lead directed **clean-room**, so this is built fresh
  from the contract — no Rust video stack, no GPL dependency, one C plugin in
  qdev-host's own model.
- **D-GVA-2 (capability advertisement):** see above — `vtpv_caps` in the
  QUERY_CAPS completion.

## PCI identity and interrupt line (plugin args, 2026-11-28)

| Arg | Default | Meaning |
|---|---|---|
| `vendor-id=`, `device-id=` | `0x1b36`, `0xeee2` | PCI identity (subsystem the same). The x86 eee1 guest kext matches 0xEEE21B36. The stock **arm64e** `AppleVideoToolboxParavirtualization` (25G83) binds Apple's `AppleVirtIOPCITransport` on `IOPCIPrimaryMatch 0x1a03106b`, so arm64 guests take `vendor-id=0x106b,device-id=0x1a03` |
| `irq=` | `msix` | `msix`: MSI-X, 3 vectors (unchanged). `intx`: INTA# only, no MSI-X capability. `auto`: both; MSI-X once the guest enables it, INTx otherwise. INTx is virtio's: the ISR status bit (0x1 queue, 0x2 config) plus the level line of `../uniq-vgpu/pvg-intx.h`. The driver's ISR read (read-to-clear, BAR0 0x1000) is the acknowledgement |

Test: `build/test-vt-identity` (in `make test`). The real `.so` runs on a real libvfio-user context with an
in-process vfio-user client. The test checks identity and capabilities per mode, the eventfd types each mode accepts,
the ISR reads and arg refusals; it needs no GPU. INTx assert/deassert itself is the shared state machine,
`make uniq-vgpu-intx-test`. A video INTx raise end to end (through the virtqueues) is not covered yet: the E1-4 runtime
check.

## Build & test

Host dependency: `libva-dev` (VA-API headers; the runtime `libva2` +
`radeonsi_drv_video.so` were already present). Installed 2.22.0-3 to match.

```
# module unit tests (no VM): protocol handler, VA-API backend (live), H.264
# I_PCM decode (live, pixel-verified), plugin ABI smoke
make -C plugins/apple-videotoolbox test        # or: make apple-videotoolbox-test

# build the plugin into the qdev-host tree (needs the vendored libvfio-user)
make VIDEOTOOLBOX=1
```

The default `make` does not build this plugin (gated on `VIDEOTOOLBOX=1`), so
other lanes' qdev-host builds are unaffected.
