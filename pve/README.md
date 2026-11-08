# qdev-host on Proxmox VE (qdev-host-pve)

Adds out-of-process devices (qdev-host plugins) to VMs on a stock Proxmox VE 9 node.
No Proxmox package is patched; the integration uses `qm`, the node's own QEMU
monitor API (`PVE::QemuServer::Monitor::mon_cmd`), the standard guest hookscript
and systemd.

```bash
apt install ./pve-ui-overlays_0.1.0_all.deb ./qdev-host_0.5.0_amd64.deb ./qdev-host-pve_0.5.0_all.deb
qdevctl add 128 tbvga0 --plugin tb-vga --args vram=16      # define + hookscript + root port in args
qm stop 128 && qm start 128                                  # the root port appears at the next start
qdevctl list 128                                             # tbvga0 ... attached
qdevctl set 128 tbvga0 --args vram=32                       # change it (a running VM sees an unplug and a replug)
qdevctl remove 128 tbvga0                                    # hot-unplug, stop the process, forget it
```

What `add` does:

1. writes `/etc/qdev-host/vms/VMID.json` and `/etc/qdev-host/units/VMID-ID.env`;
2. sets the VM's `hookscript` to `local:snippets/qdev-host-hook.pl` (a VM's own
   hookscript is remembered and chained);
3. for the default bus `qdev-rp0` appends a `pcie-root-port` with reserved windows
   (`pref64-reserve=256M,mem-reserve=16M`) to the VM's `args` (root@pam only, like
   any `args` change). `--bus ich9-pcie-port-N` uses one of PVE's own q35 ports
   instead (no restart, but the firmware reserves only small windows there);
4. if the VM is running and the bus exists: starts
   `qdev-host-device@VMID-ID.service` and hot-plugs the device (`device_add
   vfio-user-pci`).

On every VM start the hookscript (post-start) starts the device processes and
hot-plugs them; on post-stop it stops them (`--persistent` keeps a process, and
its VRAM, across VM restarts).

`set` (the panel's *Edit*) changes the plugin arguments, the KMS output, the
Display1 export and persistence of a device; the device ID and plugin stay. A
vfio-user device of a running VM gets a new process and is unplugged and plugged
again; `set` without options applies a device's settings to the VM config again.

## VirGL 3D GPU (`virgl-gpu`)

A built-in plugin backed by QEMU's own out-of-process virtio-gpu, the
`/usr/lib/kvm/vhost-user-gpu` that pve-qemu-kvm ships: VirGL 3D rendering runs in
`qdev-host-vugpu@VMID-ID.service` on a render node of this node, not inside QEMU.
Linux guests use it with their stock `virtio_gpu` driver and Mesa's virgl driver.

```bash
qdevctl add 990 gpu0 --plugin virgl-gpu --args render=/dev/dri/renderD128,vga=1
qm stop 990 && qm start 990     # the GPU is on QEMU's command line from the next start
```

- **From the next VM start, never hot-plugged.** QEMU's D-Bus display exports only
  the consoles that exist when it starts, and only those get its GL context. So
  qdev-host writes `-chardev socket,... -device vhost-user-vga|vhost-user-gpu-pci,...`
  into the VM's `args` (behind its root port), and the hookscript starts the GPU
  process in pre-start, before QEMU connects. The process serves that one QEMU and
  ends with it.
- **Shared guest memory.** A vhost-user device reads guest memory directly, which
  PVE shares only with hugepages or a virtiofs device. Otherwise qdev-host adds
  `-object memory-backend-memfd,id=qdev-mem,size=<memory>M,share=on -machine
  memory-backend=qdev-mem`. Its size must equal the VM's memory: after changing
  the memory, save the GPU again (Edit, or `qdevctl set VMID ID`); the pre-start
  hook stops a start whose sizes differ and says so. VMs with NUMA nodes or memory
  hot-plug need hugepages or virtiofs instead.
- **Primary display** (`vga=1`, the default): `vhost-user-vga`, the VM's first
  display. qdev-host sets the VM's Display to none and takes the place of QSM
  Direct's in-process VirGL GPU (`virtio-vga-gl,id=qsm-direct-gpu`); both come back
  when the GPU is removed. With QSM Direct's Display1 VirGL profile (a D-Bus
  display with `gl=on`, qsm-pve-direct 0.4.0+stock4.14~exp233 or later) the QSM console shows it. `vga=0` adds a `vhost-user-gpu-pci` next to the VM's display instead.
