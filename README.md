# qdev-host

**Out-of-process PCI devices for a stock QEMU, with the device's display exported
over QEMU's own D-Bus display protocol (`org.qemu.Display1`).**

`qdev-host` runs a device model in its own process and plugs it into a running,
unmodified QEMU through `vfio-user-pci` (QEMU ≥ 10.1; Proxmox VE 9.2 ships 11.0, verified with pve-qemu-kvm 11.0.3).
Device models are plugins (`.so`) with a small, versioned ABI, so new devices can
be added, removed and replaced while the VM keeps running, and no QEMU fork or
patch has to follow QEMU releases. The first plugin, `tb-vga`, is the
hot-pluggable display endpoint of the
[qemu-thunderbolt](https://github.com/rdmitry0911/qemu-thunderbolt) fork, moved
out of QEMU.

The display of such a device does not live inside QEMU, so `qdev-host` exports it
itself, speaking exactly what a QEMU started with `-display dbus` speaks: the
`org.qemu.Display1` interfaces, over a peer-to-peer socket and/or a message bus.
[QSM Direct](https://github.com/rdmitry0911/qsm-rd) (WebRTC console for Proxmox)
and its `qemu-display-probe` attach to it unchanged.

Project sponsor: [Takt](https://taktcycles.com/).

## Contents

- [Architecture](#architecture)
- [Status](#status)
- [Requirements](#requirements)
- [Build](#build)
- [Run](#run)
- [Display export (QSM Direct)](#display-export-qsm-direct)
- [Input forwarding](#input-forwarding)
- [Control socket (automation)](#control-socket-automation)
- [Proxmox VE](#proxmox-ve)
- [Plugin ABI](#plugin-abi) (versions 1 and 2, tests without a VM)
- [The tb-vga plugin](#the-tb-vga-plugin)
- [Thunderbolt hotplug layer on stock QEMU](#thunderbolt-hotplug-layer-on-stock-qemu)
- [Performance](#performance)
- [Roadmap](#roadmap)
- [Repository layout](#repository-layout)
- [License](#license)

## Architecture

```text
 guest                 stock QEMU                    qdev-host (one process per device)
 ┌───────────┐   ┌──────────────────────┐    unix     ┌───────────────────────────────────┐
 │ driver    │   │ pcie-root-port tbrp0 │   socket    │ libvfio-user ctx                  │
 │ (bochs-   │◄─►│  └ vfio-user-pci ────┼────────────►│  └ plugin.so (qdp.h ABI): tb-vga  │
 │  drm, …)  │   │     BAR0 = mmap of   │             │       BAR0 VRAM = memfd           │
 └───────────┘   │     the plugin memfd │             │       BAR2 regs via messages      │
                 └──────────┬───────────┘             │                                   │
                            │ QMP device_add/del      │ scanout (memfd, mode)             │
                      tools/tbctl.py                  │   └ damage: shadow diff or plugin │
                                                      │       ├ sink: org.qemu.Display1   │
                                                      │       │   p2p socket / message bus│
                                                      │       ├ sink: PPM (SIGUSR1, ctl)  │
                                                      │       └ sink: DRM/KMS             │
                                                      └─────────────┬─────────────────────┘
                                                                    │ Display1 (same as -display dbus)
                                                      QSM Direct worker / qemu-display-probe
```

Key properties:

- **Stock QEMU.** Nothing is patched. The guest sees a normal PCIe endpoint behind
  a normal `pcie-root-port`; `device_add` / `device_del` hot-plug it.
- **State lives in `qdev-host`.** `device_del` disconnects the socket; the process,
  its VRAM and registers stay. `device_add` reattaches, even after a QEMU restart.
  This is the Thunderbolt `connected=false/true` toggle of the fork, done with
  stock commands.
- **Guest memory access without exits.** A plugin region backed by a memfd is
  mmap-ed by QEMU into the guest (KVM memslot): guest writes to the framebuffer
  never leave the guest. Display listeners get the same memfd (`ScanoutMap`), so a
  frame travels guest → VRAM → encoder with zero copies.
- **Plugins are isolated.** A plugin links against libvfio-user only; the host ABI
  is one header, `qdp.h`.

## Status

Verified on 2026-11-07 with stock QEMU v11.1.3 (built from the upstream tag),
libvfio-user master, a Debian 13 guest (kernel 6.12) under KVM:

| Step | Result |
|---|---|
| `device_add vfio-user-pci` behind `pcie-root-port` | guest sees `01:00.0 VGA [1234:1111]`, BAR0 16 MiB 64-bit prefetchable (mmap), BAR2 4 KiB, PCIe endpoint 8 GT/s x4 |
| Stock Linux driver | `bochs-drm` binds unmodified, reads the EDID (1280x800), creates `/dev/fb1` |
| Guest draws a frame | pixels land in the `qdev-host` memfd; `SIGUSR1` dumps them; `qemu-display-probe` (QSM Direct) receives them via `ScanoutMap`/`UpdateMap` over p2p and over a private bus as `org.qemu` |
| `device_del` / `device_add` | `DEVICE_DELETED`, the guest drops `card1`; replug rebinds, VRAM kept |
| `disconnect` / `connect` of the whole port | works (`tools/tbctl.py`) |
| ACPI `_DSD` of the fork (`PCI-Thunderbolt`, `pci-supports-link-change`, `IOPCITunnelled`) | delivered by an SSDT through `-acpitable`, visible in the guest |

Live display and input, verified on a laptop with an Intel Iris Xe and a 3840x2400
panel ([measurements](docs/live-display-results.md)):

| Step | Result |
|---|---|
| `--kms /dev/dri/card0:eDP-1` | `qdev-host` is DRM master; the panel's EDID goes to the guest, which picks 3840x2400 |
| Guest frames on the panel | every guest frame reaches the screen up to the poll rate (60 fps → 55 flips/s), detection→vblank ≈ 15 ms, ≈ 50 % of one core at 4K under load, ≈ 7 % idle with adaptive polling |
| Input | QSM-style keyboard/mouse events relayed to the VM's own QEMU and seen on the guest's USB devices; one extra D-Bus hop (~0.2 ms); second-head pointer mapping verified numerically |
| Zero-copy KMS | the panel scans the device memfd out directly (udmabuf); 60 fps at 4K with no copies and no polling |
| Clipboard / audio | relayed to the VM's QEMU; clipboard verified with `dbus-monitor`, audio relay accepted by QEMU (end-to-end audio needs the real QSM worker) |

Not done yet: guest-agent clipboard traffic and end-to-end audio with the real QSM
worker, macOS validation, several devices per host process.

## Requirements

- Linux host with KVM.
- QEMU ≥ 10.1 with the `vfio-user-pci` device (`qemu-system-x86_64 -device vfio-user-pci,help`).
  Proxmox VE 9.2: `pve-qemu-kvm` 11.0.3 has it. Ubuntu 24.04's QEMU 8.2 does not.
  Plugins that do DMA from their own threads (ABI v2, GPUs) need QEMU ≥ 11.0: the
  10.x client's `VFIO_USER_DMA_READ/WRITE` messages use a 32-bit count where the
  protocol has 64 bits (fixed upstream in e64eb57443).
- [libvfio-user](https://github.com/nutanix/libvfio-user): a patched copy is vendored
  in `third_party/libvfio-user` and built by `make` (meson, ninja, json-c and cmocka
  headers: `meson ninja-build libjson-c-dev libcmocka-dev`).
- GLib/GIO ≥ 2.64 with `gdbus-codegen` (`libglib2.0-dev`, `libglib2.0-dev-bin`).
- For the tests: `debugfs` (e2fsprogs), Python 3, optionally `iasl` to rebuild the
  SSDT and `qemu-display-probe` from the qsm-rd repository.

## Build

```bash
make                          # builds third_party/libvfio-user into build/vfu, then the host,
                              # plugins and tools (rpath to build/vfu/lib)
make vfu                      # only the vendored libvfio-user
make VFU_PREFIX=/opt/vfu      # use a libvfio-user installed elsewhere (it must carry the patches below)
make install PREFIX=/usr/local
make distclean                # also removes build/
```

The vendored libvfio-user (`VENDORED_COMMIT`) carries three patches the host and v2
plugins rely on; an upstream build does not compile with the host (`#error`):

- `vfu_dma_info_t` has `int fd` and `uint64_t fd_offset` (appended): the region's
  backing file as the client passed it (-1 for message-based regions) and the file
  offset of `iova`. It is the fd cache's de-duplicated descriptor, shared by regions
  of the same file, closed after the `dma_unregister` callback of the last of them.
- `vfu_irq_trigger()` is thread-safe: a mutex serialises the eventfd lookup and
  write against `SET_IRQS` and context reset; it is never held across a callback.
- On a lost connection the reset callback (`VFU_RESET_LOST_CONN`) runs before the
  DMA regions are removed (upstream removed them first, although its documentation
  says the context is cleaned up after the callback).

`dbus/dbus-display1.xml` is QEMU's interface definition (Linux variant); the GDBus
glue is generated at build time with `gdbus-codegen`.

## Run

### 1. Start the device host

```bash
qdev-host --plugin plugins/tb-vga/tb-vga.so --socket /run/qdev/tbvga0.sock \
          --plugin-args edid=plugins/tb-vga/edid-1280x800.bin,vram=16 \
          --display-p2p /run/qdev/tbvga0.display \
          --display-bus unix:path=/run/qdev/tbvga0.bus --display-name org.qemu \
          --dump-dir /tmp --fps 30 -v
```

| Option | Meaning |
|---|---|
| `--plugin FILE.so` | device model |
| `--socket PATH` | vfio-user socket QEMU connects to (unix socket paths are limited to 107 bytes) |
| `--plugin-args STR` | plugin options, see the plugin's README |
| `--display-p2p PATH` | `org.qemu.Display1` peer-to-peer server socket; each client gets its own object tree |
| `--display-bus ADDR` | join a message bus (`unix:path=…`) and own `--display-name` (default `org.qemu`, what QSM Direct expects) |
| `--vm-name NAME` | `org.qemu.Display1.VM.Name` |
| `--dump-dir DIR` | `SIGUSR1` writes `qdev-fb-N.ppm` there |
| `--fps N` | how often the framebuffer is compared with its shadow to find damage (default 30) |
| `--idle-fps N`, `--idle-after S` | drop to N Hz after S seconds without changes (default 10 Hz after 2 s; 0 = off) |
| `--kms /dev/dri/cardN[:CONNECTOR]` | scan the device out on a real monitor (see below) |
| `--on-connect CMD`, `--on-disconnect CMD` | run when that connector's status changes (e.g. `tbctl.py … plug/unplug`) |
| `--qemu-display ADDR`, `--qemu-display-dest NAME`, `--qemu-console N` | forward keyboard/mouse to the VM's own Display1 console (see below) |
| `--qemu-qmp PATH` | forward keyboard/mouse with QMP `input-send-event`, for VMs without a graphic console (see below) |
| `--control PATH` | control socket for automation: `screendump FILE`, `frame`, `stats`, `uiinfo W H [WMM HMM]` (see below) |
| `--stats-interval S` | print damage-detection and KMS statistics every S seconds (`SIGUSR2` prints once) |
| `--kms-copy` | force the dumb-buffer copy path instead of zero-copy |
| `--head-origin X,Y`, `--desktop WxH` | guest desktop layout for the pointer on this head (see below) |

### 2. Start the VM on a stock QEMU

What the VM needs (see `tools/run-guest.sh` for a complete example):

```bash
qemu-system-x86_64 -machine q35,accel=kvm \
  -object memory-backend-memfd,id=m0,size=2G,share=on -machine memory-backend=m0 \
  -device pcie-root-port,id=tbrp0,bus=pcie.0,chassis=1,slot=1,addr=0x05,pref64-reserve=64M,mem-reserve=8M \
  -acpitable file=acpi/ssdt-tb.aml \
  -qmp unix:/run/qdev/qmp0.sock,server,nowait ...
```

- `pcie-root-port` with **reserved windows**: a hot-plugged 16 MiB prefetchable BAR
  needs `pref64-reserve`; without it Linux logs `BAR 0: can't assign; no space`.
- **Shared guest memory** (`memory-backend-memfd,share=on`): required for any
  plugin that does DMA (the client only hands file descriptors of shared memory to
  the device process; otherwise every DMA access becomes a socket message, 20×
  slower, see [Performance](#performance)). `tb-vga` has no DMA but the setting is
  harmless.
- The SSDT is only needed for the Thunderbolt ACPI properties (macOS).

### 3. Hot-plug, draw, unplug

```bash
tools/tbctl.py /run/qdev/qmp0.sock plug tbvga0 /run/qdev/tbvga0.sock tbrp0
tools/tbctl.py /run/qdev/qmp0.sock pci                     # list devices (behind which bridge)
tools/tbctl.py /run/qdev/qmp0.sock unplug tbvga0           # device_del, waits for DEVICE_DELETED
tools/tbctl.py /run/qdev/qmp0.sock disconnect tbrp0        # = Thunderbolt connected=false
tools/tbctl.py /run/qdev/qmp0.sock connect tbrp0 tbvga0=/run/qdev/tbvga0.sock
```

`tools/test-flow.sh` runs the whole sequence against a prepared Debian guest
(`tools/prep-guest.sh` makes one without root rights on the host: it extracts the
kernel and initrd with `debugfs` and injects a serial autologin).

## Display export (QSM Direct)

`qdev-host` implements the server side of `org.qemu.Display1` the way QEMU does
(`ui/dbus*.c`), with the same object paths:

| Object | Interfaces | Notes |
|---|---|---|
| `/org/qemu/Display1/VM` | `VM` | `Name`, `UUID`, `ConsoleIDs = [0]` |
| `/org/qemu/Display1/Console_0` | `Console`, `Keyboard`, `Mouse` | `Type = Graphic`, `Width/Height` follow the guest's mode; `RegisterListener(fd)` opens a p2p connection to the client's `Listener` object |
| client's `/org/qemu/Display1/Listener` | `Listener`, `Listener.Unix.Map` | `ScanoutMap(memfd, offset, w, h, stride, pixman)` + `UpdateMap(x, y, w, h)` when the listener advertises `Unix.Map` (QSM Direct does), otherwise inline `Scanout` / `Update`; `Disable` when the guest turns the output off; `CursorDefine` + `MouseSet` for plugins with a cursor plane (v2) |

Two transports, usable at the same time:

- **p2p** (`--display-p2p`): a listening unix socket; a client connects with no
  bus in between (`qemu-display-probe --dbus-address unix:path=…`, QSM worker
  with an inherited fd).
- **message bus** (`--display-bus`): the per-VM private `dbus-daemon` model that
  QSM Direct's standalone profile uses (`/run/qsm-pve-direct/<vmid>/qemu-display1.bus`,
  destination `org.qemu`). For an external device the terminal service would run
  one such bus per external console, or pass a p2p fd; nothing in the worker changes.

Damage detection: the host keeps a shadow copy of the scanout and compares it
`--fps` times per second (per row, then per pixel at the edges), sending one
bounding rectangle per tick. Guest writes to the mmap-ed VRAM are invisible to the
host, so this is the only generic option. A v2 plugin that knows its frames (a
GPU) sets `QDP_F_PLUGIN_DAMAGE` and calls `frame_ready()`: no polling, no shadow;
the host re-reads `get_scanout()` and sends the plugin's damage rectangle (united
over the frames announced since the last batch) to every sink.

Cursor (v2 `get_cursor()`): after `cursor_changed()` the host sends `CursorDefine`
when the image serial changes and `MouseSet(x + hot_x, y + hot_y, visible)` when
anything changes; a listener that registers later gets the current cursor. The
cursor is not drawn into PPM dumps or the KMS picture (as with QEMU's own
`screendump`).

Verified with QSM Direct's `qemu-display-probe` in both transports:
`scanout inline/map: 0/1`, `updates inline/map: 0/N`, `session errors: 0`, and the
PPM snapshot equals the frame drawn by the guest.

## Real monitor as the output of the guest device (DRM/KMS sink)

`--kms /dev/dri/card0:eDP-1` makes `qdev-host` the DRM master of that card and
drives the connector directly (`kms-sink.c`):

- The connector's **EDID is handed to the plugin** (`qdp_host_env.edid`), so the
  guest sees the real monitor and picks a mode it really has; `tb-vga` serves it
  unless an `edid=` file overrides it.
- When the guest sets a mode, the sink picks the connector mode of the same
  size (highest refresh, preferred first), else keeps the preferred mode and
  centers the guest frame in it.
- **Zero-copy by default**: the plugin's memfd is wrapped into a dma-buf with
  `/dev/udmabuf`, imported into DRM and set on the CRTC, so the monitor reads the
  guest's pixels directly; no polling, no copies, latency = the next scan-out.
  Needs a sealed memfd (`F_SEAL_SHRINK`, `tb-vga` does this), a mode of exactly
  the guest size, `/dev/udmabuf` access (group `kvm`) and a KMS driver that scans
  out imported system memory (i915 verified). Otherwise, or with `--kms-copy`:
- Two dumb buffers of mode size; each damage rectangle found by the host is
  copied into the back buffer, which is **page-flipped at vblank**
  (`DRM_MODE_PAGE_FLIP_EVENT`); damage arriving during a flip is accumulated per
  buffer and flipped next.
- `SIGUSR2` / `--stats-interval` print flips per second, damage→vblank latency,
  copy cost and process CPU time. On exit the previous CRTC configuration is
  restored and the master dropped.
- `--on-connect` / `--on-disconnect` watch the connector's sysfs status once a
  second and run a command, so plugging a monitor can `device_add` the device
  into the VM and unplugging can `device_del` it: the Thunderbolt story end to
  end. (Hooks are implemented, not yet exercised with a physically hot-plugged
  monitor.)

Requirements: no other DRM master on the card (stop the display manager, or use
a card without a desktop on it); `/dev/dri/cardN` writable (group `video`).
udmabuf limits one buffer to `size_limit_mb` (64 MB by default), so only the
scanout region is wrapped, not the whole VRAM.

## Input forwarding

QSM Direct sends keyboard and pointer events to the console it captures. For an
external device the input devices still live in the VM's own QEMU, so
`qdev-host` relays `Keyboard.Press/Release` and `Mouse.Press/Release/RelMotion/
SetAbsPosition` of its exported console to the VM's Display1 console
(`--qemu-display unix:path=… --qemu-display-dest org.qemu --qemu-console 0`;
empty destination = p2p). Absolute positions are rescaled from this display's
size to the VM console's size, `Mouse.IsAbsolute` mirrors the VM's. Calls are
forwarded asynchronously; the extra hop costs one D-Bus method call
(`tools/display1-input-bench` measures it: direct vs. through `qdev-host`).

Device as the only display (`--qemu-qmp PATH`): a VM whose only display is the
external device (e.g. a paravirtual GPU with its own option ROM) has no graphic
console in QEMU, so there is no Display1 console to relay to. `qdev-host` then
sends the events to the VM's QMP socket as `input-send-event` (no `device`/`head`:
QEMU's default handlers, e.g. `usb-kbd` and `usb-tablet`). Display1 key codes are
QEMU key numbers, the same encoding as a QMP key of type `number`; buttons map to
`InputButton` names; absolute positions are scaled to 0..0x7fff over this display
(or the `--desktop` layout). `Mouse.IsAbsolute` is true when QEMU has an absolute
mouse (`query-mice`). The QMP connection is asynchronous and retried every
second, so `qdev-host` can (and for a cold-plugged device must) start before QEMU,
and it reconnects after a QEMU restart. QMP accepts one client per monitor: give
QEMU a second `-qmp` for other tools. `--qemu-display` wins if both are given.

Second head: the VM's tablet spans the whole guest desktop, so a point on this
display is mapped to desktop coordinates first (`--head-origin X,Y`, default: right
of the VM console; `--desktop WxH`, default: VM console + this display) and then
scaled to the VM console's size. Verified against the guest's tablet values
([measurements](docs/live-display-results.md)). The guest's own screen layout
must match these options (or the other way round).

Clipboard and audio: when the VM's QEMU exposes `org.qemu.Display1.Clipboard` and
`.Audio`, `qdev-host` exports the same objects and relays them, so a QSM worker
attached to the external display gets the VM's clipboard and audio as if it were
attached to QEMU: clipboard calls go both ways (QEMU's callbacks reach the client),
audio listener fds are handed to QEMU, which then talks to the client directly.
One clipboard peer per QEMU is a QEMU rule, which the relay respects.

## Control socket (automation)

QMP `screendump` cannot see an external device's display (QEMU has no console for
it), so test harnesses use `--control PATH`: a unix stream socket with one command
per line and one reply line per command, answered in order.

| Command | Reply |
|---|---|
| `screendump FILE` | waits for the next presented frame, at most 1 s, then writes the current scanout to `FILE` as binary PPM (`P6`, written as `FILE.tmp` and renamed) and replies `ok <seq> <w>x<h>` |
| `frame` | `ok <seq> <w>x<h> stride <bytes> format 0x<pixman>`, or `ok <seq> off` |
| `stats` | `ok key=value ...`: `seq`, `mode`, `frames`, `frame_calls`, `ticks`, `damaged`, `idle_s`, `irqs`, `irqs_dropped`, `dma_regions`, `dma_mib`, `dma_maps`, `dma_unmaps`, `quiesce`, `cursor_calls`, `cursor_sent`, `listeners`, `attached`, `cpu_s` |
| `uiinfo W H [WMM HMM]` | asks for a `W`x`H` display (pixels; optional physical size in mm), as a Display1 `SetUIInfo` does: handed to an ABI v3 plugin's `ui_info()`. Replies `ok queued WxH`, `ok unchanged WxH` (same as the last accepted request) or `err ...` (no `ui_info` in the plugin, or refused). The scanout, and `frame`, change only when the guest presents at the new size |

Errors are `err <reason>` (`err scanout off`, `err unknown command ...`). `<seq>`
is the plugin's frame sequence number for `QDP_F_PLUGIN_DAMAGE` plugins; for
polling plugins the host counts the ticks that found damage (a pending
`screendump` keeps the comparison running even with no other consumer). `FILE` is
relative to `qdev-host`'s working directory; give an absolute path. 32 bpp pixman
formats of type ARGB, ABGR, BGRA and RGBA are written; the cursor is not included.

```bash
printf 'screendump /tmp/shot.ppm\nframe\n' | socat - UNIX-CONNECT:/run/qdev/gpu0.ctl
tools/qdev-control.py /run/qdev/gpu0.ctl "screendump /tmp/shot.ppm" stats
```

## Proxmox VE

Two Debian packages, built with `scripts/mkchroot.sh` + `scripts/build-deb-in-chroot.sh`
(Debian 13 chroot) or any trixie box with the `Build-Depends`:

- `qdev-host` (amd64): the host, plugins, a private copy of libvfio-user
  (`/usr/lib/qdev-host/libvfio-user.so.0`), the ABI header, EDID and SSDT files.
- `qdev-host-pve` (all): `qdevctl`, a guest hookscript, systemd units, the
  Hardware-panel web UI overlay and its node service. Stock PVE 9 only: `qm` for
  config edits, the node's own `PVE::QemuServer::Monitor::mon_cmd` for
  `device_add`/`device_del`/`query-pci`, the standard `hookscript` mechanism,
  systemd for the device processes. No Proxmox code is patched; the web UI
  entry is an overlay descriptor for `pve-ui-overlays` (one script tag).

```bash
apt install ./pve-ui-overlays_0.1.0_all.deb ./qdev-host_0.5.0_amd64.deb ./qdev-host-pve_0.5.0_all.deb
qdevctl add 109 tbvga0 --plugin tb-vga --args vram=16     # q35 VM; adds hookscript + TB bus (args)
qm start 109                                               # hookscript starts the process and hot-plugs
qdevctl list                                               # VM 109  tbvga0  tb-vga  bus=qdev-tb0  attached (01:00.0)
qdevctl stop 109 tbvga0; qdevctl start 109                 # hot-unplug / hot-plug on the running VM
qdevctl remove 109 tbvga0 --purge-port                     # forget it, restore hookscript and args
```

**Web UI.** After the install the VM's *Hardware* panel lists qdev-host devices
as rows (`qdev-host (tbvga0)`: plugin, args, KMS output, live state such as
`attached (01:00.0)` or `defined, attaches after the next VM start`), *Add* has a
*qdev-host Device* entry (plugin, device ID, the plugin's arguments as form
fields, KMS output picked from the node's connectors, Display1 export,
persistent process), *Edit* (button or double click) changes a row's settings and
*Remove* hot-unplugs the selected row. It follows QSM
Direct's structural pattern:

- `qdev-host-hardware.js` is an ExtJS override of `PVE.qemu.HardwareView`
  served from `/pve2/js/`. It reaches the page through
  [pve-ui-overlays](https://github.com/rdmitry0911/pve-ui-overlays), the
  shared owner of PVE's `index.html.tpl` diversion: the package only ships the
  descriptor `/usr/share/pve-ui-overlays/overlays.d/qdev-host.json`, and
  `pve-ui-overlays` renders every registered overlay (QSM Direct's console
  entry, this one, ...) for the reviewed pve-manager versions, in any install
  order (`pve-ui-overlays status`).
- `qdev-host-api.service` (HTTPS on port 8017, the node's own certificate)
  executes the operations. Every request carries the browser's PVE ticket in an
  `Authorization` header; the service relays it to the node's `/access/ticket`
  with `path=/vms/VMID` and `privs=VM.Config.HWType`, and additionally requires
  the confirmed user to be in `/etc/qdev-host/ui.json` (`root@pam` by default,
  because the operations edit the VM's `args`, which PVE reserves to root).
  Without the gateway the browser must trust the certificate on port 8017
  once (the panel shows a link when the service does not answer).
- On a node behind QSM Direct's TCP 8006 gateway (exp230 or later) the
  package's route publishes the service as `https://NODE:8006/qdev-host/`, and
  the panel uses that same origin: a browser that reaches only port 8006 needs
  no TCP 8017. It goes to port 8017 only when pveproxy answers that path (a
  gateway without the route), not after a failed request
  ([details](pve/README.md)).

Details in [pve/README.md](pve/README.md). Verified on a PVE 9.2.21 node
(pve-qemu-kvm 11.0.3, qemu-server 9.2.11, qsm-pve-direct installed, in both
install orders): CLI and web UI add → attached → remove cycles, headless
browser test in `tools/pve-ui-test.mjs`. Again on 2026-11-08 on the same node
after its move to the official pve-manager 9.2.21 and to qsm-pve-direct
0.4.0+stock4.14~exp229, the first qsm version that is itself a pve-ui-overlays
client (its upgrade took over the template diversion qsm had held before):
both entries on the page, the full web UI cycle on a test VM passes. The device process is `qdev-host-device@VMID-ID.service`;
its Display1 socket (`/run/qdev-host/VMID-ID/display.sock`) is where a QSM Direct
worker attaches, and when QSM's per-VM bus exists the device relays input,
clipboard and audio to it automatically.

Built-in plugin `virgl-gpu`: a VirGL 3D GPU whose rendering runs in QEMU's own
`vhost-user-gpu` process on a render node of the node, on the VM's command line
from its next start, as the primary display or an additional GPU, and shown by the
QSM Direct console ([details](pve/README.md#virgl-3d-gpu-virgl-gpu)). From 0.5.4 a
preloaded library works around two faults of `vhost-user-gpu`: a crash when a virgl
fence comes during an EDID reply, and a GPU socket refused after a VM pause or a
reboot inside the guest.

**TB bus** (0.6.0): hot-plug PCIe root ports with the Thunderbolt ACPI
properties of the fork, generated per VM, for QEMU's own devices: PCI
pass-through, network cards, a USB controller and USB devices of the node on it,
added to and removed from the running VM. qdev-host's own devices (tb-vga,
virgl-gpu) take its ports as well. For a macOS guest the bus switches the VM to native
PCIe hot-plug and drops the ports' power controller, which is what macOS needs to see
devices arrive (0.6.1). PVE's own *PCI Device*, *Network
Device* and *USB Device* dialogs get a *Bus* field (*Standard* or *TB bus*)
([details](pve/README.md#tb-bus-hot-plug-ports-with-thunderbolt-properties)).

Not yet: VMs on another
cluster node than the one serving the browser, several devices per process, a
hookscript-free path for VMs whose firmware already reserves enough window space.

## Plugin ABI

`qdp.h`, versions 1 to 3 (`QDP_ABI_VERSION_MIN` .. `QDP_ABI_VERSION`). A plugin
is a shared object exporting one symbol (the name stays `qdp_plugin_v1` for every
version):

```c
const qdp_device_ops *qdp_plugin_v1(void);

typedef struct qdp_device_ops {
    uint32_t    abi_version;              /* QDP_ABI_VERSION the plugin was built for */
    const char *name, *version;
    int  (*setup)(vfu_ctx_t *ctx, const char *args,
                  const qdp_host_env *env, void **priv);            /* configure the device on ctx */
    void (*teardown)(void *priv);
    int  (*reset)(void *priv, vfu_reset_type_t type);               /* client reset or lost connection */
    uint32_t tick_ms; void (*tick)(void *priv);                     /* optional periodic work */
    bool (*get_scanout)(void *priv, qdp_scanout *out);              /* optional display */
    /* v2 */
    uint32_t flags;                                                 /* QDP_F_PLUGIN_DAMAGE */
    int  (*quiesce)(void *priv);  void (*resume)(void *priv);       /* around guest memory map changes */
    void (*dma_register)(void *priv, const qdp_dma_region *r);      /* guest RAM regions with fd + offset */
    void (*dma_unregister)(void *priv, const qdp_dma_region *r);
    bool (*get_cursor)(void *priv, qdp_cursor *out);                /* optional cursor plane */
    /* v3 */
    int  (*ui_info)(void *priv, const qdp_ui_info *info);           /* optional: a UI asks for a size */
} qdp_device_ops;
```

Version 3 adds only the optional `ui_info()`: Display1 `SetUIInfo` and the
control command `uiinfo` hand it the size a UI asks for (window resized,
fullscreen entered or left) in device pixels, with the physical size when known
(`0` = unknown). It runs on the main loop and returns `0` (accepted),
`QDP_UI_INFO_UNCHANGED` or `-errno`. The plugin decides what the guest is offered;
the scanout follows when the guest presents at the new size. A v1 or v2 plugin
loads as before; `SetUIInfo` is then accepted and ignored.

Version 1:

- `qdp_host_env` carries what the host knows: the EDID of the physical display
  behind the KMS sink (`edid`, `edid_len`), or `NULL`.
- `setup()` runs before `vfu_realize_ctx()`. It sets PCI identity and
  capabilities (`vfu_pci_init`, `vfu_pci_set_id`, `vfu_pci_add_capability`), BARs
  (`vfu_setup_region`, with a memfd and mmap areas for regions the guest should
  access directly), interrupts (`vfu_setup_device_nr_irqs`, `vfu_irq_trigger`)
  and returns its private state in `*priv`.
- The host owns the socket, the attach/detach cycle, the reset callback (chained
  to `reset()`), DMA registration callbacks and the main loop (GLib). Region
  accesses are served by the callbacks the plugin registered, on the main loop.
- `get_scanout()` describes the current framebuffer: a memfd + offset, size,
  stride and pixman format (`0x20020888` = `x8r8g8b8`). `enabled = false` means
  blank. The host maps it, detects damage and feeds the sinks.
- `tick()` is called at the poll rate (the v1 host behaviour, kept for v1
  plugins); `teardown()` on `SIGTERM`/`SIGINT`.
- Rules for a fast device (measured, see below): doorbells as posted MMIO writes;
  no register reads or port I/O on the hot path; status and queues in guest
  memory (shared) with MSI-X completions; never do message-based DMA inside an
  access handler (the client holds its global lock while waiting for the reply,
  the request deadlocks until the 5 s timeout).

Version 2 (GPU plugins) appends to `qdp_device_ops` and `qdp_host_env`; the host
reads the v2 fields of a v2 plugin only, so v1 plugins load unchanged.

- **Guest RAM regions.** `dma_register()` / `dma_unregister()` get a
  `qdp_dma_region {iova, len, vaddr, fd, fd_offset, prot}` for every region the
  client maps (with QEMU and `memory-backend-memfd,share=on`: the RAM sections
  around the PCI hole and the PAM areas, all on the same memfd at their own
  offsets). `vaddr` is the host's mapping, `fd`/`fd_offset` let the plugin map the
  region itself or build a dma-buf (udmabuf). Both stay valid until
  `dma_unregister()` of that region returns; regions of one file share the fd
  (never close it, `dup()` to keep it). Regions without an fd (firmware ROMs:
  message-based) have `vaddr == NULL` and must not be used from plugin threads.
  An identical re-map is not reported twice. A v2 plugin does not call
  `vfu_setup_device_dma()` or `vfu_setup_device_quiesce_cb()`.
- **Quiesce.** `quiesce()` is synchronous: stop every guest memory access and
  every `irq_trigger()`, then return 0 (it may block), or `-errno` to fail the
  client's request. The host calls it before every `DMA_MAP` and `DMA_UNMAP`,
  before a device reset or FLR and before it drops a lost client (so
  `dma_register()`, `dma_unregister()` and `reset()` run quiesced), and calls
  `resume()` once the request has been handled.
- **Lost connection.** Order: `quiesce()`, `reset(VFU_RESET_LOST_CONN)` with the
  guest RAM regions still mapped, `dma_unregister()` of each region, IRQ eventfds
  closed, `resume()`, then the host waits for the next client with the plugin's
  state kept. A device that cannot serve another client returns `QDP_RESET_EXIT`
  from `reset(LOST_CONN)`: the host calls `teardown()` instead of `resume()` and
  exits with status 0.
- **Lifetime.** `teardown()` (on `SIGTERM`/`SIGINT` or after `QDP_RESET_EXIT`) is
  the last call into the plugin. The host never `dlclose()`s a plugin (it is
  opened `RTLD_NODELETE`) and leaves a v2 plugin's teardown with `_exit()`
  after flushing stdio, so threads the plugin cannot join do not race library
  destructors.
- **Scanout.** The host calls `get_scanout()` once right after `setup()` (before
  serving the client), then after each `frame_ready()` batch (or at the poll rate
  without `QDP_F_PLUGIN_DAMAGE`), always on the main loop. It reads only the
  buffer of its latest `get_scanout()` result, and all its reads (inline Display1
  updates, PPM, the KMS copy path) are on the main loop, so it is done with the
  previous buffer when it calls `get_scanout()` again. Not covered: Display1
  listeners that took the memfd with `ScanoutMap` read it asynchronously until the
  next `ScanoutMap`, and the zero-copy KMS sink scans the memfd out until it
  switches; a plugin that reuses an older buffer may show torn frames there. A
  buffer change (another fd or offset) is a mode change for the sinks (unmap/map,
  a new `ScanoutMap` to each listener), so flipping between two buffers costs a
  synchronous D-Bus call per listener and frame.
- **Host services** (`env->host_ops`, callable from any plugin thread between
  `setup()` returning and `teardown()`):
  - `irq_trigger(host, subindex)`: raise MSI/MSI-X vector `subindex` (INTx with 0
    when MSI is off). It writes the client's eventfd directly (KVM irqfd, no QEMU
    thread on the way), serialised only against `SET_IRQS`/reset inside
    libvfio-user, never against the main loop: a plugin thread does not wait for
    an MMIO request being handled, and a region handler or `quiesce()` may block
    on a thread that raises IRQs. The host never holds that lock while it calls
    into the plugin, so a plugin may hold its own mutex across `irq_trigger()`
    and take it in `quiesce()`, `resume()`, `reset()` or the DMA callbacks. Returns `-EINVAL` when the guest has not enabled
    the interrupt (the event is dropped, as on real PCI). Cost: about 1-2 µs per
    call (measured with test-v2, see below).
  - `frame_ready(host, {seq, x, y, w, h})`: a new frame is in the scanout
    (`QDP_F_PLUGIN_DAMAGE`); `w == 0` means all of it. The host wakes its main
    loop through an eventfd once per batch, re-reads `get_scanout()` and feeds the
    sinks. Frames announced faster than the main loop drains them are merged
    (damage united, the last `seq` wins).
  - `cursor_changed(host)`: the host calls `get_cursor()` on its main loop soon
    after. `qdp_cursor.x/y` is the image's top-left corner (pointer minus hotspot).
  - `log(host, level, fmt, ...)`: through the host's stderr, one line per call,
    `qdev-host: <name>: [error: |warning: |debug: ]<text>`; level 3 only with `-v`.
    A plugin may also write to stderr itself: the host does not capture or rewrite
    fd 2, so the plugin's own lines (and their prefixes) pass through unchanged.
- `tick()` runs every `tick_ms` (0 = never) for v2 plugins.

The scanout of a v2 plugin is still a memfd (`get_scanout()`); a GPU plugin copies
its finished frame into it (or renders into memory imported from it) and calls
`frame_ready()`. dma-buf scanouts are on the roadmap.

### Testing plugins without a VM

`plugins/test-v2` is a v2 device without function that uses every v2 callback and
host service and checks the host's rules (see its header comment for the BAR0
registers). `tools/vfu-test-client` is a minimal vfio-user client (its own
transport, only `vfio-user.h`) that maps memfd-backed DMA regions, sets IRQ
eventfds and reads/writes regions:

```bash
make
tools/test-abi-v2.sh /tmp/qdev-test                     # test-v2 + tb-vga, no QEMU at all
tools/test-abi-v2.sh /tmp/qdev-test-v1 /path/to/old/tb-vga.so   # a v1 binary
QEMU=/path/to/qemu-11/build/qemu-system-x86_64 tools/test-v2-qemu.sh /tmp/qdev-qemu
```

- `test-abi-v2.sh`: DMA regions with fd/offset (two regions on one memfd share
  the fd), vaddr == `mmap(fd, fd_offset)`, an identical re-map, a region without
  fd; IRQs from the plugin thread with latency through a DMA record; 200 eventfd
  replacements under IRQ load; 100 unmap/map cycles of the region the IRQ thread
  writes into (quiesce); reset; `reset(LOST_CONN)` with the regions still mapped;
  the initial `get_scanout()` without any `frame_ready()`; `QDP_RESET_EXIT` (host
  exits 0 by itself); Display1 listener (`Scanout`, `Update` per frame,
  `CursorDefine`, `MouseSet`); control socket `screendump` checked against the
  frame seq in the picture; tb-vga config space, EDID/DISPI/QEXT registers, mode
  set, VRAM write and PPM.
- `test-v2-qemu.sh`: a stock QEMU ≥ 11 with the qtest accelerator (no guest code,
  no disk image) as the client: QEMU's memfd RAM through `DMA_MAP`, MMIO through
  QEMU, plugin DMA writes visible in guest RAM, INTx and MSI from the plugin thread
  (MSI aimed at guest RAM so the message write can be read back), PAM map changes
  (quiesce), `system_reset`, and the `--qemu-qmp` input relay checked in QEMU's
  input trace.

Compatibility: `abi_version` is checked at load; new fields are appended.

## The tb-vga plugin

`plugins/tb-vga/`: a stdvga/Bochs-compatible hot-pluggable display, the
out-of-process equivalent of the fork's `thunderbolt-vga`.

- PCI `1234:1111` rev 2, subsystem `1af4:1100`, class `03 00`, PCI Express endpoint.
- BAR0: VRAM (`vram=` MiB, default 16), 64-bit prefetchable, memfd-backed and
  mmap-able (no exits on guest writes).
- BAR2 (4 KiB MMIO, message based): `0x000` EDID blob (`edid=` file, e.g. from
  `qemu-edid -x 1280 -y 800`), `0x400` VGA ports, `0x500` DISPI registers (ID
  `0xB0C5`, XRES/YRES/BPP/ENABLE/VIRT_*/OFFSET/VIDEO_MEMORY_64K), `0x600` QEMU
  extended registers (size, byte order).
- `get_scanout()` reports the DISPI mode (32 bpp only) with the VRAM memfd.
- A client reset clears the registers (as QEMU's stdvga does); a lost connection
  keeps mode and VRAM, so a replug or a QEMU restart finds the picture intact.

Drivers: Linux `bochs-drm` (verified), the same IDs the stock QEMU `VGA` and
`bochs-display` devices use. No legacy VGA ports/0xA0000 window and no option ROM:
the boot display stays a stock QEMU device, as in the fork's examples.

## Thunderbolt hotplug layer on stock QEMU

The fork added three devices; this is how each maps onto stock QEMU
([details, Russian](docs/thunderbolt-on-stock-ru.md)):

| Fork device | Stock QEMU |
|---|---|
| `thunderbolt-root-port` (`_DSD` `PCI-Thunderbolt`, `pci-supports-link-change`; `connected` toggle) | `pcie-root-port` + `acpi/ssdt-tb.dsl` via `-acpitable`; `tools/tbctl.py disconnect/connect` |
| `thunderbolt-pcie-pci-bridge` (`_DSD` `IOPCITunnelled`) | `pcie-pci-bridge` + the same SSDT |
| `thunderbolt-vga` | `qdev-host` + `plugins/tb-vga` |

Bridges cannot leave the QEMU process (`vfio-user-pci` presents endpoints only),
but everything Thunderbolt-specific in them is ACPI data and a presence toggle.
QEMU names PCI devices in ACPI `S<devfn hex>` (`addr=0x05` → `\_SB.PCI0.S28`);
check `/sys/bus/pci/devices/<bdf>/firmware_node/path` and adjust the SSDT scopes.
On macOS the SSDT can also be injected through OpenCore (`ACPI/Add`). On Proxmox VE
the TB bus of `qdev-host-pve` generates this SSDT per VM, with the properties as
`_DSD` and as the `_DSM` macOS reads, for its own ports
([TB bus](pve/README.md#tb-bus-hot-plug-ports-with-thunderbolt-properties)).

## Performance

Measured on this scheme with a microbenchmark (kvm-unit-tests `vmexit` against
`pci-testdev`, native vs. the same device behind `vfio-user-pci`;
[report, Russian](docs/overhead-vfio-user-ru.md), sources in `bench/`):

| Path | In-process device | vfio-user (stock client) |
|---|---:|---:|
| MMIO write (doorbell), posted | 3.1 µs | 4.8–5.0 µs |
| MMIO read | 3.3 µs | 22–26 µs |
| Port I/O read/write | 2.6 µs | 22–26 µs |
| MMIO write when the device has any mmap-able BAR | 3.1 µs | 23 µs (client disables posted writes) |
| Access to a mmap-ed BAR | no exit | no exit |
| DMA 4 KiB, shared guest memory | memcpy | 1.0–1.1 µs |
| DMA 4 KiB, message based | memcpy | 20–27 µs |
| MSI-X | irqfd | same irqfd (by code) |

For a display device none of this matters (a few register writes per mode set, the
framebuffer is shared memory). For a throughput device the rules in
[Plugin ABI](#plugin-abi) keep the overhead at about +1.7 µs per doorbell.

The display path itself (damage polling, KMS copy and flip, input hop) is measured
in [docs/live-display-results.md](docs/live-display-results.md).

## Roadmap

1. **GPU plugins**: ABI v2 (guest RAM with fd, quiesce, thread-safe IRQs, plugin
   damage, cursor) is done; next are dma-buf scanouts (`ScanoutDMABUF`) instead of
   memfd, a KMS cursor plane, and a dedicated vfio-user thread so MMIO is not
   served on the main loop next to D-Bus and the damage comparison.
2. **Several devices per host process** and **plugin replacement**: load v2 next
   to v1, move the guest over, retire v1 (the ABI already keeps state in the
   plugin process).
3. **Orchestration**: `qdevctl` over QMP, Proxmox hookscript (`post-start`) so
   external devices reappear after a VM restart; QSM terminal service: one
   Display1 endpoint per external console.
4. macOS validation of the Thunderbolt layer (`_DSD` vs `_DSM` properties).

## Repository layout

```text
qdev-host.c        host: plugin loader (ABI 1, 2), vfio-user main loop, DMA/quiesce forwarding, host services,
                   damage detection, Display1 server, PPM sink, control socket, input relays
qdp.h              plugin ABI
dbus/              org.qemu.Display1 interface XML (from QEMU, Linux variant); GDBus code generated at build time
plugins/tb-vga/    the display plugin + sample EDID
plugins/test-v2/   ABI v2 test device (DMA regions, IRQ thread, frames, cursor, quiesce checks)
kms-sink.c/.h      DRM/KMS sink (real monitor), EDID export, page flips, hot-plug hooks
tools/             tbctl.py (QMP plug/unplug/connect), run-guest.sh, prep-guest.sh, test-flow.sh,
                   remote-kms-test.sh / remote-features-test.sh (live-display tests on a GPU host),
                   guest_cmd.py (serial console driver), guest-draw.py, guest-animate.py (fps load, raw BAR
                   writes), guest-abs-read.py, ppm2png.py, display1-input-bench.c (input round trip),
                   display1-abs-test.py, display1-clipboard-test.py, pve-ui-test.mjs (headless PVE UI test),
                   vfu-test-client.c (vfio-user client without a VM), test-abi-v2.sh / test-v2-qemu.sh (+ -drive.py),
                   display1-listener-test.py, qdev-control.py (control socket)
acpi/              SSDT with the Thunderbolt ACPI properties (.dsl + .aml)
bench/             the vfio-user overhead microbenchmark (libvfio-user pci-testdev + kvm-unit-tests patch)
docs/              overhead report, Thunderbolt-on-stock notes (Russian), live-display measurements
pve/               qdevctl + qdev_pve.py (operations), qdev_host_api.py (node service), qdev-host-hardware.js +
                   qdev-host.overlay.json (Hardware panel overlay for pve-ui-overlays), hookscript, systemd units
debian/, scripts/  Debian packaging; mkchroot.sh and build-deb-in-chroot.sh build the debs in a trixie chroot
third_party/       vendored libvfio-user (BSD-3-Clause), with the three patches described under Build
```

## License

GPL-2.0-or-later. `dbus/dbus-display1.xml` comes from QEMU (GPL-2.0-or-later).
