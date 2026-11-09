# qdev-host on Proxmox VE (qdev-host-pve)

Adds out-of-process devices (qdev-host plugins) to VMs on a stock Proxmox VE 9 node.
No Proxmox package is patched; the integration uses `qm`, the node's own QEMU
monitor API (`PVE::QemuServer::Monitor::mon_cmd`), the standard guest hookscript
and systemd.

```bash
apt install ./pve-ui-overlays_0.1.0_all.deb ./qdev-host_0.5.0_amd64.deb ./qdev-host-pve_0.5.0_all.deb
qdevctl add 128 tbvga0 --plugin tb-vga --args vram=16      # define + hookscript + TB bus in args
qm stop 128 && qm start 128                                  # the TB bus appears at the next start
qdevctl list 128                                             # tbvga0 ... attached
qdevctl set 128 tbvga0 --args vram=32                       # change it (a running VM sees an unplug and a replug)
qdevctl remove 128 tbvga0                                    # hot-unplug, stop the process, forget it
```

What `add` does:

1. writes `/etc/qdev-host/vms/VMID.json` and `/etc/qdev-host/units/VMID-ID.env`;
2. sets the VM's `hookscript` to `local:snippets/qdev-host-hook.pl` (a VM's own
   hookscript is remembered and chained);
3. puts the device on a free port of the VM's [TB bus](#tb-bus-hot-plug-ports-with-thunderbolt-properties)
   (from 0.6.0), whose two option groups go in front of the VM's `args` with the
   first device (root@pam only, like any `args` change). Devices of earlier
   versions keep their own root port `qdev-rp0` (0x08,
   `pref64-reserve=256M,mem-reserve=16M`), which `--bus qdev-rp0` still adds;
   `--bus ich9-pcie-port-N` uses one of PVE's own q35 ports instead (no restart,
   but the firmware reserves only small windows there);
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
qdevctl add 990 gpu0 --plugin virgl-gpu --args render=/dev/dri/renderD128,display=primary
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
- **Display** (`display=`):
  - `primary` (the default from 0.5.5): `vhost-user-gpu-pci` is the VM's only display.
    qdev-host sets the VM's Display to none and takes the place of QSM Direct's in-process
    VirGL GPU (`virtio-vga-gl,id=qsm-direct-gpu`); both come back when the GPU is removed.
    With QSM Direct's Display1 VirGL profile (a D-Bus display with `gl=on`) the QSM console
    shows it. It has no VGA part, so QEMU 11's crash on a guest reboot (below) cannot happen.
    The firmware screen needs OVMF, whose virtio-gpu driver draws it; with SeaBIOS nothing
    shows until the guest's driver starts.
  - `vga`: the same with a VGA part (`vhost-user-vga`), for guests or firmware that need a
    VGA adapter. QEMU 11 crashes when such a guest reboots while a console is open.
  - `secondary`: a `vhost-user-gpu-pci` next to the VM's own display, which stays the one
    the console shows.

  Devices saved before 0.5.5 keep their mode: `vga=1` is `vga`, `vga=0` is `secondary`.
- **Picture.** The GPU hands its frames to QEMU as dma-bufs, so only a GL display
  shows them. Without one the guest still renders, and the panel says so.
- **NVIDIA and AMD.** Checked on an AMD Raphael (amdgpu) and an RTX 3080 with the
  proprietary driver 595.91 (its GBM backend): a Kubuntu guest's KWin and SDDM
  render through virgl on the host GPU, where `nvidia-smi` lists the
  `vhost-user-gpu` process.
- **Two faults of vhost-user-gpu, worked around** (from 0.5.4) by
  `/usr/lib/qdev-host/libqdev-vugpu-guard.so`, which the unit preloads into the
  process (`pve/vugpu-guard.c`). It uses only virglrenderer's public callbacks and
  the vhost-user-gpu protocol; QEMU and its binary are unchanged.
  - *Crash on a fence during an EDID or display info reply.* vhost-user-gpu keeps
    these requests on its fence queue while QEMU answers, and a virgl fence
    signalled meanwhile completes and frees them; the reply then finds an empty
    queue (`segfault at 28` in `get_edid_cb`). Seen with a Plasma guest while a
    console resized its display. The library holds fence notifications until the
    reply has been handled.
  - *GPU socket refused after a pause or a device reset.* QEMU closes its end of
    the GPU socket whenever it stops the device (`qm suspend`, a backup in suspend
    mode, a snapshot with RAM, a reboot inside the guest) and sends a new one when
    it starts it again; vhost-user-gpu refuses it (`assertion 'g->sock_fd == -1'
    failed`), the display stops, and a later stop ends the process. The library
    puts the new socket under the old descriptor and answers for QEMU while its end
    is gone. vhost-user-gpu's own assertion message still appears; the library
    logs `qdev-host vugpu guard: ...` lines next to it.

  The process picks the library up when the VM next starts. Not covered: commands
  still running when the guest resets the device, and the contexts and resources
  virglrenderer keeps from the guest's previous boot; vhost-user-gpu clears
  neither. A 3D desktop after a reboot inside the guest has not been checked yet;
  `qm reboot` (or Shutdown and Start) starts a fresh process.
