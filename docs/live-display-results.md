# Live display and input forwarding: measurements (2026-11-07)

Host: laptop, Ubuntu 24.04, kernel 6.8, Intel Raptor Lake-P with Iris Xe (`/dev/dri/card0`,
panel `eDP-1` 3840x2400@60, Samsung ATNA60YV04), 20 CPUs, loaded with other VMs
(load average ~6). Stock QEMU v11.1.3 built from the upstream tag, libvfio-user master,
guest Debian 13 (kernel 6.12, `bochs-drm`), KVM, 2 vCPUs. The display manager (sddm
greeter, Xorg) was stopped for the test so `qdev-host` could be DRM master; restarted after.

## Setup

```text
guest (bochs-drm / raw BAR writes) -> VRAM memfd in qdev-host -> damage detection (poll)
   -> KMS sink: copy damage into a dumb buffer, page flip at vblank -> eDP-1 panel
   -> Display1 p2p (QSM Direct probe)      [not active during the KMS load runs]
QSM-style input: qemu-display-probe --input-smoke -> qdev-host Console_0 -> VM's QEMU
   Display1 Console_0 (private dbus-daemon, destination org.qemu) -> usb-kbd / usb-tablet
```

Commands: `tools/remote-kms-test.sh` (full flow) and the paced runs with
`tools/guest-animate.py <pci> <frames> bar|full <fps> raw`.

## What worked

| Step | Result |
|---|---|
| KMS sink opens card0/eDP-1 | DRM master taken, 1 mode (3840x2400@60), EDID 256 bytes exported to the plugin |
| Guest sees the real panel | `bochs-drm` reads the EDID, `card1-Virtual-2` lists `3840x2400`, `fb1` created |
| Guest draws | frame visible in the scanned-out buffer (`results/kms-static-screen.png`), i.e. on the panel |
| `device_del` / `device_add` | screen blanks, device state kept, picture returns after replug |
| Input forwarding | probe `--input-smoke` events arrive in the guest on the USB keyboard (`event4`) and USB mouse (`event3`) |

## Pipeline throughput and latency at 3840x2400 (guest writes straight into VRAM)

Host poll 60 Hz, each run 10 s:

| Guest load | Guest fps | Damaged ticks | Flips/s | Detection→vblank avg / max | Copy per flip avg / max | Data copied | qdev-host CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| moving bar + counter | 60 | 601 / 654 | 55 | 14.8 / 21.3 ms | 1.8 / 7.5 ms | 7.8 GB | ~47 % of a core |
| full frame | 60 | 599 / 651 | 55 | 15.1 / 27.3 ms | 4.2 / 12.9 ms | 21.9 GB | ~57 % |
| full frame | 30 | 329 / 645 | 30 | 14.0 / 29.1 ms | 4.6 / 12.9 ms | 12.1 GB | ~46 % |
| moving bar | 90 | 600 / 651 | 55 | 13.6 / 17.7 ms | 1.5 / 11.4 ms | 8.3 GB | ~41 % |

- Every guest frame reaches the panel up to the poll rate (600 frames → 601 flips);
  at 90 fps the 60 Hz poll is the cap.
- Compare (memcmp of 36.9 MB against the shadow, plus shadow update): 5.3–6.7 ms avg,
  up to 24 ms under load.
- End-to-end (guest write → photons) adds the poll phase (avg 8 ms at 60 Hz) to
  detection→vblank: about 25–30 ms average, up to ~45 ms.
- "Data copied" is large for the bar case because damage is tracked as one bounding
  rectangle per tick and the test touches the top-left corner and the middle of the
  screen (worst case for a single rectangle).

Idle:

| Poll rate | Compare avg | CPU |
|---|---:|---:|
| 60 Hz (fixed) | 3.3–5.3 ms | 20–32 % |
| 30 Hz (fixed) | 4.8 ms | ~15 % |
| adaptive, 10 Hz after 2 s idle (`--idle-fps 10`) | 4.6 ms | ~7 % |

Wake-up from idle: the first frame of a new animation is detected within one idle
tick (≤100 ms); the following 120 frames at 60 fps produced 120 flips.

For comparison, a 1280x800 frame is 1/9 of the data, so all costs above divide by ~9.

