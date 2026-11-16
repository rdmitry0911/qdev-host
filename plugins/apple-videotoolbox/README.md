# apple-videotoolbox — AppleGVA host backend (increment 1)

Host side of Apple's paravirtualized **VideoToolbox** codec device
(PCI `106b:1a03`), as a qdev-host plugin. Lane L-M2V03, 2026-11-16.

This is **increment 1** of a multi-increment effort. It moves AppleGVA from
*no implementation* to **host device skeleton + VA-API decode backend +
unit tests**, all buildable and tested without a VM.

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
| `vtpv_vaapi.{h,c}` | **VA-API decode backend** over libva on a DRM render node: display open, capability enumeration (→ D-GVA-2), decode config/context/surface pool, surface readback | **complete + tested live** on `/dev/dri/renderD128` (`test_vaapi.c`); the per-frame bitstream decode is the next stage (below) |
| `apple-videotoolbox.c` | **qdp plugin**: presents PCI `106b:1a03` as a virtio-1.x device, lays out BAR0 (common/ISR/notify/device cfg), MSI-X (per-queue + config), the two virtqueues (0 submission, 1 completion), and instantiates the handler over the backend | **compiles + registers + loadable** (`test_plugin.c` ABI smoke); the virtio ring state machine is the next increment |

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
2. **The per-frame bitstream decode** — `vtpv_vaapi`'s `decode` has the VA
   object lifecycle and the surface hand-off wired and returns a real
   render-target surface, but the H.264/HEVC access-unit → VA-buffer mapping
   (`vaBeginPicture`/`vaRenderPicture`/`vaEndPicture` over a parse) is the next
   stage; until then the surface content is undefined. (A minimal I\_PCM
   I-frame fixture is the intended first decode-verify; radeonsi I\_PCM support
   to be confirmed.)
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

## Build & test

Host dependency: `libva-dev` (VA-API headers; the runtime `libva2` +
`radeonsi_drv_video.so` were already present). Installed 2.22.0-3 to match.

```
# module unit tests (no VM): protocol handler, VA-API backend (live), plugin ABI
make -C plugins/apple-videotoolbox test        # or: make apple-videotoolbox-test

# build the plugin into the qdev-host tree (needs the vendored libvfio-user)
make VIDEOTOOLBOX=1
```

The default `make` does not build this plugin (gated on `VIDEOTOOLBOX=1`), so
other lanes' qdev-host builds are unaffected.