- **Picture.** The GPU hands its frames to QEMU as dma-bufs, so only a GL display
  shows them. Without one the guest still renders, and the panel says so.
- **NVIDIA and AMD.** Checked on an AMD Raphael (amdgpu) and an RTX 3080 with the
  proprietary driver 595.91 (its GBM backend): a Kubuntu guest's KWin and SDDM
  render through virgl on the host GPU, where `nvidia-smi` lists the
  `vhost-user-gpu` process. If the GPU process dies while the VM runs, QEMU cannot
  reconnect and logs `failed to read msg header` until the VM stops; stop the VM.
- **Not with the native shared display** of the older QSM preview (`dbus-display: 1`):
  move the VM to QSM Display1 VirGL (Hardware > Display) and its sound to QSM
  Console first. One device per PCIe root port: a GPU and a tb-vga need two buses.
- Settings: `render` (default the first render node), `vga`, `outputs` (1-4),
  `xres`, `yres`. Removing the GPU from a running VM changes its config only; the
  VM keeps the GPU until it stops.

Verified on PVE 9.2.21 with pve-qemu-kvm 11.0.3 and an AMD Raphael render node:
a Debian 13 guest found `virtio-vga` with `+virgl +edid` and two virgl capability
sets, and the QSM Direct console showed the GPU's output (the guest's console at
1024x768, captured as dma-buf, encoded with VA-API). Adding and removing the GPU
gave back the VM's config byte for byte. A 3D application in the guest was not
run (the test guest has no Mesa).

Sockets: `/run/qdev-host/VMID-ID/vfio.sock` (vfio-user) and `display.sock`
(org.qemu.Display1 p2p). When QSM Direct runs the VM's Display1 bus
(`/run/qsm-pve-direct/VMID/qemu-display1.bus`), the device relays keyboard,
mouse, clipboard and audio to it automatically. A real monitor: `--kms
/dev/dri/cardN:CONNECTOR` (the node must not run a compositor on that card).

Logs: `journalctl -u qdev-host-device@VMID-ID`. Requirements: q35 machine type,
`pve-qemu-kvm` ≥ 10.1 (vfio-user-pci), a storage with `snippets` content.

## Web UI (Hardware panel)

Installing `qdev-host-pve` adds qdev-host devices to the VM *Hardware* panel of
the stock PVE 9 web UI: one row per device with its live state, *Add > qdev-host
Device* and *Remove*. Two parts, modelled on QSM Direct's PVE integration:

1. **Overlay**: `/usr/share/pve-manager/js/qdev-host-hardware.js`, an ExtJS
   override of `PVE.qemu.HardwareView`, described by
   `/usr/share/pve-ui-overlays/overlays.d/qdev-host.json`. The shared package
   [pve-ui-overlays](https://github.com/rdmitry0911/pve-ui-overlays) (a
   dependency) owns the single `dpkg-divert` of `index.html.tpl` and renders
   this overlay together with any other (QSM Direct's console entry, ...),
   only for pve-manager versions in its reviewed allow-list; after an unknown
   PVE upgrade the stock page comes back until a row is added
   (`pve-ui-overlays template-hash` prints it). Install order does not
   matter. `pve-ui-overlays status|list|disable qdev-host|enable qdev-host`.
2. **Node service**: `qdev-host-api.service` (`/usr/lib/qdev-host/pve/qdev_host_api.py`),
   HTTPS on port 8017 with the node's own PVE certificate. The overlay sends the
   browser's PVE ticket (`Authorization: Bearer`) and user name (`X-QDev-User`);
   the service relays them to the node's `/api2/json/access/ticket` with
   `path=/vms/VMID` and `privs=VM.Config.HWType`, then checks the confirmed user
   against the allow-list. Nothing is registered in pveproxy or pvedaemon.

   **Behind QSM Direct's TCP 8006 gateway** (qsm-pve-direct-gateway from
   0.4.0+stock4.14~exp230, enabled), the package's route file
   `/usr/share/qsm-pve-direct-gateway/routes.d/qdev-host.json` publishes the
   service as `https://NODE:8006/qdev-host/` (to `127.0.0.1:<port from ui.json>`),
   applied by a dpkg trigger without a pveproxy restart. The panel sees the
   gateway in `window.QSM_DIRECT_ENDPOINT` and uses that same-origin path, so a
   browser that reaches only the forwarded port 8006 needs neither port 8017 nor
   a certificate exception. When the reply does not come from the service (an
   older gateway without the route), it falls back to port 8017.
   `qsm-pve-direct-gateway routes` lists what is published.

   | Method | Path | Action |
   |---|---|---|
   | GET | `/api/v1/vms/VMID` | devices with state, plugins (+ argument schema), KMS outputs |
   | POST | `/api/v1/vms/VMID/devices` | `{id, plugin, args, kms, display, persistent}` = `qdevctl add` |
   | DELETE | `/api/v1/vms/VMID/devices/ID[?purge_port=1]` | = `qdevctl remove` |
   | POST | `/api/v1/vms/VMID/devices/ID/attach` / `detach` | = `qdevctl start` / `stop` |