## fbdev path (what a normal Linux console/fbcon does)

Through `/dev/fb1` the guest's writes go into the kernel's shadow framebuffer and are
flushed to the device by fbdev deferred I/O, so the device sees at most ~20 updates/s
regardless of the host: 169 damaged ticks for 600 guest frames, 15.5 flips/s. DRM/KMS
clients (a compositor, `modetest`) write to the device directly and are not limited
this way.

## Input forwarding latency

`tools/display1-input-bench` (`Keyboard.Press`/`Release` round trips, 2000 calls,
GDBus client on the same host):

| Path | avg | median | p99 | max |
|---|---:|---:|---:|---:|
| directly to the VM's QEMU (`-display dbus`, message bus) | 208 µs | 210 µs | 445 µs | 660 µs |
| to `qdev-host` (p2p), which forwards asynchronously | 162 µs | 154 µs | 442 µs | 1.1 ms |

The client sees `qdev-host`'s immediate completion; the delivery to the VM adds one
more D-Bus call of the same order (~0.2 ms), so an event reaches the guest ~0.4 ms
after the client sent it. Verified in the guest with `od` on the USB input devices.

## Overheads of the whole scheme (where the time goes)

| Component | Cost | Notes |
|---|---|---|
| guest → VRAM | 0 | mmap-ed BAR, no exits (measured earlier: a mmap-ed BAR access is a plain memory access) |
| damage detection | memcmp of the frame per poll tick | 4K: 3–7 ms per tick; 1280x800: ~0.5 ms; adaptive polling makes idle cheap |
| copy into the KMS dumb buffer | 2 GB/s into write-combined memory | 4K full frame 4–13 ms; small rectangles 1–2 ms |
| page flip | one vblank | avg half a frame of wait, 16.7 ms at 60 Hz |
| Display1 export (QSM) | `ScanoutMap` once + `UpdateMap` per damage | zero-copy (shared memfd); not measured under load here |
| input hop | +1 D-Bus call | ~0.2 ms |
| vfio-user register traffic | a few DISPI writes per mode set | negligible (23 µs each, posted writes disabled by the mmap BAR) |

## Pitfalls found

- **VRAM must be at least twice the frame**: the DRM VRAM helper validates modes with
  `2 × width × height × 4 ≤ VRAM` (double buffering). 3840x2400 needs 73.7 MB, so
  `vram=128` (BAR sizes are powers of two). With 64 MiB the driver logs
  `Cannot find any crtc or sizes`; with 32 MiB `less than 4 MB video memory`.
- **Root port window**: a 128 MiB prefetchable BAR needs `pref64-reserve=256M`
  (`64M` fails with `BAR 0: can't assign; no space` even for a 64 MiB BAR).
- **`CONFIG_IO_STRICT_DEVMEM`**: `resource0` of a device bound to a driver cannot be
  mmap-ed from user space (EINVAL); the raw animator detaches `bochs-drm` first and
  programs DISPI itself.
- **DRM master**: `drmSetMaster` fails with `EPERM` while an X server / compositor
  holds the card; stop the display manager or use a card without one.
- The guest's connector of the hot-plugged device is `card1-Virtual-2` when the boot
  VGA already owns `Virtual-1`.
- A test script must never kill processes by name on a shared host; the scripts here
  track their own PIDs.

## Next steps for the display path

1. Damage from the plugin instead of polling: a GPU-style plugin (virtio-gpu
   transfers, dma-buf scanouts) knows what changed; polling then disappears.
2. Zero-copy scan-out of the memfd (udmabuf → `drmPrimeFDToHandle`) where the KMS
   driver can scan out system memory; removes the 2 GB/s copy.
3. Multiple damage rectangles per tick instead of one bounding box.
4. Per-head input device for multi-head guests (pointer lands on the external head).

## Round 2 (same evening): zero-copy KMS, second-head pointer, Clipboard/Audio relays

Same host, same guest, `tools/remote-features-test.sh`.

### Zero-copy scan-out through udmabuf

