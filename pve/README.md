# qdev-host on Proxmox VE (qdev-host-pve)

Adds out-of-process devices (qdev-host plugins) to VMs on a stock Proxmox VE 9 node.
No Proxmox package is patched; the integration uses `qm`, the node's own QEMU
monitor API (`PVE::QemuServer::Monitor::mon_cmd`), the standard guest hookscript
and systemd.

```bash
apt install ./qdev-host_0.2.0_amd64.deb ./qdev-host-pve_0.2.0_all.deb
qdevctl add 128 tbvga0 --plugin tb-vga --args vram=16      # define + hookscript + root port in args
qm stop 128 && qm start 128                                  # the root port appears at the next start
qdevctl list 128                                             # tbvga0 ... attached
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
   override of `PVE.qemu.HardwareView`. `qdev-host-ui` (`install | reconcile |
   remove | status`) adds its script tag to `/usr/share/pve-manager/index.html.tpl`
   through a package-owned `dpkg-divert`, but only when the installed
   `pve-manager` version *and* the template's SHA-256 are listed in
   `/usr/share/qdev-host/pve/pve-manager-index-template.sha256`. After a
   pve-manager upgrade a dpkg trigger and `qdev-host-ui-reconcile.path` re-run
   `reconcile`: a reviewed version is re-patched, an unknown one gets the stock
   template back and the panel entry disappears until a new row is shipped.
   When `qsm-pve-direct` already diverts the template, the block is inserted into
   its rendered copy instead (and re-inserted by the path unit whenever qsm
   re-renders); when qsm is installed later, `qdev-host-ui` lets it take the
   diversion first. Either package can be removed independently.
2. **Node service**: `qdev-host-api.service` (`/usr/lib/qdev-host/pve/qdev_host_api.py`),
   HTTPS on port 8017 with the node's own PVE certificate. The overlay sends the
   browser's PVE ticket (`Authorization: Bearer`) and user name (`X-QDev-User`);
   the service relays them to the node's `/api2/json/access/ticket` with
   `path=/vms/VMID` and `privs=VM.Config.HWType`, then checks the confirmed user
   against the allow-list. Nothing is registered in pveproxy or pvedaemon.

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
Change the port here and run `qdev-host-ui reconcile` (the port is written into
the page as `window.QDEV_HOST_ENDPOINT`).

Plugins describe their arguments for the dialog in a sidecar next to the `.so`
(`/usr/lib/qdev-host/plugins/tb-vga.json`: `args: [{name, label, type:
int|path|string|bool, default, min, max, hint}]`); a plugin without one gets no
argument fields.

Troubleshooting:

- The row says *node service unreachable*: the browser has not accepted the
  node certificate on port 8017 yet; the row links to `https://NODE:8017/`,
  open it once, then reload the UI. Also check `systemctl status qdev-host-api`.
- No qdev-host entries at all: `qdev-host-ui status`; `inactive` after a PVE
  upgrade means the new `pve-manager` is not in the allow-list yet (check the
  anchor line, add `VERSION<TAB>SHA256` to the manifest, `qdev-host-ui reconcile`).
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