Configuration, optional: `/etc/qdev-host/ui.json`

```json
{"port": 8017, "listen": "", "users": ["root@pam"], "privilege": "VM.Config.HWType"}
```

`users` defaults to `root@pam` on purpose: adding a device appends a root port to
the VM's `args` and starts a root process with plugin arguments, both things PVE
itself allows only root to do. `"users": ["*"]` leaves only the privilege check.
Change the port here and run `pve-ui-overlays reconcile` (the port is published
to the page as `window.QDEV_HOST_ENDPOINT` through the descriptor's `globals`).

Plugins describe their arguments for the dialog in a sidecar next to the `.so`
(`/usr/lib/qdev-host/plugins/tb-vga.json`: `args: [{name, label, type:
int|path|string|bool, default, min, max, hint}]`); a plugin without one gets no
argument fields.

Troubleshooting:

- The row says *node service unreachable*: the browser has not accepted the
  node certificate on port 8017 yet; the row links to `https://NODE:8017/`,
  open it once, then reload the UI. Also check `systemctl status qdev-host-api`.
  On a node behind the QSM gateway the browser may not reach 8017 at all:
  check `qsm-pve-direct-gateway routes` (empty = the gateway is older than
  exp230 or the route was not applied: `qsm-pve-direct-gateway refresh`).
- No qdev-host entries at all: `pve-ui-overlays status`;
  `disabled-unsupported-pve` after a PVE upgrade means the new `pve-manager`
  is not in the allow-list yet (review the template, add the row printed by
  `pve-ui-overlays template-hash` to `/etc/pve-ui-overlays/manifest.d/local.sha256`,
  `pve-ui-overlays reconcile`). `skipped qdev-host: missing ...` means the
  node service files are absent.
- *not allowed to manage qdev-host devices*: the user is not in `ui.json`.
- The VM lives on another node: open the web UI of that node (the service only
  manages local VMs; a cluster-wide relay is on the list).
- Actions are logged to `journalctl -u qdev-host-api` with the confirmed user.

Headless check (needs `npm install playwright` somewhere and a Chromium; the
ticket is minted on the node, so no password is typed anywhere):

```bash
PVE_HOST=172.16.16.17 VMID=990 \
PVE_TICKET="$(ssh root@172.16.16.17 "perl -e 'use PVE::AccessControl; print PVE::AccessControl::assemble_ticket(q{root@pam})'")" \
PLAYWRIGHT=/path/node_modules/playwright/index.mjs CHROME=/path/to/chrome \
node tools/pve-ui-test.mjs --shots /tmp/ui-shots        # --dry: open panel, menu and dialog only
```

It logs in with the ticket as the password (that is what stores the capability
map the Hardware panel needs), opens *Hardware*, adds `uitest0` (tb-vga) through
the dialog, waits for `attached (…)`, removes it and saves screenshots.