- **If the GPU process dies anyway**, QEMU cannot reconnect and logs `failed to
  read msg header` many thousand times a minute until the VM stops: stop the VM.
  journald keeps its size limit, but the flood pushes older entries out.
- **Not with the native shared display** of the older QSM preview (`dbus-display: 1`):
  move the VM to QSM Display1 VirGL (Hardware > Display) and its sound to QSM
  Console first. One device per PCIe root port: a GPU and a tb-vga need two buses.
- Settings: `render` (default the first render node), `display`, `outputs` (1-4),
  `xres`, `yres`. Removing the GPU from a running VM changes its config only; the
  VM keeps the GPU until it stops.

Verified on PVE 9.2.21 with pve-qemu-kvm 11.0.3 and an AMD Raphael render node:
a Debian 13 guest found `virtio-vga` with `+virgl +edid` and two virgl capability
sets, and the QSM Direct console showed the GPU's output (the guest's console at
1024x768, captured as dma-buf, encoded with VA-API). Adding and removing the GPU
gave back the VM's config byte for byte. A 3D application in the guest was not
run (the test guest has no Mesa).

The guard (0.5.4), on the same node and guest: with a test fence reported during
every EDID reply (`QDEV_VUGPU_TEST_FENCE_IN_REPLY=edid`), the stock process
crashed at the same address as on the Plasma VM, and with the guard it held the
fence and the guest got its 256-byte EDID. Two `qm suspend`/`qm resume` cycles, a
driver rebind and two reboots inside the guest each replaced the GPU socket; the
guest initialized its GPU at once with the EDID and logged no errors. Without the
library each of these lost the display or ended the process. Test switches:
`QDEV_VUGPU_FENCE_HOLD=0`, `QDEV_VUGPU_SOCKET_GUARD=0`,
`QDEV_VUGPU_TEST_FENCE_IN_REPLY=1|edid|info` (environment of the unit).

Sockets: `/run/qdev-host/VMID-ID/vfio.sock` (vfio-user) and `display.sock`
(org.qemu.Display1 p2p). When QSM Direct runs the VM's Display1 bus
(`/run/qsm-pve-direct/VMID/qemu-display1.bus`), the device relays keyboard,
mouse, clipboard and audio to it automatically. A real monitor: `--kms
/dev/dri/cardN:CONNECTOR` (the node must not run a compositor on that card).

Logs: `journalctl -u qdev-host-device@VMID-ID`. Requirements: q35 machine type,
`pve-qemu-kvm` ≥ 10.1 (vfio-user-pci), a storage with `snippets` content.

## TB bus (hot-plug ports with Thunderbolt properties)

A VM can get a **TB bus**: hot-plug PCIe root ports `qdev-tb0..3` (`pcie.0` slot
0x09, functions 0..3) whose ACPI nodes carry the Thunderbolt properties of the
qemu-thunderbolt fork (`PCI-Thunderbolt`, `pci-supports-link-change`), as `_DSD`
device properties and as the `_DSM` macOS reads device properties from.
qdev-host's own devices take its ports (from 0.6.0): a plugin device such as
`tb-vga` is hot-plugged there, the `virgl-gpu` GPU sits on one from the VM
start (its `-device` stays in `args`, after the bus, where QSM Direct finds it).
QEMU's own devices are put on it too and can be added to and removed from the
running VM:

| Plugin | Device | Arguments (PVE's own syntax) | Added in the web UI |
|---|---|---|---|
| `tb-pci` | PCI pass-through (`vfio-pci`), all functions of a device when the function is left out | `host=0000:06:00.0` or `mapping=NAME`, `rombar`, `romfile`, `vendor-id`... | *Add > PCI Device*, Bus *TB bus* |
| `tb-net` | network card on a tap of a node bridge | `virtio[=MAC],bridge=vmbr0[,tag=N][,rate=MB/s][,mtu=N][,queues=N]` | *Add > Network Device*, Bus *TB bus* |
| `tb-xhci` | QEMU's USB 3 controller (`qemu-xhci`) | `ports=N` (USB 2 and USB 3 ports each, default 8) | *Add > qdev-host Device* |
| `tb-usb` | USB device of the node on a `tb-xhci` controller (its bus) | `host=VENDOR:PRODUCT`, `host=BUS-PORT` or `mapping=NAME` | *Add > USB Device*, Bus *TB bus, USB controller ...* |

```bash
qdevctl add 990 tbnet0 --plugin tb-net --args 'virtio,bridge=vmbr0'          # MAC from the datacenter prefix
qdevctl add 990 tbxhci0 --plugin tb-xhci
qdevctl add 990 tbusb0 --plugin tb-usb --args host=1a86:7523 --bus tbxhci0    # hot-plugged when the VM runs
qdevctl add 990 tbpci0 --plugin tb-pci --args host=0000:08:00.3               # bound to vfio-pci like a hostpciN
qdevctl stop 990 tbusb0; qdevctl start 990 tbusb0                              # unplug / plug again
qdevctl tb 990 --ports 6                                                       # more ports, from the next VM start
qdevctl tb 990 --acpi hotplug                                                  # ACPI properties: thunderbolt|hotplug|none
qdevctl status 990
```

In the web UI the dialogs of PVE's own *PCI Device*, *Network Device* and *USB
Device* get a **Bus** field: *Standard (PVE)* keeps PVE's behaviour, *TB bus*
hands the device to qdev-host. The USB dialog lists the VM's TB USB
controllers, or offers a new one (created with the device). TB devices are rows
of the Hardware panel (`Network Device (tbnet0)`, value `TB port qdev-tb1, ...`);
*Edit* opens the same PVE dialog filled with the device's settings, *Remove*
unplugs it.

How it works:

- The VM's `args` get `-readconfig /var/lib/qdev-host/tb/VMID.cfg -acpitable
  file=/var/lib/qdev-host/tb/VMID.aml` once (the first TB device; if the VM
  runs, the bus exists from its next start). The hookscript writes both files
  again at every VM start (pre-start): the root ports and the devices present at
  the start go to `VMID.cfg`, the SSDT for the ports (and for `qdev-rp0` when it
  is in the args) to `VMID.aml`, built by qdev-host's own small AML encoder
  (PCI pass-through devices excepted, see below).
  So adding or removing a device never changes `args` again.
- While the VM runs, devices are added with QMP `device_add`/`netdev_add` through
  the node's `mon_cmd`, removed with `device_del` (the guest releases a PCIe
  device in about five seconds) and `netdev_del`.
- PCI devices are resolved, bound to `vfio-pci` and reserved for the VM with
  qemu-server's own functions (`parse_hostpci`, `prepare_pci_device`,
  `reserve_pci_usage`), so a device another running VM uses is refused, and a
  mapping works as for `hostpciN`. Mediated devices, *Primary GPU* (`x-vga`)
  and `legacy-igd` need the standard bus. They are not in `VMID.cfg`: the
  hookscript hot-plugs them right after the start (post-start), because QEMU
  pins all of the VM's memory for the first one before it daemonizes and
  qemu-server lengthens its start timeout only for its own `hostpciN` (a 32 GiB
  VM was killed after 32 s; hot-plugged, the pinning took about 100 s inside
  `qm start`'s hookscript step).
- QEMU 11 aborts when a vfio-pci device connects while a vfio-user device
  (`tb-vga`, ...) is attached: `hw/vfio/container-legacy.c` casts every VFIO
  container of the address space to a legacy one. qdev-host therefore plugs PCI
  devices before its vfio-user devices at the start and refuses to hot-plug one
  while a vfio-user device is attached (it is then present from the next start).
- A network device's tap is `tapVMIDi32` and up (PVE's own NICs are 0..31), so
  the VM's traffic counters include it. QEMU's `script` is
  `/usr/lib/qdev-host/qdev-tb-ifup`, which plugs the tap into the bridge with
  qemu-server's `tap_plug` (VLAN tag, rate) from the settings in
  `/run/qdev-host/tb-net/IFACE.json`. The PVE firewall does not filter these
  devices (its rules come from the VM's `netN` entries; the dialog's Firewall
  box is off on the TB bus). Models: `virtio`, `e1000e`, `vmxnet3`: the ports
  have no I/O window (`io-reserve=0`), the q35 I/O space is too small for more
  bridges with one, and `e1000`/`rtl8139` need port I/O.
- Ports reserve 32 MiB of 32-bit and 1 GiB of 64-bit prefetchable memory each
  (`mem-reserve=32M,pref64-reserve=1G`), enough for a NIC, a USB controller or a
  GPU with a 256 MiB BAR; `qdevctl tb VMID --io-reserve/--mem-reserve/--pref64-reserve
  SIZE` changes them for a VM (`''` = QEMU's default), from its next start.
- `qdevctl tb VMID --acpi MODE` chooses the ports' ACPI properties: `thunderbolt`
  (default, the fork's `PCI-Thunderbolt` + `pci-supports-link-change`),
  `hotplug` (`PCIHotplugCapable` + `pci-supports-link-change`, macOS's native
  hot-plug port) or `none`.
- `qdevctl remove ... --purge-port` of the VM's last qdev-host device also
  removes the TB bus from `args` and deletes its two files.

Checked on PVE 9.2.21 (pve-qemu-kvm 11.0.3) with a Debian 13 guest: the ports
at `00:09.0-3` with ACPI paths `\_SB.PCI0.S48..S4B`, the SSDT loaded without
ACPI errors; at VM start and hot-plugged into the running VM: virtio and
e1000e NICs on a bridge, the xHCI controller with a USB serial adapter of the
node (the node gets it back on unplug), unplug and replug of the controller
with its device; on a second node an AMD USB 3 controller passed through
(cold, hot-unplug, hot-plug, PVE's reservation following). The web UI flow
(Add, Edit, Remove in the PCI, Network and USB dialogs) passes the headless
test `tools/pve-tb-ui-test.mjs`.

macOS (a Sequoia-era guest with OpenCore on PVE 9.2.21, machine q35): macOS reads
the properties (with `thunderbolt` it spreads the ports' bus numbers 14/72/134/195
as for Thunderbolt), but it configured no device behind any PCIe root port of
that VM: not on the TB ports in any of the three modes, with or without an I/O
window, and not on PVE's own `ich9-pcie-port-2` either (a control xHCI), while
everything behind PVE's conventional `pci.N` bridges works. It also ignored hot-plug
and the unplug requests on the TB ports. So that guest can use the TB bus only once
it configures root-port devices at all; another macOS VM (Ventura, machine
pc-q35-8.1, PCI pass-through on PVE's ports) does, and is the next one to try.

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
   a certificate exception. Only when pveproxy itself answers that path (an
   older gateway without the route) does the panel use port 8017 for the rest
   of the page's life. A failed or timed-out request, or an error page of
   HAProxy, is retried once and reported in the row; the next refresh uses the
   gateway again (before 0.5.2 one such failure moved the page to port 8017,
   where the browser had often not accepted the certificate). The service
   marks its answers with `X-QDev-Host: 1`; the panel logs failed requests to
   the browser console (`qdev-host: ...`). `qsm-pve-direct-gateway routes`
   lists what is published. Through the gateway the requests go with the
   page's credentials mode (from 0.5.3), on the page's own connections: with a
   page opened by IP address and the certificate warning accepted, Chrome
   rejected the certificate on separate connections without credentials, and
   the row said "Failed to fetch". The service ignores the PVE cookie that now
   comes along and authorises only the `Authorization` header.

   The panel's GET reads the VM config from `/etc/pve/qemu-server/VMID.conf`
   (pending changes applied, as `qm config` prints it) and the running state
   from the pid file, as qemu-server does; changes still go through `qm set`.

   | Method | Path | Action |
   |---|---|---|
   | GET | `/api/v1/vms/VMID` | devices with state, plugins (+ argument schema), KMS outputs |
   | POST | `/api/v1/vms/VMID/devices` | `{id, plugin, args, bus, kms, display, persistent}` = `qdevctl add` |
   | PUT | `/api/v1/vms/VMID/devices/ID` | `{args, kms, display, persistent}` = `qdevctl set` |
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

- The row says *node service did not answer at https://NODE:8017*: the
  browser has not accepted the node certificate on port 8017 yet; the row
  links to `https://NODE:8017/`, open it once, then reload the UI. Also check
  `systemctl status qdev-host-api`.
- The row says *did not answer through the TCP 8006 gateway*: the page's own
  certificate is used, nothing to accept. Check `systemctl status
  qdev-host-api` and `qsm-pve-direct-gateway routes`; the browser console
  shows the failed request. If the panel moved to port 8017 on a gateway
  node, `qsm-pve-direct-gateway routes` is probably empty (the gateway is
  older than exp230 or the route was not applied: `qsm-pve-direct-gateway
  refresh`).
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