The plugin's VRAM memfd (sealed with `F_SEAL_SHRINK`) is wrapped into a dma-buf by
`/dev/udmabuf`, imported with `drmPrimeFDToHandle`, turned into a framebuffer with
`drmModeAddFB2` and set on the CRTC. The panel then reads the guest's pixels directly:

```text
kms: zero-copy: eDP-1 scans out the device memfd directly: CRTC fb 245 (ours 245), 3840x2400 pitch 15360 offset 0
```

| Guest load at 3840x2400 (raw VRAM writes) | Copy path (round 1) | Zero-copy |
|---|---:|---:|
| 600 frames, moving bar, 60 fps | 55 flips/s, 7.8 GB copied, ~47 % of a core | 0 copies, 0 polls, process CPU 0.05 s over the run (≈ 0 %) |
| 600 frames, full frame, 60 fps | 55 flips/s, 21.9 GB copied, ~57 % | 0 copies, 0 polls, ≈ 0 % |
| latency guest write → panel | poll + compare + copy + vblank ≈ 25–30 ms | next scan-out of the panel (≤ 1 frame, 16.7 ms at 60 Hz) |

With zero-copy and no Display1 listener attached, the host does not poll for damage
at all; when a listener (QSM) connects, polling resumes for it only. Tearing is
possible by construction (single buffer, like a real dumb framebuffer); a guest
driver that double-buffers (DRM clients) avoids it.

Pitfalls: udmabuf refuses buffers above `size_limit_mb` (64 MB by default,
`/sys/module/udmabuf/parameters/size_limit_mb`), so the dma-buf covers only the
scanout region (page aligned), not the whole VRAM; `/dev/udmabuf` must be
accessible (group `kvm` on Ubuntu); the mode must match the guest frame exactly
(otherwise the copy path with centering is used); i915 accepted the imported
system-memory buffer for scan-out, other drivers may not (fallback is automatic).

### Pointer on the second head

`--head-origin`/`--desktop` (default: this display to the right of the VM console).
Sent `SetAbsPosition(1920, 1200)` for the 3840x2400 head with the VM console at
640x480: expected guest tablet values `x' = (640+1920)·640/4480 = 365 →
ABS_X = 365·32767/640 = 18687`, `y' = 1200·480/2400 = 240 → ABS_Y = 16383`;
the guest's USB tablet reported `ABS_X=18687 ABS_Y=16383`. (QEMU scales by the
console size, not size−1.) Whether the guest desktop really places the external
head there is the guest's configuration; the options exist for other layouts.

### Clipboard relay

The exported `/org/qemu/Display1/Clipboard` relays `Register/Unregister/Grab/
Release/Request` to the VM's QEMU, and QEMU's callbacks (`Grab/Release/Request`,
and the `Register/Release` QEMU itself issues to its peer) to the registered
client. All relays are asynchronous: QEMU answers a `Register` with a property
fetch on the caller's object, so a synchronous relay deadlocks (observed, 5 s
timeout, fixed). `dbus-monitor` on the VM bus during the test client's run:

```text
:1.5 (qdev-host) -> :1.0 (QEMU)  Register
:1.0 -> :1.5                     Register        (QEMU registering with its peer)
:1.5 -> :1.0                     Grab
:1.5 -> :1.0                     Release
:1.0 -> :1.5                     Release         (relayed to the client)
:1.5 -> :1.0                     Unregister
```

After our client unregistered, a second peer registering directly at QEMU was
accepted; while ours is registered QEMU refuses others (one clipboard peer per
QEMU), which is why one QSM worker per VM must go through the relay, not beside it.
Guest-side clipboard traffic (`Request` from the guest agent) was not exercised
(no agent in the test guest).

### Audio relay

`Audio.RegisterOutListener/RegisterInListener` pass the client's listener fd to the
VM's QEMU (`audio listener relayed to the VM`), and `VM.Interfaces` advertises
`org.qemu.Display1.Audio`. The end-to-end check with `qemu-display-probe
--require-audio` failed with the same error with and without `qdev-host`, directly
against this QEMU: `Failed to setup peer connection: Unexpected lack of content
trying to read a byte` on QEMU's side, i.e. the probe did not serve its listener
socket for QEMU's handshake. So the relay is transparent, but audio was not
verified end to end here; it needs the real QSM worker (or a fixed probe).
